/*
 * Copyright (C) 2014 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "log.hh"
#include "database.hh"
#include "unimplemented.hh"
#include "core/future-util.hh"
#include "db/commitlog/commitlog_entry.hh"
#include "db/system_keyspace.hh"
#include "db/consistency_level.hh"
#include "db/commitlog/commitlog.hh"
#include "db/config.hh"
#include "to_string.hh"
#include "query-result-writer.hh"
#include "nway_merger.hh"
#include "cql3/column_identifier.hh"
#include "core/seastar.hh"
#include <seastar/core/sleep.hh>
#include <seastar/core/rwlock.hh>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include "sstables/sstables.hh"
#include "sstables/compaction.hh"
#include "sstables/remove.hh"
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/adaptor/map.hpp>
#include "locator/simple_snitch.hh"
#include <boost/algorithm/cxx11/all_of.hpp>
#include <boost/algorithm/cxx11/any_of.hpp>
#include <boost/function_output_iterator.hpp>
#include <boost/range/algorithm/heap_algorithm.hpp>
#include <boost/range/algorithm/remove_if.hpp>
#include <boost/range/algorithm/find.hpp>
#include <boost/range/algorithm/find_if.hpp>
#include <boost/range/adaptor/map.hpp>
#include "frozen_mutation.hh"
#include "mutation_partition_applier.hh"
#include "core/do_with.hh"
#include "service/migration_manager.hh"
#include "service/storage_service.hh"
#include "mutation_query.hh"
#include "sstable_mutation_readers.hh"
#include <core/fstream.hh>
#include <seastar/core/enum.hh>
#include "utils/latency.hh"
#include "utils/flush_queue.hh"
#include "schema_registry.hh"
#include "service/priority_manager.hh"

#include "checked-file-impl.hh"
#include "disk-error-handler.hh"

using namespace std::chrono_literals;

logging::logger dblog("database");

// Slight extension to the flush_queue type.
class column_family::memtable_flush_queue : public utils::flush_queue<db::replay_position> {
public:
    template<typename Func, typename Post>
    auto run_cf_flush(db::replay_position rp, Func&& func, Post&& post) {
        // special case: empty rp, yet still data.
        // We generate a few memtables with no valid, "high_rp", yet
        // still containing data -> actual flush.
        // And to make matters worse, we can initiate a flush of N such
        // tables at the same time.
        // Just queue them at the end of the queue and treat them as such.
        if (rp == db::replay_position() && !empty()) {
            rp = highest_key();
        }
        return run_with_ordered_post_op(rp, std::forward<Func>(func), std::forward<Post>(post));
    }
};

// Used for tests where the CF exists without a database object. We need to pass a valid
// dirty_memory manager in that case.
thread_local memtable_dirty_memory_manager default_dirty_memory_manager;

lw_shared_ptr<memtable_list>
column_family::make_memory_only_memtable_list() {
    auto seal = [this] (memtable_list::flush_behavior ignored) { return make_ready_future<>(); };
    auto get_schema = [this] { return schema(); };
    return make_lw_shared<memtable_list>(std::move(seal), std::move(get_schema), _config.dirty_memory_manager);
}

lw_shared_ptr<memtable_list>
column_family::make_memtable_list() {
    auto seal = [this] (memtable_list::flush_behavior behavior) { return seal_active_memtable(behavior); };
    auto get_schema = [this] { return schema(); };
    return make_lw_shared<memtable_list>(std::move(seal), std::move(get_schema), _config.dirty_memory_manager);
}

lw_shared_ptr<memtable_list>
column_family::make_streaming_memtable_list() {
    auto seal = [this] (memtable_list::flush_behavior behavior) { return seal_active_streaming_memtable(behavior); };
    auto get_schema =  [this] { return schema(); };
    return make_lw_shared<memtable_list>(std::move(seal), std::move(get_schema), _config.streaming_dirty_memory_manager);
}

lw_shared_ptr<memtable_list>
column_family::make_streaming_memtable_big_list(streaming_memtable_big& smb) {
    auto seal = [this, &smb] (memtable_list::flush_behavior) { return seal_active_streaming_memtable_big(smb); };
    auto get_schema =  [this] { return schema(); };
    return make_lw_shared<memtable_list>(std::move(seal), std::move(get_schema), _config.streaming_dirty_memory_manager);
}

column_family::column_family(schema_ptr schema, config config, db::commitlog* cl, compaction_manager& compaction_manager)
    : _schema(std::move(schema))
    , _config(std::move(config))
    , _memtables(_config.enable_disk_writes ? make_memtable_list() : make_memory_only_memtable_list())
    , _streaming_memtables(_config.enable_disk_writes ? make_streaming_memtable_list() : make_memory_only_memtable_list())
    , _compaction_strategy(make_compaction_strategy(_schema->compaction_strategy(), _schema->compaction_strategy_options()))
    , _sstables(make_lw_shared(_compaction_strategy.make_sstable_set(_schema)))
    , _cache(_schema, sstables_as_mutation_source(), global_cache_tracker(), _config.max_cached_partition_size_in_bytes)
    , _commitlog(cl)
    , _compaction_manager(compaction_manager)
    , _flush_queue(std::make_unique<memtable_flush_queue>())
{
    if (!_config.enable_disk_writes) {
        dblog.warn("Writes disabled, column family no durable.");
    }
}

partition_presence_checker
column_family::make_partition_presence_checker(sstables::shared_sstable exclude_sstable) {
    return [this, exclude_sstable = std::move(exclude_sstable)] (partition_key_view key) {
        auto exclude = [e = std::move(exclude_sstable)] (auto s) { return s != e; };
        for (auto&& s : *_sstables->all() | boost::adaptors::filtered(exclude)) {
            if (s->filter_has_key(*_schema, key)) {
                return partition_presence_checker_result::maybe_exists;
            }
        }
        return partition_presence_checker_result::definitely_doesnt_exist;
    };
}

mutation_source
column_family::sstables_as_mutation_source() {
    return mutation_source([this] (schema_ptr s,
                                   const query::partition_range& r,
                                   const query::partition_slice& slice,
                                   const io_priority_class& pc,
                                   tracing::trace_state_ptr trace_state) {
        return make_sstable_reader(std::move(s), r, slice, pc, std::move(trace_state));
    });
}

// define in .cc, since sstable is forward-declared in .hh
column_family::~column_family() {
}


logalloc::occupancy_stats column_family::occupancy() const {
    logalloc::occupancy_stats res;
    for (auto m : *_memtables) {
        res += m->region().occupancy();
    }
    for (auto m : *_streaming_memtables) {
        res += m->region().occupancy();
    }
    for (auto smb : _streaming_memtables_big) {
        for (auto m : *smb.second->memtables) {
            res += m->region().occupancy();
        }
    }
    return res;
}

static
bool belongs_to_current_shard(const streamed_mutation& m) {
    return dht::shard_of(m.decorated_key().token()) == engine().cpu_id();
}

// Stores ranges for all components of the same clustering key, index 0 referring to component
// range 0, and so on.
using ck_filter_clustering_key_components = std::vector<nonwrapping_range<bytes_view>>;
// Stores an entry for each clustering key range specified by the filter.
using ck_filter_clustering_key_ranges = std::vector<ck_filter_clustering_key_components>;

// Used to split a clustering key range into a range for each component.
// If a range in ck_filtering_all_ranges is composite, a range will be created
// for each component. If it's not composite, a single range is created.
// This split is needed to check for overlap in each component individually.
static ck_filter_clustering_key_ranges
ranges_for_clustering_key_filter(const schema_ptr& schema, const query::clustering_row_ranges& ck_filtering_all_ranges) {
    ck_filter_clustering_key_ranges ranges;

    for (auto& r : ck_filtering_all_ranges) {
        // this vector stores a range for each component of a key, only one if not composite.
        ck_filter_clustering_key_components composite_ranges;

        if (r.is_full()) {
            ranges.push_back({ nonwrapping_range<bytes_view>::make_open_ended_both_sides() });
            continue;
        }
        auto start = r.start() ? r.start()->value().components() : clustering_key_prefix::make_empty().components();
        auto end = r.end() ? r.end()->value().components() : clustering_key_prefix::make_empty().components();
        auto start_it = start.begin();
        auto end_it = end.begin();

        // This test is enough because equal bounds in nonwrapping_range are inclusive.
        auto is_singular = [&schema] (const auto& type_it, const bytes_view& b1, const bytes_view& b2) {
            if (type_it == schema->clustering_key_type()->types().end()) {
                throw std::runtime_error(sprint("clustering key filter passed more components than defined in schema of %s.%s",
                    schema->ks_name(), schema->cf_name()));
            }
            return (*type_it)->compare(b1, b2) == 0;
        };
        auto type_it = schema->clustering_key_type()->types().begin();
        composite_ranges.reserve(schema->clustering_key_size());

        // the rule is to ignore any component cn if another component ck (k < n) is not if the form [v, v].
        // If we have [v1, v1], [v2, v2], ... {vl3, vr3}, ....
        // then we generate [v1, v1], [v2, v2], ... {vl3, vr3}. Where {  = '(' or '[', etc.
        while (start_it != start.end() && end_it != end.end() && is_singular(type_it++, *start_it, *end_it)) {
            composite_ranges.push_back(nonwrapping_range<bytes_view>({{ std::move(*start_it++), true }},
                {{ std::move(*end_it++), true }}));
        }
        // handle a single non-singular tail element, if present
        if (start_it != start.end() && end_it != end.end()) {
            composite_ranges.push_back(nonwrapping_range<bytes_view>({{ std::move(*start_it), r.start()->is_inclusive() }},
                {{ std::move(*end_it), r.end()->is_inclusive() }}));
        } else if (start_it != start.end()) {
            composite_ranges.push_back(nonwrapping_range<bytes_view>({{ std::move(*start_it), r.start()->is_inclusive() }}, {}));
        } else if (end_it != end.end()) {
            composite_ranges.push_back(nonwrapping_range<bytes_view>({}, {{ std::move(*end_it), r.end()->is_inclusive() }}));
        }

        ranges.push_back(std::move(composite_ranges));
    }
    return ranges;
}

// Return true if this sstable possibly stores clustering row(s) specified by ranges.
static inline bool
contains_rows(const sstables::sstable& sst, const schema_ptr& schema, const ck_filter_clustering_key_ranges& ranges) {
    auto& clustering_key_types = schema->clustering_key_type()->types();
    auto& clustering_components_ranges = sst.clustering_components_ranges();

    if (!schema->clustering_key_size() || clustering_components_ranges.empty()) {
        return true;
    }
    return boost::algorithm::any_of(ranges, [&] (const ck_filter_clustering_key_components& range) {
        auto s = std::min(range.size(), clustering_components_ranges.size());
        return boost::algorithm::all_of(boost::irange<unsigned>(0, s), [&] (unsigned i) {
            auto& type = clustering_key_types[i];
            return range[i].is_full() || range[i].overlaps(clustering_components_ranges[i], type->as_tri_comparator());
        });
    });
}

// Filter out sstables for reader using bloom filter and sstable metadata that keeps track
// of a range for each clustering component.
static std::vector<sstables::shared_sstable>
filter_sstable_for_reader(std::vector<sstables::shared_sstable>&& sstables, column_family& cf, const schema_ptr& schema,
        const sstables::key& key, const query::partition_slice& slice) {
    auto sstable_has_not_key = [&] (const sstables::shared_sstable& sst) {
        return !sst->filter_has_key(key);
    };
    sstables.erase(boost::remove_if(sstables, sstable_has_not_key), sstables.end());

    // no clustering filtering is applied if schema defines no clustering key or
    // compaction strategy thinks it will not benefit from such an optimization.
    if (!schema->clustering_key_size() || !cf.get_compaction_strategy().use_clustering_key_filter()) {
         return sstables;
    }
    ::cf_stats* stats = cf.cf_stats();
    stats->clustering_filter_count++;
    stats->sstables_checked_by_clustering_filter += sstables.size();

    auto ck_filtering_all_ranges = slice.get_all_ranges();
    // fast path to include all sstables if only one full range was specified.
    // For example, this happens if query only specifies a partition key.
    if (ck_filtering_all_ranges.size() == 1 && ck_filtering_all_ranges[0].is_full()) {
        stats->clustering_filter_fast_path_count++;
        stats->surviving_sstables_after_clustering_filter += sstables.size();
        return sstables;
    }
    auto ranges = ranges_for_clustering_key_filter(schema, ck_filtering_all_ranges);
    if (ranges.empty()) {
        return {};
    }

    int64_t min_timestamp = std::numeric_limits<int64_t>::max();
    auto sstable_has_clustering_key = [&min_timestamp, &schema, &ranges] (const sstables::shared_sstable& sst) {
        if (!contains_rows(*sst, schema, ranges)) {
            return false; // ordered after sstables that contain clustering rows.
        } else {
            min_timestamp = std::min(min_timestamp, sst->get_stats_metadata().min_timestamp);
            return true;
        }
    };
    auto sstable_has_relevant_tombstone = [&min_timestamp] (const sstables::shared_sstable& sst) {
        const auto& stats = sst->get_stats_metadata();
        // re-add sstable as candidate if it contains a tombstone that may cover a row in an included sstable.
        return (stats.max_timestamp > min_timestamp && stats.estimated_tombstone_drop_time.bin.map.size());
    };
    auto skipped = std::partition(sstables.begin(), sstables.end(), sstable_has_clustering_key);
    auto actually_skipped = std::partition(skipped, sstables.end(), sstable_has_relevant_tombstone);
    sstables.erase(actually_skipped, sstables.end());
    stats->surviving_sstables_after_clustering_filter += sstables.size();

    return sstables;
}

class range_sstable_reader final : public combined_mutation_reader {
    schema_ptr _s;
    const query::partition_range* _pr;
    lw_shared_ptr<sstables::sstable_set> _sstables;

    struct sstable_and_reader {
        sstables::shared_sstable _sstable;
        // This indirection is sad, but we need stable pointers to mutation
        // readers. If this ever becomes a performance issue we could store
        // mutation readers in an object pool (we don't need to preserve order
        // and can have holes left in the container when elements are removed).
        std::unique_ptr<mutation_reader> _reader;

        bool operator<(const sstable_and_reader& other) const {
            return _sstable < other._sstable;
        }

        struct less_compare {
            bool operator()(const sstable_and_reader& a, const sstable_and_reader& b) {
                return a < b;
            }
            bool operator()(const sstable_and_reader& a, const sstables::shared_sstable& b) {
                return a._sstable < b;
            }
            bool operator()(const sstables::shared_sstable& a, const sstable_and_reader& b) {
                return a < b._sstable;
            }
        };
    };
    std::vector<sstable_and_reader> _current_readers;

    // Use a pointer instead of copying, so we don't need to regenerate the reader if
    // the priority changes.
    const io_priority_class& _pc;
    tracing::trace_state_ptr _trace_state;
    const query::partition_slice& _slice;
private:
    std::unique_ptr<mutation_reader> create_reader(sstables::shared_sstable sst) {
        tracing::trace(_trace_state, "Reading partition range {} from sstable {}", *_pr, seastar::value_of([&sst] { return sst->get_filename(); }));
        // FIXME: make sstable::read_range_rows() return ::mutation_reader so that we can drop this wrapper.
        mutation_reader reader =
            make_mutation_reader<sstable_range_wrapping_reader>(sst, _s, *_pr, _slice, _pc);
        if (sst->is_shared()) {
            reader = make_filtering_reader(std::move(reader), belongs_to_current_shard);
        }
        return std::make_unique<mutation_reader>(std::move(reader));
    }
public:
    range_sstable_reader(schema_ptr s,
                         lw_shared_ptr<sstables::sstable_set> sstables,
                         const query::partition_range& pr,
                         const query::partition_slice& slice,
                         const io_priority_class& pc,
                         tracing::trace_state_ptr trace_state)
        : _s(s)
        , _pr(&pr)
        , _sstables(std::move(sstables))
        , _pc(pc)
        , _trace_state(std::move(trace_state))
        , _slice(slice)
    {
        auto ssts = _sstables->select(pr);
        std::vector<mutation_reader*> readers;
        readers.reserve(ssts.size());
        _current_readers.reserve(ssts.size());
        for (auto& sst : ssts) {
            auto reader = create_reader(sst);
            readers.emplace_back(reader.get());
            _current_readers.emplace_back(sstable_and_reader { sst, std::move(reader) });
        }
        init_mutation_reader_set(std::move(readers));
    }

    range_sstable_reader(range_sstable_reader&&) = delete; // reader takes reference to member fields

    virtual future<> fast_forward_to(const query::partition_range& pr) override {
        _pr = &pr;

        auto new_sstables = _sstables->select(pr);
        boost::range::sort(new_sstables);
        boost::range::sort(_current_readers);

        std::vector<sstables::shared_sstable> to_add;
        std::vector<sstable_and_reader> to_remove, unchanged;
        sstable_and_reader::less_compare cmp;
        boost::set_difference(new_sstables, _current_readers, std::back_inserter(to_add), cmp);
        std::set_difference(_current_readers.begin(), _current_readers.end(), new_sstables.begin(), new_sstables.end(),
                            boost::back_move_inserter(to_remove), cmp);
        std::set_intersection(_current_readers.begin(), _current_readers.end(), new_sstables.begin(), new_sstables.end(),
                              boost::back_move_inserter(unchanged), cmp);

        std::vector<sstable_and_reader> to_add_sar;
        boost::transform(to_add, std::back_inserter(to_add_sar), [&] (const sstables::shared_sstable& sst) {
            return sstable_and_reader { sst, create_reader(sst) };
        });

        auto get_mutation_readers = [] (std::vector<sstable_and_reader>& ssts) {
            std::vector<mutation_reader*> mrs;
            mrs.reserve(ssts.size());
            boost::range::transform(ssts, std::back_inserter(mrs), [] (const sstable_and_reader& s_a_r) {
                return s_a_r._reader.get();
            });
            return mrs;
        };

        auto to_add_mrs = get_mutation_readers(to_add_sar);
        auto to_remove_mrs = get_mutation_readers(to_remove);

        unchanged.insert(unchanged.end(), std::make_move_iterator(to_add_sar.begin()), std::make_move_iterator(to_add_sar.end()));
        return combined_mutation_reader::fast_forward_to(std::move(to_add_mrs), std::move(to_remove_mrs), pr).then([this, new_readers = std::move(unchanged)] () mutable {
            _current_readers = std::move(new_readers);
        });
    }
};

class single_key_sstable_reader final : public mutation_reader::impl {
    column_family* _cf;
    schema_ptr _schema;
    dht::ring_position _rp;
    sstables::key _key;
    std::vector<streamed_mutation> _mutations;
    bool _done = false;
    lw_shared_ptr<sstables::sstable_set> _sstables;
    utils::estimated_histogram& _sstable_histogram;
    // Use a pointer instead of copying, so we don't need to regenerate the reader if
    // the priority changes.
    const io_priority_class& _pc;
    const query::partition_slice& _slice;
    tracing::trace_state_ptr _trace_state;
public:
    single_key_sstable_reader(column_family* cf,
                              schema_ptr schema,
                              lw_shared_ptr<sstables::sstable_set> sstables,
                              utils::estimated_histogram& sstable_histogram,
                              const partition_key& key,
                              const query::partition_slice& slice,
                              const io_priority_class& pc,
                              tracing::trace_state_ptr trace_state)
        : _cf(cf)
        , _schema(std::move(schema))
        , _rp(dht::global_partitioner().decorate_key(*_schema, key))
        , _key(sstables::key::from_partition_key(*_schema, key))
        , _sstables(std::move(sstables))
        , _sstable_histogram(sstable_histogram)
        , _pc(pc)
        , _slice(slice)
        , _trace_state(std::move(trace_state))
    { }

    virtual future<streamed_mutation_opt> operator()() override {
        if (_done) {
            return make_ready_future<streamed_mutation_opt>();
        }
        auto candidates = filter_sstable_for_reader(_sstables->select(query::partition_range(_rp)), *_cf, _schema, _key, _slice);
        return parallel_for_each(std::move(candidates),
            [this](const lw_shared_ptr<sstables::sstable>& sstable) {
                tracing::trace(_trace_state, "Reading key {} from sstable {}", *_rp.key(), seastar::value_of([&sstable] { return sstable->get_filename(); }));
                return sstable->read_row(_schema, _key, _slice, _pc).then([this](auto smo) {
                    if (smo) {
                        _mutations.emplace_back(std::move(*smo));
                    }
                });
        }).then([this] () -> streamed_mutation_opt {
            _done = true;
            if (_mutations.empty()) {
                return { };
            }
            _sstable_histogram.add(_mutations.size());
            return merge_mutations(std::move(_mutations));
        });
    }
};

mutation_reader
column_family::make_sstable_reader(schema_ptr s,
                                   const query::partition_range& pr,
                                   const query::partition_slice& slice,
                                   const io_priority_class& pc,
                                   tracing::trace_state_ptr trace_state) const {
    // restricts a reader's concurrency if the configuration specifies it
    auto restrict_reader = [&] (mutation_reader&& in) {
        auto&& config = [this, &pc] () -> const restricted_mutation_reader_config& {
            if (service::get_local_streaming_read_priority().id() == pc.id()) {
                return _config.streaming_read_concurrency_config;
            }
            return _config.read_concurrency_config;
        }();
        if (config.sem) {
            return make_restricted_reader(config, 1, std::move(in));
        } else {
            return std::move(in);
        }
    };

    if (pr.is_singular() && pr.start()->value().has_key()) {
        const dht::ring_position& pos = pr.start()->value();
        if (dht::shard_of(pos.token()) != engine().cpu_id()) {
            return make_empty_reader(); // range doesn't belong to this shard
        }
        return restrict_reader(make_mutation_reader<single_key_sstable_reader>(const_cast<column_family*>(this), std::move(s), _sstables,
            _stats.estimated_sstable_per_read, *pos.key(), slice, pc, std::move(trace_state)));
    } else {
        // range_sstable_reader is not movable so we need to wrap it
        return restrict_reader(make_mutation_reader<range_sstable_reader>(std::move(s), _sstables, pr, slice, pc, std::move(trace_state)));
    }
}

// Exposed for testing, not performance critical.
future<column_family::const_mutation_partition_ptr>
column_family::find_partition(schema_ptr s, const dht::decorated_key& key) const {
    return do_with(query::partition_range::make_singular(key), [s = std::move(s), this] (auto& range) {
        return do_with(this->make_reader(s, range), [] (mutation_reader& reader) {
            return reader().then([] (auto sm) {
                return mutation_from_streamed_mutation(std::move(sm));
            }).then([] (mutation_opt&& mo) -> std::unique_ptr<const mutation_partition> {
                if (!mo) {
                    return {};
                }
                return std::make_unique<const mutation_partition>(std::move(mo->partition()));
            });
        });
    });
}

future<column_family::const_mutation_partition_ptr>
column_family::find_partition_slow(schema_ptr s, const partition_key& key) const {
    return find_partition(s, dht::global_partitioner().decorate_key(*s, key));
}

future<column_family::const_row_ptr>
column_family::find_row(schema_ptr s, const dht::decorated_key& partition_key, clustering_key clustering_key) const {
    return find_partition(std::move(s), partition_key).then([clustering_key = std::move(clustering_key)] (const_mutation_partition_ptr p) {
        if (!p) {
            return make_ready_future<const_row_ptr>();
        }
        auto r = p->find_row(clustering_key);
        if (r) {
            // FIXME: remove copy if only one data source
            return make_ready_future<const_row_ptr>(std::make_unique<row>(*r));
        } else {
            return make_ready_future<const_row_ptr>();
        }
    });
}

mutation_reader
column_family::make_reader(schema_ptr s,
                           const query::partition_range& range,
                           const query::partition_slice& slice,
                           const io_priority_class& pc,
                           tracing::trace_state_ptr trace_state) const {
    std::vector<mutation_reader> readers;
    readers.reserve(_memtables->size() + 1);

    // We're assuming that cache and memtables are both read atomically
    // for single-key queries, so we don't need to special case memtable
    // undergoing a move to cache. At any given point in time between
    // deferring points the sum of data in memtable and cache is coherent. If
    // single-key queries for each data source were performed across deferring
    // points, it would be possible that partitions which are ahead of the
    // memtable cursor would be placed behind the cache cursor, resulting in
    // those partitions being missing in the combined reader.
    //
    // We need to handle this in range queries though, as they are always
    // deferring. scanning_reader from memtable.cc is falling back to reading
    // the sstable when memtable is flushed. After memtable is moved to cache,
    // new readers will no longer use the old memtable, but until then
    // performance may suffer. We should fix this when we add support for
    // range queries in cache, so that scans can always be satisfied form
    // memtable and cache only, as long as data is not evicted.
    //
    // https://github.com/scylladb/scylla/issues/309
    // https://github.com/scylladb/scylla/issues/185

    for (auto&& mt : *_memtables) {
        readers.emplace_back(mt->make_reader(s, range, slice, pc));
    }

    if (_config.enable_cache) {
        readers.emplace_back(_cache.make_reader(s, range, slice, pc, std::move(trace_state)));
    } else {
        readers.emplace_back(make_sstable_reader(s, range, slice, pc, std::move(trace_state)));
    }

    return make_combined_reader(std::move(readers));
}

mutation_reader
column_family::make_streaming_reader(schema_ptr s,
                           const query::partition_range& range) const {
    auto& slice = query::full_slice;
    auto& pc = service::get_local_streaming_read_priority();

    std::vector<mutation_reader> readers;
    readers.reserve(_memtables->size() + 1);

    for (auto&& mt : *_memtables) {
        readers.emplace_back(mt->make_reader(s, range, slice, pc));
    }

    readers.emplace_back(make_sstable_reader(s, range, slice, pc, nullptr));

    return make_combined_reader(std::move(readers));
}

// Not performance critical. Currently used for testing only.
template <typename Func>
future<bool>
column_family::for_all_partitions(schema_ptr s, Func&& func) const {
    static_assert(std::is_same<bool, std::result_of_t<Func(const dht::decorated_key&, const mutation_partition&)>>::value,
                  "bad Func signature");

    struct iteration_state {
        mutation_reader reader;
        Func func;
        bool ok = true;
        bool empty = false;
    public:
        bool done() const { return !ok || empty; }
        iteration_state(schema_ptr s, const column_family& cf, Func&& func)
            : reader(cf.make_reader(std::move(s)))
            , func(std::move(func))
        { }
    };

    return do_with(iteration_state(std::move(s), *this, std::move(func)), [] (iteration_state& is) {
        return do_until([&is] { return is.done(); }, [&is] {
            return is.reader().then([] (auto sm) {
                return mutation_from_streamed_mutation(std::move(sm));
            }).then([&is](mutation_opt&& mo) {
                if (!mo) {
                    is.empty = true;
                } else {
                    is.ok = is.func(mo->decorated_key(), mo->partition());
                }
            });
        }).then([&is] {
            return is.ok;
        });
    });
}

future<bool>
column_family::for_all_partitions_slow(schema_ptr s, std::function<bool (const dht::decorated_key&, const mutation_partition&)> func) const {
    return for_all_partitions(std::move(s), std::move(func));
}

class lister {
public:
    using dir_entry_types = std::unordered_set<directory_entry_type, enum_hash<directory_entry_type>>;
    using walker_type = std::function<future<> (directory_entry)>;
    using filter_type = std::function<bool (const sstring&)>;
private:
    file _f;
    walker_type _walker;
    filter_type _filter;
    dir_entry_types _expected_type;
    subscription<directory_entry> _listing;
    sstring _dirname;

public:
    lister(file f, dir_entry_types type, walker_type walker, sstring dirname)
            : _f(std::move(f))
            , _walker(std::move(walker))
            , _filter([] (const sstring& fname) { return true; })
            , _expected_type(type)
            , _listing(_f.list_directory([this] (directory_entry de) { return _visit(de); }))
            , _dirname(dirname) {
    }

    lister(file f, dir_entry_types type, walker_type walker, filter_type filter, sstring dirname)
            : lister(std::move(f), type, std::move(walker), dirname) {
        _filter = std::move(filter);
    }

    static future<> scan_dir(sstring name, dir_entry_types type, walker_type walker, filter_type filter = [] (const sstring& fname) { return true; });
protected:
    future<> _visit(directory_entry de) {

        return guarantee_type(std::move(de)).then([this] (directory_entry de) {
            // Hide all synthetic directories and hidden files.
            if ((!_expected_type.count(*(de.type))) || (de.name[0] == '.')) {
                return make_ready_future<>();
            }

            // apply a filter
            if (!_filter(_dirname + "/" + de.name)) {
                return make_ready_future<>();
            }

            return _walker(de);
        });

    }
    future<> done() {
        return _listing.done().then([this] {
            return _f.close();
        });
    }
private:
    future<directory_entry> guarantee_type(directory_entry de) {
        if (de.type) {
            return make_ready_future<directory_entry>(std::move(de));
        } else {
            auto f = engine().file_type(_dirname + "/" + de.name);
            return f.then([de = std::move(de)] (std::experimental::optional<directory_entry_type> t) mutable {
                de.type = t;
                return make_ready_future<directory_entry>(std::move(de));
            });
        }
    }
};


future<> lister::scan_dir(sstring name, lister::dir_entry_types type, walker_type walker, filter_type filter) {
    return open_checked_directory(general_disk_error_handler, name).then([type, walker = std::move(walker), filter = std::move(filter), name] (file f) {
            auto l = make_lw_shared<lister>(std::move(f), type, walker, filter, name);
            return l->done().then([l] { });
    });
}

static bool belongs_to_current_shard(const std::vector<shard_id>& shards) {
    return boost::find(shards, engine().cpu_id()) != shards.end();
}

static bool belongs_to_other_shard(const std::vector<shard_id>& shards) {
    return shards.size() != size_t(belongs_to_current_shard(shards));
}

future<> column_family::load_sstable(sstables::sstable&& sstab, bool reset_level) {
    auto sst = make_lw_shared<sstables::sstable>(std::move(sstab));
    return sst->get_owning_shards_from_unloaded().then([this, sst, reset_level] (std::vector<shard_id> shards) mutable {
        // Checks whether or not sstable belongs to current shard.
        if (!belongs_to_current_shard(shards)) {
            dblog.debug("sstable {} not relevant for this shard, ignoring", sst->get_filename());
            sst->mark_for_deletion();
            return make_ready_future<>();
        }
        bool in_other_shard = belongs_to_other_shard(shards);
        return sst->load().then([this, sst, in_other_shard, reset_level] () mutable {
            if (in_other_shard) {
                // If we're here, this sstable is shared by this and other
                // shard(s). Shared sstables cannot be deleted until all
                // shards compacted them, so to reduce disk space usage we
                // want to start splitting them now.
                // However, we need to delay this compaction until we read all
                // the sstables belonging to this CF, because we need all of
                // them to know which tombstones we can drop, and what
                // generation number is free.
                _sstables_need_rewrite.push_back(sst);
            }
            if (reset_level) {
                // When loading a migrated sstable, set level to 0 because
                // it may overlap with existing tables in levels > 0.
                // This step is optional, because even if we didn't do this
                // scylla would detect the overlap, and bring back some of
                // the sstables to level 0.
                sst->set_sstable_level(0);
            }
            add_sstable(sst);
        });
    });
}

// load_sstable() wants to start rewriting sstables which are shared between
// several shards, but we can't start any compaction before all the sstables
// of this CF were loaded. So call this function to start rewrites, if any.
void column_family::start_rewrite() {
    for (auto sst : _sstables_need_rewrite) {
        dblog.info("Splitting {} for shard", sst->get_filename());
        _compaction_manager.submit_sstable_rewrite(this, sst);
    }
    _sstables_need_rewrite.clear();
}

future<sstables::entry_descriptor> column_family::probe_file(sstring sstdir, sstring fname) {

    using namespace sstables;

    entry_descriptor comps = entry_descriptor::make_descriptor(fname);

    // Every table will have a TOC. Using a specific file as a criteria, as
    // opposed to, say verifying _sstables.count() to be zero is more robust
    // against parallel loading of the directory contents.
    if (comps.component != sstable::component_type::TOC) {
        return make_ready_future<entry_descriptor>(std::move(comps));
    }

    update_sstables_known_generation(comps.generation);

    {
        auto i = boost::range::find_if(*_sstables->all(), [gen = comps.generation] (sstables::shared_sstable sst) { return sst->generation() == gen; });
        if (i != _sstables->all()->end()) {
            auto new_toc = sstdir + "/" + fname;
            throw std::runtime_error(sprint("Attempted to add sstable generation %d twice: new=%s existing=%s",
                                            comps.generation, new_toc, (*i)->toc_filename()));
        }
    }

    return load_sstable(sstables::sstable(
            _schema, sstdir, comps.generation,
            comps.version, comps.format)).then_wrapped([fname, comps] (future<> f) {
        try {
            f.get();
        } catch (malformed_sstable_exception& e) {
            dblog.error("malformed sstable {}: {}. Refusing to boot", fname, e.what());
            throw;
        } catch(...) {
            dblog.error("Unrecognized error while processing {}: {}. Refusing to boot",
                    fname, std::current_exception());
            throw;
        }
        return make_ready_future<entry_descriptor>(std::move(comps));
    });
}

void column_family::update_stats_for_new_sstable(uint64_t disk_space_used_by_sstable) {
    _stats.live_disk_space_used += disk_space_used_by_sstable;
    _stats.total_disk_space_used += disk_space_used_by_sstable;
    _stats.live_sstable_count++;
}

void column_family::add_sstable(sstables::sstable&& sstable) {
    add_sstable(make_lw_shared(std::move(sstable)));
}

void column_family::add_sstable(lw_shared_ptr<sstables::sstable> sstable) {
    // allow in-progress reads to continue using old list
    _sstables = make_lw_shared(*_sstables);
    update_stats_for_new_sstable(sstable->bytes_on_disk());
    _sstables->insert(std::move(sstable));
}

future<>
column_family::update_cache(memtable& m, sstables::shared_sstable exclude_sstable) {
    if (_config.enable_cache) {
       // be careful to use the old sstable list, since the new one will hit every
       // mutation in m.
       return _cache.update(m, make_partition_presence_checker(std::move(exclude_sstable)));
    } else {
       return make_ready_future<>();
    }
}

// FIXME: because we are coalescing, it could be that mutations belonging to the same
// range end up in two different tables. Technically, we should wait for both. However,
// the only way we have to make this happen now is to wait on all previous writes. This
// certainly is an overkill, so we won't do it. We can fix this longer term by looking
// at the PREPARE messages, and then noting what is the minimum future we should be
// waiting for.
future<>
column_family::seal_active_streaming_memtable_delayed() {
    auto old = _streaming_memtables->back();
    if (old->empty()) {
        return make_ready_future<>();
    }

    if (!_delayed_streaming_flush.armed()) {
            // We don't want to wait for too long, because the incoming mutations will not be available
            // until we flush them to SSTables. On top of that, if the sender ran out of messages, it won't
            // send more until we respond to some - which depends on these futures resolving. Sure enough,
            // the real fix for that second one is to have better communication between sender and receiver,
            // but that's not realistic ATM. If we did have better negotiation here, we would not need a timer
            // at all.
            _delayed_streaming_flush.arm(2s);
    }

    return with_gate(_streaming_flush_gate, [this, old] {
        return _waiting_streaming_flushes.get_shared_future();
    });
}

future<>
column_family::seal_active_streaming_memtable_immediate() {
    auto old = _streaming_memtables->back();
    if (old->empty()) {
        return make_ready_future<>();
    }
    _streaming_memtables->add_memtable();
    _streaming_memtables->erase(old);

    auto guard = _streaming_flush_phaser.start();
    return with_gate(_streaming_flush_gate, [this, old] {
        _delayed_streaming_flush.cancel();
        auto current_waiters = std::exchange(_waiting_streaming_flushes, shared_promise<>());
        auto f = current_waiters.get_shared_future(); // for this seal

        with_lock(_sstables_lock.for_read(), [this, old] {
            auto newtab = make_lw_shared<sstables::sstable>(_schema,
                _config.datadir, calculate_generation_for_new_table(),
                sstables::sstable::version_types::ka,
                sstables::sstable::format_types::big);

            newtab->set_unshared();

            auto&& priority = service::get_local_streaming_write_priority();
            // This is somewhat similar to the main memtable flush, but with important differences.
            //
            // The first difference, is that we don't keep aggregate collectd statistics about this one.
            // If we ever need to, we'll keep them separate statistics, but we don't want to polute the
            // main stats about memtables with streaming memtables.
            //
            // Second, we will not bother touching the cache after this flush. The current streaming code
            // will invalidate the ranges it touches, so we won't do it twice. Even when that changes, the
            // cache management code in here will have to differ from the main memtable's one. Please see
            // the comment at flush_streaming_mutations() for details.
            //
            // Lastly, we don't have any commitlog RP to update, and we don't need to deal manipulate the
            // memtable list, since this memtable was not available for reading up until this point.
            return newtab->write_components(*old, incremental_backups_enabled(), priority).then([this, newtab, old] {
                return newtab->open_data();
            }).then([this, old, newtab] () {
                add_sstable(newtab);
                trigger_compaction();
            }).handle_exception([] (auto ep) {
                dblog.error("failed to write streamed sstable: {}", ep);
                return make_exception_future<>(ep);
            });
            // We will also not have any retry logic. If we fail here, we'll fail the streaming and let
            // the upper layers know. They can then apply any logic they want here.
        }).then_wrapped([this, current_waiters = std::move(current_waiters)] (future <> f) mutable {
            if (f.failed()) {
                current_waiters.set_exception(f.get_exception());
            } else {
                current_waiters.set_value();
            }
        });

        return f;
    }).finally([guard = std::move(guard)] { });
}

future<> column_family::seal_active_streaming_memtable_big(streaming_memtable_big& smb) {
    auto old = smb.memtables->back();
    if (old->empty()) {
        return make_ready_future<>();
    }
    smb.memtables->add_memtable();
    smb.memtables->erase(old);
    return with_gate(_streaming_flush_gate, [this, old, &smb] {
        return with_gate(smb.flush_in_progress, [this, old, &smb] {
            return with_lock(_sstables_lock.for_read(), [this, old, &smb] {
                auto newtab = make_lw_shared<sstables::sstable>(_schema,
                                                                _config.datadir, calculate_generation_for_new_table(),
                                                                sstables::sstable::version_types::ka,
                                                                sstables::sstable::format_types::big);

                newtab->set_unshared();

                auto&& priority = service::get_local_streaming_write_priority();
                return newtab->write_components(*old, incremental_backups_enabled(), priority, true).then([this, newtab, old, &smb] {
                    smb.sstables.emplace_back(newtab);
                }).handle_exception([] (auto ep) {
                    dblog.error("failed to write streamed sstable: {}", ep);
                    return make_exception_future<>(ep);
                });
            });
        });
    });
}

future<>
column_family::seal_active_memtable(memtable_list::flush_behavior ignored) {
    auto old = _memtables->back();
    dblog.debug("Sealing active memtable of {}.{}, partitions: {}, occupancy: {}", _schema->cf_name(), _schema->ks_name(), old->partition_count(), old->occupancy());

    if (old->empty()) {
        dblog.debug("Memtable is empty");
        return make_ready_future<>();
    }
    _memtables->add_memtable();

    assert(_highest_flushed_rp < old->replay_position()
    || (_highest_flushed_rp == db::replay_position() && old->replay_position() == db::replay_position())
    );
    _highest_flushed_rp = old->replay_position();

    return _flush_queue->run_cf_flush(old->replay_position(), [old, this] {
      auto memtable_size = old->occupancy().total_space();

      _config.cf_stats->pending_memtables_flushes_count++;
      _config.cf_stats->pending_memtables_flushes_bytes += memtable_size;

      return repeat([this, old] {
        return with_lock(_sstables_lock.for_read(), [this, old] {
            _flush_queue->check_open_gate();
            return try_flush_memtable_to_sstable(old);
        });
      }).then([this, memtable_size] {
        _config.cf_stats->pending_memtables_flushes_count--;
        _config.cf_stats->pending_memtables_flushes_bytes -= memtable_size;
      });
    }, [old, this] {
        if (_commitlog) {
            _commitlog->discard_completed_segments(_schema->id(), old->replay_position());
        }
    });
    // FIXME: release commit log
    // FIXME: provide back-pressure to upper layers
}

future<stop_iteration>
column_family::try_flush_memtable_to_sstable(lw_shared_ptr<memtable> old) {
    auto gen = calculate_generation_for_new_table();

    auto newtab = make_lw_shared<sstables::sstable>(_schema,
        _config.datadir, gen,
        sstables::sstable::version_types::ka,
        sstables::sstable::format_types::big);

    newtab->set_unshared();
    dblog.debug("Flushing to {}", newtab->get_filename());
    // Note that due to our sharded architecture, it is possible that
    // in the face of a value change some shards will backup sstables
    // while others won't.
    //
    // This is, in theory, possible to mitigate through a rwlock.
    // However, this doesn't differ from the situation where all tables
    // are coming from a single shard and the toggle happens in the
    // middle of them.
    //
    // The code as is guarantees that we'll never partially backup a
    // single sstable, so that is enough of a guarantee.
    auto&& priority = service::get_local_memtable_flush_priority();
    return newtab->write_components(*old, incremental_backups_enabled(), priority).then([this, newtab, old] {
        return newtab->open_data();
    }).then_wrapped([this, old, newtab] (future<> ret) {
        dblog.debug("Flushing to {} done", newtab->get_filename());
        // Could pass the iterator to the seal functions, and avoid the need to search the
        // unordered_map here. But this is supposed to be cheap and it is a lot less clutter in the
        // method signatures. Also makes it optional and streaming memtables don't have to do it.
        // Note that the number of entries in this hash is limited by the background flushes
        // semaphore, so it'll always be small.
        //
        // In terms of releasing dirty memory, this is almost as far as we should go. We could do
        // this right before updating the cache, but from this point on to update_cache we have no
        // deferring points, so that's fine. We do it in here because if we fail this write it will
        // try the write again and that will create a new flush reader that will decrease dirty
        // memory again. So we need to get rid of the charges here anyway for correctness.
        //
        // After the cache starts to be updated the region in transferred over. We kind of assume
        // there will be no deferring point between this and update cache transferring ownership.
        // It's not that bad if it is so we wouldn't really protect against it, but without a
        // deferring point we can guarantee that no request will see a spike in dirty memory between
        // the release of our memory and the execution of a request.
        dirty_memory_manager::from_region_group(old->region_group()).remove_from_flush_manager(&(old->region()));
        try {
            ret.get();

            // We must add sstable before we call update_cache(), because
            // memtable's data after moving to cache can be evicted at any time.
            auto old_sstables = _sstables;
            add_sstable(newtab);
            old->mark_flushed(newtab);

            trigger_compaction();

            return update_cache(*old, newtab).then_wrapped([this, newtab, old] (future<> f) {
                try {
                    f.get();
                } catch(...) {
                    dblog.error("failed to move memtable for {} to cache: {}", newtab->get_filename(), std::current_exception());
                }

                _memtables->erase(old);
                dblog.debug("Memtable for {} replaced", newtab->get_filename());

                return make_ready_future<stop_iteration>(stop_iteration::yes);
            });
        } catch (...) {
            dblog.error("failed to write sstable {}: {}", newtab->get_filename(), std::current_exception());
        }
        return sleep(10s).then([] {
            return make_ready_future<stop_iteration>(stop_iteration::no);
        });
    });
}

void
column_family::start() {
    // FIXME: add option to disable automatic compaction.
    start_compaction();
}

future<>
column_family::stop() {
    return when_all(_memtables->request_flush(), _streaming_memtables->request_flush()).discard_result().finally([this] {
        return _compaction_manager.remove(this).then([this] {
            // Nest, instead of using when_all, so we don't lose any exceptions.
            return _flush_queue->close().then([this] {
                return _streaming_flush_gate.close();
            });
        }).then([this] {
            return _sstable_deletion_gate.close();
        });
    });
}

static io_error_handler error_handler_for_upload_dir() {
    return [] (std::exception_ptr eptr) {
        // do nothing about sstable exception and caller will just rethrow it.
    };
}

future<std::vector<sstables::entry_descriptor>> column_family::flush_upload_dir() {
    struct work {
        std::map<int64_t, sstables::shared_sstable> sstables;
        std::unordered_map<int64_t, sstables::entry_descriptor> descriptors;
        std::vector<sstables::entry_descriptor> flushed;
    };

    return do_with(work(), [this] (work& work) {
        return lister::scan_dir(_config.datadir + "/upload/", { directory_entry_type::regular },
                [this, &work] (directory_entry de) {
            auto comps = sstables::entry_descriptor::make_descriptor(de.name);
            if (comps.component != sstables::sstable::component_type::TOC) {
                return make_ready_future<>();
            }
            auto sst = make_lw_shared<sstables::sstable>(_schema, _config.datadir + "/upload", comps.generation,
                comps.version, comps.format, gc_clock::now(),
                [] (disk_error_signal_type&) { return error_handler_for_upload_dir(); });
            work.sstables.emplace(comps.generation, std::move(sst));
            work.descriptors.emplace(comps.generation, std::move(comps));
            return make_ready_future<>();
        }, &manifest_json_filter).then([this, &work] {
            work.flushed.reserve(work.descriptors.size());

            return do_for_each(work.sstables, [this, &work] (auto& pair) {
                auto gen = this->calculate_generation_for_new_table();
                auto& sst = pair.second;

                auto&& comps = std::move(work.descriptors.at(pair.first));
                comps.generation = gen;
                work.flushed.push_back(std::move(comps));

                // Read toc content as it will be needed for moving and deleting a sstable.
                return sst->read_toc().then([&sst] {
                    return sst->mutate_sstable_level(0);
                }).then([this, &sst, gen] {
                    return sst->create_links(_config.datadir, gen);
                }).then([&sst] {
                    return sstables::remove_by_toc_name(sst->toc_filename(), error_handler_for_upload_dir());
                });
            });
        }).then([&work] {
            return make_ready_future<std::vector<sstables::entry_descriptor>>(std::move(work.flushed));
        });
    });
}

future<std::vector<sstables::entry_descriptor>>
column_family::reshuffle_sstables(std::set<int64_t> all_generations, int64_t start) {
    struct work {
        int64_t current_gen;
        std::set<int64_t> all_generations; // Stores generation of all live sstables in the system.
        std::map<int64_t, sstables::shared_sstable> sstables;
        std::unordered_map<int64_t, sstables::entry_descriptor> descriptors;
        std::vector<sstables::entry_descriptor> reshuffled;
        work(int64_t start, std::set<int64_t> gens)
            : current_gen(start ? start : 1)
            , all_generations(gens) {}
    };

    return do_with(work(start, std::move(all_generations)), [this] (work& work) {
        return lister::scan_dir(_config.datadir, { directory_entry_type::regular }, [this, &work] (directory_entry de) {
            auto comps = sstables::entry_descriptor::make_descriptor(de.name);
            if (comps.component != sstables::sstable::component_type::TOC) {
                return make_ready_future<>();
            }
            // Skip generations that were already loaded by Scylla at a previous stage.
            if (work.all_generations.count(comps.generation) != 0) {
                return make_ready_future<>();
            }
            auto sst = make_lw_shared<sstables::sstable>(_schema,
                                                         _config.datadir, comps.generation,
                                                         comps.version, comps.format);
            work.sstables.emplace(comps.generation, std::move(sst));
            work.descriptors.emplace(comps.generation, std::move(comps));
            // FIXME: This is the only place in which we actually issue disk activity aside from
            // directory metadata operations.
            //
            // But without the TOC information, we don't know which files we should link.
            // The alternative to that would be to change create link to try creating a
            // link for all possible files and handling the failures gracefuly, but that's not
            // exactly fast either.
            //
            // Those SSTables are not known by anyone in the system. So we don't have any kind of
            // object describing them. There isn't too much of a choice.
            return work.sstables[comps.generation]->read_toc();
        }, &manifest_json_filter).then([&work] {
            // Note: cannot be parallel because we will be shuffling things around at this stage. Can't race.
            return do_for_each(work.sstables, [&work] (auto& pair) {
                auto&& comps = std::move(work.descriptors.at(pair.first));
                comps.generation = work.current_gen;
                work.reshuffled.push_back(std::move(comps));

                if (pair.first == work.current_gen) {
                    ++work.current_gen;
                    return make_ready_future<>();
                }
                return pair.second->set_generation(work.current_gen++);
            });
        }).then([&work] {
            return make_ready_future<std::vector<sstables::entry_descriptor>>(std::move(work.reshuffled));
        });
    });
}

void column_family::rebuild_statistics() {
    // zeroing live_disk_space_used and live_sstable_count because the
    // sstable list was re-created
    _stats.live_disk_space_used = 0;
    _stats.live_sstable_count = 0;

    for (auto&& tab : boost::range::join(_sstables_compacted_but_not_deleted,
                    // this might seem dangerous, but "move" here just avoids constness,
                    // making the two ranges compatible when compiling with boost 1.55.
                    // Noone is actually moving anything...
                                         std::move(*_sstables->all()))) {
        update_stats_for_new_sstable(tab->data_size());
    }
}

void
column_family::rebuild_sstable_list(const std::vector<sstables::shared_sstable>& new_sstables,
                                    const std::vector<sstables::shared_sstable>& sstables_to_remove) {
    // Build a new list of _sstables: We remove from the existing list the
    // tables we compacted (by now, there might be more sstables flushed
    // later), and we add the new tables generated by the compaction.
    // We create a new list rather than modifying it in-place, so that
    // on-going reads can continue to use the old list.
    //
    // We only remove old sstables after they are successfully deleted,
    // to avoid a new compaction from ignoring data in the old sstables
    // if the deletion fails (note deletion of shared sstables can take
    // unbounded time, because all shards must agree on the deletion).
    auto current_sstables = _sstables;
    auto new_sstable_list = _compaction_strategy.make_sstable_set(_schema);
    auto new_compacted_but_not_deleted = _sstables_compacted_but_not_deleted;


    std::unordered_set<sstables::shared_sstable> s(
           sstables_to_remove.begin(), sstables_to_remove.end());

    // First, add the new sstables.

    // this might seem dangerous, but "move" here just avoids constness,
    // making the two ranges compatible when compiling with boost 1.55.
    // Noone is actually moving anything...
    for (auto&& tab : boost::range::join(new_sstables, std::move(*current_sstables->all()))) {
        // Checks if oldtab is a sstable not being compacted.
        if (!s.count(tab)) {
            new_sstable_list.insert(tab);
        } else {
            new_compacted_but_not_deleted.push_back(tab);
        }
    }
    _sstables = make_lw_shared(std::move(new_sstable_list));
    _sstables_compacted_but_not_deleted = std::move(new_compacted_but_not_deleted);

    rebuild_statistics();

    // Second, delete the old sstables.  This is done in the background, so we can
    // consider this compaction completed.
    seastar::with_gate(_sstable_deletion_gate, [this, sstables_to_remove] {
        return sstables::delete_atomically(sstables_to_remove).then_wrapped([this, sstables_to_remove] (future<> f) {
            std::exception_ptr eptr;
            try {
                f.get();
            } catch(...) {
                eptr = std::current_exception();
            }

            // unconditionally remove compacted sstables from _sstables_compacted_but_not_deleted,
            // or they could stay forever in the set, resulting in deleted files remaining
            // opened and disk space not being released until shutdown.
            std::unordered_set<sstables::shared_sstable> s(
                   sstables_to_remove.begin(), sstables_to_remove.end());
            auto e = boost::range::remove_if(_sstables_compacted_but_not_deleted, [&] (sstables::shared_sstable sst) -> bool {
                return s.count(sst);
            });
            _sstables_compacted_but_not_deleted.erase(e, _sstables_compacted_but_not_deleted.end());
            rebuild_statistics();

            if (eptr) {
                return make_exception_future<>(eptr);
            }
            return make_ready_future<>();
        }).handle_exception([] (std::exception_ptr e) {
            try {
                std::rethrow_exception(e);
            } catch (sstables::atomic_deletion_cancelled& adc) {
                dblog.debug("Failed to delete sstables after compaction: {}", adc);
            }
        });
    });
}

future<>
column_family::compact_sstables(sstables::compaction_descriptor descriptor, bool cleanup) {
    if (!descriptor.sstables.size()) {
        // if there is nothing to compact, just return.
        return make_ready_future<>();
    }

    return with_lock(_sstables_lock.for_read(), [this, descriptor = std::move(descriptor), cleanup] {
        auto sstables_to_compact = make_lw_shared<std::vector<sstables::shared_sstable>>(std::move(descriptor.sstables));

        auto create_sstable = [this] {
                auto gen = this->calculate_generation_for_new_table();
                // FIXME: use "tmp" marker in names of incomplete sstable
                auto sst = make_lw_shared<sstables::sstable>(_schema, _config.datadir, gen,
                        sstables::sstable::version_types::ka,
                        sstables::sstable::format_types::big);
                sst->set_unshared();
                return sst;
        };
        return sstables::compact_sstables(*sstables_to_compact, *this, create_sstable, descriptor.max_sstable_bytes, descriptor.level,
                cleanup).then([this, sstables_to_compact] (auto new_sstables) {
            _compaction_strategy.notify_completion(*sstables_to_compact, new_sstables);
            return this->rebuild_sstable_list(new_sstables, *sstables_to_compact);
        });
    });
}

static bool needs_cleanup(const lw_shared_ptr<sstables::sstable>& sst,
                   const lw_shared_ptr<std::vector<nonwrapping_range<dht::token>>>& owned_ranges,
                   schema_ptr s) {
    auto first = sst->get_first_partition_key();
    auto last = sst->get_last_partition_key();
    auto first_token = dht::global_partitioner().get_token(*s, first);
    auto last_token = dht::global_partitioner().get_token(*s, last);
    nonwrapping_range<dht::token> sst_token_range = nonwrapping_range<dht::token>::make(first_token, last_token);

    // return true iff sst partition range isn't fully contained in any of the owned ranges.
    for (auto& r : *owned_ranges) {
        if (r.contains(sst_token_range, dht::token_comparator())) {
            return false;
        }
    }
    return true;
}

future<> column_family::cleanup_sstables(sstables::compaction_descriptor descriptor) {
    std::vector<nonwrapping_range<dht::token>> r = service::get_local_storage_service().get_local_ranges(_schema->ks_name());
    auto owned_ranges = make_lw_shared<std::vector<nonwrapping_range<dht::token>>>(std::move(r));
    auto sstables_to_cleanup = make_lw_shared<std::vector<sstables::shared_sstable>>(std::move(descriptor.sstables));

    return parallel_for_each(*sstables_to_cleanup, [this, owned_ranges = std::move(owned_ranges), sstables_to_cleanup] (auto& sst) {
        if (!owned_ranges->empty() && !needs_cleanup(sst, owned_ranges, _schema)) {
           return make_ready_future<>();
        }

        std::vector<sstables::shared_sstable> sstable_to_compact({ sst });
        return this->compact_sstables(sstables::compaction_descriptor(std::move(sstable_to_compact), sst->get_sstable_level()), true);
    });
}

future<>
column_family::load_new_sstables(std::vector<sstables::entry_descriptor> new_tables) {
    return parallel_for_each(new_tables, [this] (auto comps) {
        return this->load_sstable(sstables::sstable(
                _schema, _config.datadir,
                comps.generation, comps.version, comps.format), true);
    }).then([this] {
        start_rewrite();
        trigger_compaction();
        // Drop entire cache for this column family because it may be populated
        // with stale data.
        return get_row_cache().clear();
    });
}

// FIXME: this is just an example, should be changed to something more general
// Note: We assume that the column_family does not get destroyed during compaction.
future<>
column_family::compact_all_sstables() {
    std::vector<sstables::shared_sstable> sstables;
    sstables.reserve(_sstables->all()->size());
    for (auto&& sst : *_sstables->all()) {
        sstables.push_back(sst);
    }
    // FIXME: check if the lower bound min_compaction_threshold() from schema
    // should be taken into account before proceeding with compaction.
    return compact_sstables(sstables::compaction_descriptor(std::move(sstables)));
}

void column_family::start_compaction() {
    set_compaction_strategy(_schema->compaction_strategy());
}

void column_family::trigger_compaction() {
    // Submitting compaction job to compaction manager.
    do_trigger_compaction(); // see below
}

void column_family::do_trigger_compaction() {
    // But only submit if we're not locked out
    if (!_compaction_disabled) {
        _compaction_manager.submit(this);
    }
}

future<> column_family::run_compaction(sstables::compaction_descriptor descriptor) {
    return compact_sstables(std::move(descriptor));
}

void column_family::set_compaction_strategy(sstables::compaction_strategy_type strategy) {
    dblog.info("Setting compaction strategy of {}.{} to {}", _schema->ks_name(), _schema->cf_name(), sstables::compaction_strategy::name(strategy));
    auto new_cs = make_compaction_strategy(strategy, _schema->compaction_strategy_options());
    auto new_sstables = new_cs.make_sstable_set(_schema);
    for (auto&& s : *_sstables->all()) {
        new_sstables.insert(s);
    }
    // now exception safe:
    _compaction_strategy = std::move(new_cs);
    _sstables = std::move(new_sstables);
}

size_t column_family::sstables_count() const {
    return _sstables->all()->size();
}

std::vector<uint64_t> column_family::sstable_count_per_level() const {
    std::vector<uint64_t> count_per_level;
    for (auto&& sst : *_sstables->all()) {
        auto level = sst->get_sstable_level();

        if (level + 1 > count_per_level.size()) {
            count_per_level.resize(level + 1, 0UL);
        }
        count_per_level[level]++;
    }
    return count_per_level;
}

int64_t column_family::get_unleveled_sstables() const {
    // TODO: when we support leveled compaction, we should return the number of
    // SSTables in L0. If leveled compaction is enabled in this column family,
    // then we should return zero, as we currently do.
    return 0;
}

lw_shared_ptr<sstable_list> column_family::get_sstables() const {
    return _sstables->all();
}

std::vector<sstables::shared_sstable> column_family::select_sstables(const query::partition_range& range) const {
    return _sstables->select(range);
}

// Gets the list of all sstables in the column family, including ones that are
// not used for active queries because they have already been compacted, but are
// waiting for delete_atomically() to return.
//
// As long as we haven't deleted them, compaction needs to ensure it doesn't
// garbage-collect a tombstone that covers data in an sstable that may not be
// successfully deleted.
lw_shared_ptr<sstable_list> column_family::get_sstables_including_compacted_undeleted() const {
    if (_sstables_compacted_but_not_deleted.empty()) {
        return get_sstables();
    }
    auto ret = make_lw_shared(*_sstables->all());
    for (auto&& s : _sstables_compacted_but_not_deleted) {
        ret->insert(s);
    }
    return ret;
}

inline bool column_family::manifest_json_filter(const sstring& fname) {
    using namespace boost::filesystem;

    path entry_path(fname);
    if (!is_directory(status(entry_path)) && entry_path.filename() == path("manifest.json")) {
        return false;
    }

    return true;
}

future<> column_family::populate(sstring sstdir) {
    // We can catch most errors when we try to load an sstable. But if the TOC
    // file is the one missing, we won't try to load the sstable at all. This
    // case is still an invalid case, but it is way easier for us to treat it
    // by waiting for all files to be loaded, and then checking if we saw a
    // file during scan_dir, without its corresponding TOC.
    enum class status {
        has_some_file,
        has_toc_file,
        has_temporary_toc_file,
    };

    struct sstable_descriptor {
        std::experimental::optional<sstables::sstable::version_types> version;
        std::experimental::optional<sstables::sstable::format_types> format;
    };

    auto verifier = make_lw_shared<std::unordered_map<unsigned long, status>>();
    auto descriptor = make_lw_shared<sstable_descriptor>();

    return do_with(std::vector<future<>>(), [this, sstdir, verifier, descriptor] (std::vector<future<>>& futures) {
        return lister::scan_dir(sstdir, { directory_entry_type::regular }, [this, sstdir, verifier, descriptor, &futures] (directory_entry de) {
            // FIXME: The secondary indexes are in this level, but with a directory type, (starting with ".")
            auto f = probe_file(sstdir, de.name).then([verifier, descriptor, sstdir, de] (auto entry) {
                auto filename = sstdir + "/" + de.name;
                if (entry.component == sstables::sstable::component_type::TemporaryStatistics) {
                    return remove_file(sstables::sstable::filename(sstdir, entry.ks, entry.cf, entry.version, entry.generation,
                        entry.format, sstables::sstable::component_type::TemporaryStatistics));
                }

                if (verifier->count(entry.generation)) {
                    if (verifier->at(entry.generation) == status::has_toc_file) {
                        if (entry.component == sstables::sstable::component_type::TOC) {
                            throw sstables::malformed_sstable_exception("Invalid State encountered. TOC file already processed", filename);
                        } else if (entry.component == sstables::sstable::component_type::TemporaryTOC) {
                            throw sstables::malformed_sstable_exception("Invalid State encountered. Temporary TOC file found after TOC file was processed", filename);
                        }
                    } else if (entry.component == sstables::sstable::component_type::TOC) {
                        verifier->at(entry.generation) = status::has_toc_file;
                    } else if (entry.component == sstables::sstable::component_type::TemporaryTOC) {
                        verifier->at(entry.generation) = status::has_temporary_toc_file;
                    }
                } else {
                    if (entry.component == sstables::sstable::component_type::TOC) {
                        verifier->emplace(entry.generation, status::has_toc_file);
                    } else if (entry.component == sstables::sstable::component_type::TemporaryTOC) {
                        verifier->emplace(entry.generation, status::has_temporary_toc_file);
                    } else {
                        verifier->emplace(entry.generation, status::has_some_file);
                    }
                }

                // Retrieve both version and format used for this column family.
                if (!descriptor->version) {
                    descriptor->version = entry.version;
                }
                if (!descriptor->format) {
                    descriptor->format = entry.format;
                }
                return make_ready_future<>();
            });

            // push future returned by probe_file into an array of futures,
            // so that the supplied callback will not block scan_dir() from
            // reading the next entry in the directory.
            futures.push_back(std::move(f));

            return make_ready_future<>();
        }, &manifest_json_filter).then([&futures] {
            return when_all(futures.begin(), futures.end()).then([] (std::vector<future<>> ret) {
                std::exception_ptr eptr;

                for (auto& f : ret) {
                    try {
                        if (eptr) {
                            f.ignore_ready_future();
                        } else {
                            f.get();
                        }
                    } catch(...) {
                        eptr = std::current_exception();
                    }
                }

                if (eptr) {
                    return make_exception_future<>(eptr);
                }
                return make_ready_future<>();
            });
        }).then([verifier, sstdir, descriptor, this] {
            return parallel_for_each(*verifier, [sstdir = std::move(sstdir), descriptor, this] (auto v) {
                if (v.second == status::has_temporary_toc_file) {
                    unsigned long gen = v.first;
                    assert(descriptor->version);
                    sstables::sstable::version_types version = descriptor->version.value();
                    assert(descriptor->format);
                    sstables::sstable::format_types format = descriptor->format.value();

                    if (engine().cpu_id() != 0) {
                        dblog.debug("At directory: {}, partial SSTable with generation {} not relevant for this shard, ignoring", sstdir, v.first);
                        return make_ready_future<>();
                    }
                    // shard 0 is the responsible for removing a partial sstable.
                    return sstables::sstable::remove_sstable_with_temp_toc(_schema->ks_name(), _schema->cf_name(), sstdir, gen, version, format);
                } else if (v.second != status::has_toc_file) {
                    throw sstables::malformed_sstable_exception(sprint("At directory: %s: no TOC found for SSTable with generation %d!. Refusing to boot", sstdir, v.first));
                }
                return make_ready_future<>();
            });
        });
    }).then([this] {
        // Make sure this is called even if CF is empty
        mark_ready_for_writes();
    });
}

utils::UUID database::empty_version = utils::UUID_gen::get_name_UUID(bytes{});

database::database() : database(db::config())
{}

database::database(const db::config& cfg)
    : _cfg(std::make_unique<db::config>(cfg))
    , _memtable_total_space([this] {
        _stats = make_lw_shared<db_stats>();

        auto memtable_total_space = size_t(_cfg->memtable_total_space_in_mb()) << 20;
        if (!memtable_total_space) {
            return memory::stats().total_memory() / 2;
        }
        return memtable_total_space;
    }())
    , _streaming_memtable_total_space(_memtable_total_space / 4)
    // Allow system tables a pool of 10 MB extra memory to write over the threshold. Under normal
    // circumnstances it won't matter, but when we throttle, some system requests will be able to
    // keep being serviced even if user requests are not.
    //
    // Note that even if we didn't allow extra memory, we would still want to keep system requests
    // in a different region group. This is because throttled requests are serviced in FIFO order,
    // and we don't want system requests to be waiting for a long time behind user requests.
    , _system_dirty_memory_manager(*this, _memtable_total_space / 2 + (10 << 20))
    // The total space that can be used by memtables is _memtable_total_space, but we will only
    // allow the region_group to grow to half of that. This is because of virtual_dirty: memtables
    // can take a long time to flush, and if we are using the maximum amount of memory possible,
    // then requests will block until we finish flushing at least one memtable.
    //
    // We can free memory until the whole memtable is flushed because we need to keep it in memory
    // until the end, but we can fake freeing memory. When we are done with an element of the
    // memtable, we will update the region group pretending memory just went down by that amount.
    //
    // Because the amount of memory that we pretend to free should be close enough to the actual
    // memory used by the memtables, that effectively creates two sub-regions inside the dirty
    // region group, of equal size. In the worst case, we will have _memtable_total_space dirty
    // bytes used, and half of that already virtually freed.
    , _dirty_memory_manager(*this, &_system_dirty_memory_manager, _memtable_total_space / 2)
    // The same goes for streaming in respect to virtual dirty.
    , _streaming_dirty_memory_manager(*this, &_dirty_memory_manager, _streaming_memtable_total_space / 2)
    , _version(empty_version)
    , _enable_incremental_backups(cfg.incremental_backups())
{
    _compaction_manager.start();
    setup_collectd();

    dblog.info("Row: max_vector_size: {}, internal_count: {}", size_t(row::max_vector_size), size_t(row::internal_count));
}

void
database::setup_collectd() {
    _collectd.push_back(
        scollectd::add_polled_metric(scollectd::type_instance_id("memory"
                , scollectd::per_cpu_plugin_instance
                , "bytes", "dirty")
                , scollectd::make_typed(scollectd::data_type::GAUGE, [this] {
            return _dirty_memory_manager.real_dirty_memory();
    })));

    _collectd.push_back(
        scollectd::add_polled_metric(scollectd::type_instance_id("memory"
                , scollectd::per_cpu_plugin_instance
                , "bytes", "virtual_dirty")
                , scollectd::make_typed(scollectd::data_type::GAUGE, [this] {
            return _dirty_memory_manager.virtual_dirty_memory();
    })));

    _collectd.push_back(
        scollectd::add_polled_metric(scollectd::type_instance_id("memtables"
                , scollectd::per_cpu_plugin_instance
                , "queue_length", "pending_flushes")
                , scollectd::make_typed(scollectd::data_type::GAUGE, _cf_stats.pending_memtables_flushes_count)
    ));

    _collectd.push_back(
        scollectd::add_polled_metric(scollectd::type_instance_id("database"
                , scollectd::per_cpu_plugin_instance
                , "queue_length", "requests_blocked_memory")
                , scollectd::make_typed(scollectd::data_type::GAUGE, [this] {
                    return _dirty_memory_manager.region_group().blocked_requests();
                })
    ));

    _collectd.push_back(
        scollectd::add_polled_metric(scollectd::type_instance_id("database"
                , scollectd::per_cpu_plugin_instance
                , "total_operations", "requests_blocked_memory")
                , scollectd::make_typed(scollectd::data_type::DERIVE, [this] {
                    return _dirty_memory_manager.region_group().blocked_requests_counter();
                })
    ));

    _collectd.push_back(
        scollectd::add_polled_metric(scollectd::type_instance_id("memtables"
                , scollectd::per_cpu_plugin_instance
                , "bytes", "pending_flushes")
                , scollectd::make_typed(scollectd::data_type::GAUGE, _cf_stats.pending_memtables_flushes_bytes)
    ));

    _collectd.push_back(
        scollectd::add_polled_metric(scollectd::type_instance_id("database"
                , scollectd::per_cpu_plugin_instance
                , "total_operations", "clustering_filter_count")
                , scollectd::make_typed(scollectd::data_type::DERIVE, _cf_stats.clustering_filter_count)
    ));

    _collectd.push_back(
        scollectd::add_polled_metric(scollectd::type_instance_id("database"
                , scollectd::per_cpu_plugin_instance
                , "total_operations", "clustering_filter_sstables_checked")
                , scollectd::make_typed(scollectd::data_type::DERIVE, _cf_stats.sstables_checked_by_clustering_filter)
    ));

    _collectd.push_back(
        scollectd::add_polled_metric(scollectd::type_instance_id("database"
                , scollectd::per_cpu_plugin_instance
                , "total_operations", "clustering_filter_fast_path_count")
                , scollectd::make_typed(scollectd::data_type::DERIVE, _cf_stats.clustering_filter_fast_path_count)
    ));

    _collectd.push_back(
        scollectd::add_polled_metric(scollectd::type_instance_id("database"
                , scollectd::per_cpu_plugin_instance
                , "total_operations", "clustering_filter_surviving_sstables")
                , scollectd::make_typed(scollectd::data_type::DERIVE, _cf_stats.surviving_sstables_after_clustering_filter)
    ));

    _collectd.push_back(
        scollectd::add_polled_metric(scollectd::type_instance_id("database"
                , scollectd::per_cpu_plugin_instance
                , "total_operations", "total_writes")
                , scollectd::make_typed(scollectd::data_type::DERIVE, _stats->total_writes)
    ));

    _collectd.push_back(
        scollectd::add_polled_metric(scollectd::type_instance_id("database"
                , scollectd::per_cpu_plugin_instance
                , "total_operations", "total_reads")
                , scollectd::make_typed(scollectd::data_type::DERIVE, _stats->total_reads)
    ));

    _collectd.push_back(
        scollectd::add_polled_metric(scollectd::type_instance_id("database"
                , scollectd::per_cpu_plugin_instance
                , "total_operations", "sstable_read_queue_overloads")
                , scollectd::make_typed(scollectd::data_type::COUNTER, _stats->sstable_read_queue_overloaded)
    ));

    _collectd.push_back(
        scollectd::add_polled_metric(scollectd::type_instance_id("database"
                , scollectd::per_cpu_plugin_instance
                , "queue_length", "active_reads")
                , scollectd::make_typed(scollectd::data_type::GAUGE, [this] { return max_concurrent_reads() - _read_concurrency_sem.current(); })
    ));
    _collectd.push_back(
        scollectd::add_polled_metric(scollectd::type_instance_id("database"
                , scollectd::per_cpu_plugin_instance
                , "queue_length", "queued_reads")
                , scollectd::make_typed(scollectd::data_type::GAUGE, [this] { return _read_concurrency_sem.waiters(); })
    ));
    _collectd.push_back(
        scollectd::add_polled_metric(scollectd::type_instance_id("database"
                , scollectd::per_cpu_plugin_instance
                , "queue_length", "active_reads_system_keyspace")
                , scollectd::make_typed(scollectd::data_type::GAUGE, [this] { return max_system_concurrent_reads() - _system_read_concurrency_sem.current(); })
    ));
    _collectd.push_back(
        scollectd::add_polled_metric(scollectd::type_instance_id("database"
                , scollectd::per_cpu_plugin_instance
                , "queue_length", "queued_reads_system_keyspace")
                , scollectd::make_typed(scollectd::data_type::GAUGE, [this] { return _system_read_concurrency_sem.waiters(); })
    ));
}

database::~database() {
}

void database::update_version(const utils::UUID& version) {
    _version = version;
}

const utils::UUID& database::get_version() const {
    return _version;
}

future<> database::populate_keyspace(sstring datadir, sstring ks_name) {
    auto ksdir = datadir + "/" + ks_name;
    auto i = _keyspaces.find(ks_name);
    if (i == _keyspaces.end()) {
        dblog.warn("Skipping undefined keyspace: {}", ks_name);
        return make_ready_future<>();
    } else {
        dblog.info("Populating Keyspace {}", ks_name);
        auto& ks = i->second;
        return parallel_for_each(ks.metadata()->cf_meta_data() | boost::adaptors::map_values,
            [ks_name, &ks, this] (schema_ptr s) {
                utils::UUID uuid = s->id();
                lw_shared_ptr<column_family> cf = _column_families[uuid];
                sstring cfname = cf->schema()->cf_name();
                auto sstdir = ks.column_family_directory(cfname, uuid);
                dblog.info("Keyspace {}: Reading CF {} ", ks_name, cfname);
                return ks.make_directory_for_column_family(cfname, uuid).then([cf, sstdir] {
                    return cf->populate(sstdir);
                }).handle_exception([ks_name, cfname, sstdir](std::exception_ptr eptr) {
                    std::string msg =
                        sprint("Exception while populating keyspace '%s' with column family '%s' from file '%s': %s",
                               ks_name, cfname, sstdir, eptr);
                    dblog.error("Exception while populating keyspace '{}' with column family '{}' from file '{}': {}",
                                ks_name, cfname, sstdir, eptr);
                    throw std::runtime_error(msg.c_str());
                });
            });
    }
}

future<> database::populate(sstring datadir) {
    return lister::scan_dir(datadir, { directory_entry_type::directory }, [this, datadir] (directory_entry de) {
        auto& ks_name = de.name;
        if (ks_name == "system") {
            return make_ready_future<>();
        }
        return populate_keyspace(datadir, ks_name);
    });
}

template <typename Func>
static future<>
do_parse_system_tables(distributed<service::storage_proxy>& proxy, const sstring& _cf_name, Func&& func) {
    using namespace db::schema_tables;
    static_assert(std::is_same<future<>, std::result_of_t<Func(schema_result_value_type&)>>::value,
                  "bad Func signature");


    auto cf_name = make_lw_shared<sstring>(_cf_name);
    return db::system_keyspace::query(proxy, *cf_name).then([] (auto rs) {
        auto names = std::set<sstring>();
        for (auto& r : rs->rows()) {
            auto keyspace_name = r.template get_nonnull<sstring>("keyspace_name");
            names.emplace(keyspace_name);
        }
        return std::move(names);
    }).then([&proxy, cf_name, func = std::forward<Func>(func)] (std::set<sstring>&& names) mutable {
        return parallel_for_each(names.begin(), names.end(), [&proxy, cf_name, func = std::forward<Func>(func)] (sstring name) mutable {
            if (name == "system") {
                return make_ready_future<>();
            }

            return read_schema_partition_for_keyspace(proxy, *cf_name, name).then([func, cf_name] (auto&& v) mutable {
                return do_with(std::move(v), [func = std::forward<Func>(func), cf_name] (auto& v) {
                    return func(v).then_wrapped([cf_name, &v] (future<> f) {
                        try {
                            f.get();
                        } catch (std::exception& e) {
                            dblog.error("Skipping: {}. Exception occurred when loading system table {}: {}", v.first, *cf_name, e.what());
                        }
                    });
                });
            });
        });
    });
}

future<> database::parse_system_tables(distributed<service::storage_proxy>& proxy) {
    using namespace db::schema_tables;
    return do_parse_system_tables(proxy, db::schema_tables::KEYSPACES, [this] (schema_result_value_type &v) {
        auto ksm = create_keyspace_from_schema_partition(v);
        return create_keyspace(ksm);
    }).then([&proxy, this] {
        return do_parse_system_tables(proxy, db::schema_tables::USERTYPES, [this, &proxy] (schema_result_value_type &v) {
            auto&& user_types = create_types_from_schema_partition(v);
            auto& ks = this->find_keyspace(v.first);
            for (auto&& type : user_types) {
                ks.add_user_type(type);
            }
            return make_ready_future<>();
        });
    }).then([&proxy, this] {
        return do_parse_system_tables(proxy, db::schema_tables::COLUMNFAMILIES, [this, &proxy] (schema_result_value_type &v) {
            return create_tables_from_tables_partition(proxy, v.second).then([this] (std::map<sstring, schema_ptr> tables) {
                return parallel_for_each(tables.begin(), tables.end(), [this] (auto& t) {
                    auto s = t.second;
                    auto& ks = this->find_keyspace(s->ks_name());
                    auto cfg = ks.make_column_family_config(*s, this->get_config());
                    this->add_column_family(s, std::move(cfg));
                    return ks.make_directory_for_column_family(s->cf_name(), s->id()).then([s] {});
                });
            });
        });
    });
}

future<>
database::init_system_keyspace() {
    bool durable = _cfg->data_file_directories().size() > 0;
    db::system_keyspace::make(*this, durable, _cfg->volatile_system_keyspace_for_testing());

    // FIXME support multiple directories
    return io_check(touch_directory, _cfg->data_file_directories()[0] + "/" + db::system_keyspace::NAME).then([this] {
        return populate_keyspace(_cfg->data_file_directories()[0], db::system_keyspace::NAME).then([this]() {
            return init_commitlog();
        });
    }).then([this] {
        auto& ks = find_keyspace(db::system_keyspace::NAME);
        return parallel_for_each(ks.metadata()->cf_meta_data(), [this] (auto& pair) {
            auto cfm = pair.second;
            auto& cf = this->find_column_family(cfm);
            cf.mark_ready_for_writes();
            return make_ready_future<>();
        });
    });
}

future<>
database::load_sstables(distributed<service::storage_proxy>& proxy) {
	return parse_system_tables(proxy).then([this] {
		return populate(_cfg->data_file_directories()[0]);
	});
}

future<>
database::init_commitlog() {
    return db::commitlog::create_commitlog(*_cfg).then([this](db::commitlog&& log) {
        _commitlog = std::make_unique<db::commitlog>(std::move(log));
        _commitlog->add_flush_handler([this](db::cf_id_type id, db::replay_position pos) {
            if (_column_families.count(id) == 0) {
                // the CF has been removed.
                _commitlog->discard_completed_segments(id, pos);
                return;
            }
            _column_families[id]->flush(pos);
        }).release(); // we have longer life time than CL. Ignore reg anchor
    });
}

unsigned
database::shard_of(const dht::token& t) {
    return dht::shard_of(t);
}

unsigned
database::shard_of(const mutation& m) {
    return shard_of(m.token());
}

unsigned
database::shard_of(const frozen_mutation& m) {
    // FIXME: This lookup wouldn't be necessary if we
    // sent the partition key in legacy form or together
    // with token.
    schema_ptr schema = find_schema(m.column_family_id());
    return shard_of(dht::global_partitioner().get_token(*schema, m.key(*schema)));
}

void database::add_keyspace(sstring name, keyspace k) {
    if (_keyspaces.count(name) != 0) {
        throw std::invalid_argument("Keyspace " + name + " already exists");
    }
    _keyspaces.emplace(std::move(name), std::move(k));
}

future<> database::update_keyspace(const sstring& name) {
    auto& proxy = service::get_storage_proxy();
    return db::schema_tables::read_schema_partition_for_keyspace(proxy, db::schema_tables::KEYSPACES, name).then([this, name](db::schema_tables::schema_result_value_type&& v) {
        auto& ks = find_keyspace(name);

        auto tmp_ksm = db::schema_tables::create_keyspace_from_schema_partition(v);
        auto new_ksm = ::make_lw_shared<keyspace_metadata>(tmp_ksm->name(), tmp_ksm->strategy_name(), tmp_ksm->strategy_options(), tmp_ksm->durable_writes(),
                        boost::copy_range<std::vector<schema_ptr>>(ks.metadata()->cf_meta_data() | boost::adaptors::map_values), ks.metadata()->user_types());
        ks.update_from(std::move(new_ksm));
        return service::get_local_migration_manager().notify_update_keyspace(ks.metadata());
    });
}

void database::drop_keyspace(const sstring& name) {
    _keyspaces.erase(name);
}

void database::add_column_family(schema_ptr schema, column_family::config cfg) {
    schema = local_schema_registry().learn(schema);
    schema->registry_entry()->mark_synced();
    auto uuid = schema->id();
    lw_shared_ptr<column_family> cf;
    if (cfg.enable_commitlog && _commitlog) {
       cf = make_lw_shared<column_family>(schema, std::move(cfg), *_commitlog, _compaction_manager);
    } else {
       cf = make_lw_shared<column_family>(schema, std::move(cfg), column_family::no_commitlog(), _compaction_manager);
    }

    auto ks = _keyspaces.find(schema->ks_name());
    if (ks == _keyspaces.end()) {
        throw std::invalid_argument("Keyspace " + schema->ks_name() + " not defined");
    }
    if (_column_families.count(uuid) != 0) {
        throw std::invalid_argument("UUID " + uuid.to_sstring() + " already mapped");
    }
    auto kscf = std::make_pair(schema->ks_name(), schema->cf_name());
    if (_ks_cf_to_uuid.count(kscf) != 0) {
        throw std::invalid_argument("Column family " + schema->cf_name() + " exists");
    }
    ks->second.add_or_update_column_family(schema);
    cf->start();
    _column_families.emplace(uuid, std::move(cf));
    _ks_cf_to_uuid.emplace(std::move(kscf), uuid);
}

future<> database::drop_column_family(const sstring& ks_name, const sstring& cf_name, timestamp_func tsf) {
    auto uuid = find_uuid(ks_name, cf_name);
    auto& ks = find_keyspace(ks_name);
    auto cf = _column_families.at(uuid);
    _column_families.erase(uuid);
    ks.metadata()->remove_column_family(cf->schema());
    _ks_cf_to_uuid.erase(std::make_pair(ks_name, cf_name));
    return truncate(ks, *cf, std::move(tsf)).then([this, cf] {
        return cf->stop();
    }).then([this, cf] {
        return make_ready_future<>();
    });
}

const utils::UUID& database::find_uuid(const sstring& ks, const sstring& cf) const {
    try {
        return _ks_cf_to_uuid.at(std::make_pair(ks, cf));
    } catch (...) {
        throw std::out_of_range("");
    }
}

const utils::UUID& database::find_uuid(const schema_ptr& schema) const {
    return find_uuid(schema->ks_name(), schema->cf_name());
}

keyspace& database::find_keyspace(const sstring& name) {
    try {
        return _keyspaces.at(name);
    } catch (...) {
        std::throw_with_nested(no_such_keyspace(name));
    }
}

const keyspace& database::find_keyspace(const sstring& name) const {
    try {
        return _keyspaces.at(name);
    } catch (...) {
        std::throw_with_nested(no_such_keyspace(name));
    }
}

bool database::has_keyspace(const sstring& name) const {
    return _keyspaces.count(name) != 0;
}

std::vector<sstring>  database::get_non_system_keyspaces() const {
    std::vector<sstring> res;
    for (auto const &i : _keyspaces) {
        if (i.first != db::system_keyspace::NAME) {
            res.push_back(i.first);
        }
    }
    return res;
}

std::vector<lw_shared_ptr<column_family>> database::get_non_system_column_families() const {
    return boost::copy_range<std::vector<lw_shared_ptr<column_family>>>(
        get_column_families()
            | boost::adaptors::map_values
            | boost::adaptors::filtered([](const lw_shared_ptr<column_family>& cf) {
                return cf->schema()->ks_name() != db::system_keyspace::NAME;
            }));
}

column_family& database::find_column_family(const sstring& ks_name, const sstring& cf_name) {
    try {
        return find_column_family(find_uuid(ks_name, cf_name));
    } catch (...) {
        std::throw_with_nested(no_such_column_family(ks_name, cf_name));
    }
}

const column_family& database::find_column_family(const sstring& ks_name, const sstring& cf_name) const {
    try {
        return find_column_family(find_uuid(ks_name, cf_name));
    } catch (...) {
        std::throw_with_nested(no_such_column_family(ks_name, cf_name));
    }
}

column_family& database::find_column_family(const utils::UUID& uuid) {
    try {
        return *_column_families.at(uuid);
    } catch (...) {
        std::throw_with_nested(no_such_column_family(uuid));
    }
}

const column_family& database::find_column_family(const utils::UUID& uuid) const {
    try {
        return *_column_families.at(uuid);
    } catch (...) {
        std::throw_with_nested(no_such_column_family(uuid));
    }
}

bool database::column_family_exists(const utils::UUID& uuid) const {
    return _column_families.count(uuid);
}

void
keyspace::create_replication_strategy(const std::map<sstring, sstring>& options) {
    using namespace locator;

    auto& ss = service::get_local_storage_service();
    _replication_strategy =
            abstract_replication_strategy::create_replication_strategy(
                _metadata->name(), _metadata->strategy_name(),
                ss.get_token_metadata(), options);
}

locator::abstract_replication_strategy&
keyspace::get_replication_strategy() {
    return *_replication_strategy;
}


const locator::abstract_replication_strategy&
keyspace::get_replication_strategy() const {
    return *_replication_strategy;
}

void
keyspace::set_replication_strategy(std::unique_ptr<locator::abstract_replication_strategy> replication_strategy) {
    _replication_strategy = std::move(replication_strategy);
}

void keyspace::update_from(::lw_shared_ptr<keyspace_metadata> ksm) {
    _metadata = std::move(ksm);
   create_replication_strategy(_metadata->strategy_options());
}

column_family::config
keyspace::make_column_family_config(const schema& s, const db::config& db_config) const {
    column_family::config cfg;
    cfg.datadir = column_family_directory(s.cf_name(), s.id());
    cfg.enable_disk_reads = _config.enable_disk_reads;
    cfg.enable_disk_writes = _config.enable_disk_writes;
    cfg.enable_commitlog = _config.enable_commitlog;
    cfg.enable_cache = _config.enable_cache;
    cfg.dirty_memory_manager = _config.dirty_memory_manager;
    cfg.streaming_dirty_memory_manager = _config.streaming_dirty_memory_manager;
    cfg.read_concurrency_config = _config.read_concurrency_config;
    cfg.streaming_read_concurrency_config = _config.streaming_read_concurrency_config;
    cfg.cf_stats = _config.cf_stats;
    cfg.enable_incremental_backups = _config.enable_incremental_backups;
    cfg.max_cached_partition_size_in_bytes = db_config.max_cached_partition_size_in_kb() * 1024;

    return cfg;
}

sstring
keyspace::column_family_directory(const sstring& name, utils::UUID uuid) const {
    auto uuid_sstring = uuid.to_sstring();
    boost::erase_all(uuid_sstring, "-");
    return sprint("%s/%s-%s", _config.datadir, name, uuid_sstring);
}

future<>
keyspace::make_directory_for_column_family(const sstring& name, utils::UUID uuid) {
    auto cfdir = column_family_directory(name, uuid);
    return seastar::async([cfdir = std::move(cfdir)] {
        io_check(touch_directory, cfdir).get();
        io_check(touch_directory, cfdir + "/upload").get();
    });
}

no_such_keyspace::no_such_keyspace(const sstring& ks_name)
    : runtime_error{sprint("Can't find a keyspace %s", ks_name)}
{
}

no_such_column_family::no_such_column_family(const utils::UUID& uuid)
    : runtime_error{sprint("Can't find a column family with UUID %s", uuid)}
{
}

no_such_column_family::no_such_column_family(const sstring& ks_name, const sstring& cf_name)
    : runtime_error{sprint("Can't find a column family %s in keyspace %s", cf_name, ks_name)}
{
}

column_family& database::find_column_family(const schema_ptr& schema) {
    return find_column_family(schema->id());
}

const column_family& database::find_column_family(const schema_ptr& schema) const {
    return find_column_family(schema->id());
}

void keyspace_metadata::validate() const {
    using namespace locator;

    auto& ss = service::get_local_storage_service();
    abstract_replication_strategy::validate_replication_strategy(name(), strategy_name(), ss.get_token_metadata(), strategy_options());
}

schema_ptr database::find_schema(const sstring& ks_name, const sstring& cf_name) const {
    try {
        return find_schema(find_uuid(ks_name, cf_name));
    } catch (std::out_of_range&) {
        std::throw_with_nested(no_such_column_family(ks_name, cf_name));
    }
}

schema_ptr database::find_schema(const utils::UUID& uuid) const {
    return find_column_family(uuid).schema();
}

bool database::has_schema(const sstring& ks_name, const sstring& cf_name) const {
    return _ks_cf_to_uuid.count(std::make_pair(ks_name, cf_name)) > 0;
}


void database::create_in_memory_keyspace(const lw_shared_ptr<keyspace_metadata>& ksm) {
    keyspace ks(ksm, std::move(make_keyspace_config(*ksm)));
    ks.create_replication_strategy(ksm->strategy_options());
    _keyspaces.emplace(ksm->name(), std::move(ks));
}

future<>
database::create_keyspace(const lw_shared_ptr<keyspace_metadata>& ksm) {
    auto i = _keyspaces.find(ksm->name());
    if (i != _keyspaces.end()) {
        return make_ready_future<>();
    }

    create_in_memory_keyspace(ksm);
    auto& datadir = _keyspaces.at(ksm->name()).datadir();
    if (datadir != "") {
        return io_check(touch_directory, datadir);
    } else {
        return make_ready_future<>();
    }
}

std::set<sstring>
database::existing_index_names(const sstring& cf_to_exclude) const {
    std::set<sstring> names;
    for (auto& p : _column_families) {
        auto& cf = *p.second;
        if (!cf_to_exclude.empty() && cf.schema()->cf_name() == cf_to_exclude) {
            continue;
        }
        for (auto& cd : cf.schema()->all_columns_in_select_order()) {
            if (cd.idx_info.index_name) {
                names.emplace(*cd.idx_info.index_name);
            }
        }
    }
    return names;
}

// Based on:
//  - org.apache.cassandra.db.AbstractCell#reconcile()
//  - org.apache.cassandra.db.BufferExpiringCell#reconcile()
//  - org.apache.cassandra.db.BufferDeletedCell#reconcile()
int
compare_atomic_cell_for_merge(atomic_cell_view left, atomic_cell_view right) {
    if (left.timestamp() != right.timestamp()) {
        return left.timestamp() > right.timestamp() ? 1 : -1;
    }
    if (left.is_live() != right.is_live()) {
        return left.is_live() ? -1 : 1;
    }
    if (left.is_live()) {
        auto c = compare_unsigned(left.value(), right.value());
        if (c != 0) {
            return c;
        }
        if (left.is_live_and_has_ttl()
            && right.is_live_and_has_ttl()
            && left.expiry() != right.expiry())
        {
            return left.expiry() < right.expiry() ? -1 : 1;
        }
    } else {
        // Both are deleted
        if (left.deletion_time() != right.deletion_time()) {
            // Origin compares big-endian serialized deletion time. That's because it
            // delegates to AbstractCell.reconcile() which compares values after
            // comparing timestamps, which in case of deleted cells will hold
            // serialized expiry.
            return (uint32_t) left.deletion_time().time_since_epoch().count()
                   < (uint32_t) right.deletion_time().time_since_epoch().count() ? -1 : 1;
        }
    }
    return 0;
}

struct query_state {
    explicit query_state(schema_ptr s,
                         const query::read_command& cmd,
                         query::result_request request,
                         const std::vector<query::partition_range>& ranges)
            : schema(std::move(s))
            , cmd(cmd)
            , builder(cmd.slice, request)
            , limit(cmd.row_limit)
            , partition_limit(cmd.partition_limit)
            , current_partition_range(ranges.begin())
            , range_end(ranges.end()){
    }
    schema_ptr schema;
    const query::read_command& cmd;
    query::result::builder builder;
    uint32_t limit;
    uint32_t partition_limit;
    bool range_empty = false;   // Avoid ubsan false-positive when moving after construction
    std::vector<query::partition_range>::const_iterator current_partition_range;
    std::vector<query::partition_range>::const_iterator range_end;
    mutation_reader reader;
    bool done() const {
        return !limit || current_partition_range == range_end;
    }
};

future<lw_shared_ptr<query::result>>
column_family::query(schema_ptr s, const query::read_command& cmd, query::result_request request, const std::vector<query::partition_range>& partition_ranges, tracing::trace_state_ptr trace_state) {
    utils::latency_counter lc;
    _stats.reads.set_latency(lc);
    auto qs_ptr = std::make_unique<query_state>(std::move(s), cmd, request, partition_ranges);
    auto& qs = *qs_ptr;
    {
        return do_until(std::bind(&query_state::done, &qs), [this, &qs, trace_state = std::move(trace_state)] {
            auto&& range = *qs.current_partition_range++;
            return data_query(qs.schema, as_mutation_source(trace_state), range, qs.cmd.slice, qs.limit, qs.partition_limit,
                              qs.cmd.timestamp, qs.builder).then([&qs] (auto&& r) {
                qs.limit -= r.live_rows;
                qs.partition_limit -= r.partitions;
            });
        }).then([qs_ptr = std::move(qs_ptr), &qs] {
            return make_ready_future<lw_shared_ptr<query::result>>(
                    make_lw_shared<query::result>(qs.builder.build()));
        }).finally([lc, this]() mutable {
            _stats.reads.mark(lc);
            if (lc.is_start()) {
                _stats.estimated_read.add(lc.latency(), _stats.reads.hist.count);
            }
        });
    }
}

mutation_source
column_family::as_mutation_source(tracing::trace_state_ptr trace_state) const {
    return mutation_source([this, trace_state = std::move(trace_state)] (schema_ptr s,
                                   const query::partition_range& range,
                                   const query::partition_slice& slice,
                                   const io_priority_class& pc) {
        return this->make_reader(std::move(s), range, slice, pc, std::move(trace_state));
    });
}

future<lw_shared_ptr<query::result>>
database::query(schema_ptr s, const query::read_command& cmd, query::result_request request, const std::vector<query::partition_range>& ranges, tracing::trace_state_ptr trace_state) {
    column_family& cf = find_column_family(cmd.cf_id);
    return cf.query(std::move(s), cmd, request, ranges, std::move(trace_state)).then([this, s = _stats] (auto&& res) {
        ++s->total_reads;
        return std::move(res);
    });
}

future<reconcilable_result>
database::query_mutations(schema_ptr s, const query::read_command& cmd, const query::partition_range& range, tracing::trace_state_ptr trace_state) {
    column_family& cf = find_column_family(cmd.cf_id);
    return mutation_query(std::move(s), cf.as_mutation_source(std::move(trace_state)), range, cmd.slice, cmd.row_limit, cmd.partition_limit,
            cmd.timestamp).then([this, s = _stats] (auto&& res) {
        ++s->total_reads;
        return std::move(res);
    });
}

std::unordered_set<sstring> database::get_initial_tokens() {
    std::unordered_set<sstring> tokens;
    sstring tokens_string = get_config().initial_token();
    try {
        boost::split(tokens, tokens_string, boost::is_any_of(sstring(", ")));
    } catch (...) {
        throw std::runtime_error(sprint("Unable to parse initial_token=%s", tokens_string));
    }
    tokens.erase("");
    return tokens;
}

std::experimental::optional<gms::inet_address> database::get_replace_address() {
    auto& cfg = get_config();
    sstring replace_address = cfg.replace_address();
    sstring replace_address_first_boot = cfg.replace_address_first_boot();
    try {
        if (!replace_address.empty()) {
            return gms::inet_address(replace_address);
        } else if (!replace_address_first_boot.empty()) {
            return gms::inet_address(replace_address_first_boot);
        }
        return std::experimental::nullopt;
    } catch (...) {
        return std::experimental::nullopt;
    }
}

bool database::is_replacing() {
    sstring replace_address_first_boot = get_config().replace_address_first_boot();
    if (!replace_address_first_boot.empty() && db::system_keyspace::bootstrap_complete()) {
        dblog.info("Replace address on first boot requested; this node is already bootstrapped");
        return false;
    }
    return bool(get_replace_address());
}

std::ostream& operator<<(std::ostream& out, const atomic_cell_or_collection& c) {
    return out << to_hex(c._data);
}

std::ostream& operator<<(std::ostream& os, const mutation& m) {
    const ::schema& s = *m.schema();
    fprint(os, "{%s.%s key %s data ", s.ks_name(), s.cf_name(), m.decorated_key());
    os << m.partition() << "}";
    return os;
}

std::ostream& operator<<(std::ostream& out, const column_family& cf) {
    return fprint(out, "{column_family: %s/%s}", cf._schema->ks_name(), cf._schema->cf_name());
}

std::ostream& operator<<(std::ostream& out, const database& db) {
    out << "{\n";
    for (auto&& e : db._column_families) {
        auto&& cf = *e.second;
        out << "(" << e.first.to_sstring() << ", " << cf.schema()->cf_name() << ", " << cf.schema()->ks_name() << "): " << cf << "\n";
    }
    out << "}";
    return out;
}

void
column_family::apply(const mutation& m, const db::replay_position& rp) {
    utils::latency_counter lc;
    _stats.writes.set_latency(lc);
    _memtables->active_memtable().apply(m, rp);
    _stats.writes.mark(lc);
    if (lc.is_start()) {
        _stats.estimated_write.add(lc.latency(), _stats.writes.hist.count);
    }
}

void
column_family::apply(const frozen_mutation& m, const schema_ptr& m_schema, const db::replay_position& rp) {
    utils::latency_counter lc;
    _stats.writes.set_latency(lc);
    check_valid_rp(rp);
    _memtables->active_memtable().apply(m, m_schema, rp);
    _stats.writes.mark(lc);
    if (lc.is_start()) {
        _stats.estimated_write.add(lc.latency(), _stats.writes.hist.count);
    }
}

void column_family::apply_streaming_mutation(schema_ptr m_schema, utils::UUID plan_id, const frozen_mutation& m, bool fragmented) {
    if (fragmented) {
        apply_streaming_big_mutation(std::move(m_schema), plan_id, m);
        return;
    }
    _streaming_memtables->active_memtable().apply(m, m_schema);
}

void column_family::apply_streaming_big_mutation(schema_ptr m_schema, utils::UUID plan_id, const frozen_mutation& m) {
    auto it = _streaming_memtables_big.find(plan_id);
    if (it == _streaming_memtables_big.end()) {
        it = _streaming_memtables_big.emplace(plan_id, make_lw_shared<streaming_memtable_big>()).first;
        it->second->memtables = _config.enable_disk_writes ? make_streaming_memtable_big_list(*it->second) : make_memory_only_memtable_list();
    }
    auto entry = it->second;
    entry->memtables->active_memtable().apply(m, m_schema);
}

void
column_family::check_valid_rp(const db::replay_position& rp) const {
    if (rp < _highest_flushed_rp) {
        throw replay_position_reordered_exception();
    }
}

future<> dirty_memory_manager::shutdown() {
    _db_shutdown_requested = true;
    _should_flush.signal();
    return std::move(_waiting_flush).then([this] {
        return _region_group.shutdown();
    });
}

future<> memtable_list::request_flush() {
    if (!_flush_coalescing) {
        _flush_coalescing = shared_promise<>();
        return _dirty_memory_manager->get_flush_permit().then([this] (auto permit) {
            auto current_flush = std::move(*_flush_coalescing);
            _flush_coalescing = {};
            return _dirty_memory_manager->flush_one(*this, std::move(permit)).then_wrapped([this, current_flush = std::move(current_flush)] (auto f) mutable {
                if (f.failed()) {
                    current_flush.set_exception(f.get_exception());
                } else {
                    current_flush.set_value();
                }
            });
        });
    } else {
        return _flush_coalescing->get_shared_future();
    }
}

future<> dirty_memory_manager::flush_one(memtable_list& mtlist, semaphore_units<> permit) {
    if (mtlist.back()->empty()) {
        return make_ready_future<>();
    }

    auto* region = &(mtlist.back()->region());
    auto* region_group = mtlist.back()->region_group();
    auto schema = mtlist.back()->schema();
    // Because the region groups are hierarchical, when we pick the biggest region creating pressure
    // (in the memory-driven flush case) we may be picking a memtable that is placed in a region
    // group below ours. That's totally fine and we can certainly use our semaphore to account for
    // it, but we need to destroy the semaphore units from the right flush manager.
    //
    // If we abandon size-driven flush and go with another flushing scheme that always guarantees
    // that we're picking from this region_group, we can simplify this.
    dirty_memory_manager::from_region_group(region_group).add_to_flush_manager(region, std::move(permit));
    auto fut = mtlist.seal_active_memtable(memtable_list::flush_behavior::immediate);
    return get_units(_background_work_flush_serializer, 1).then([this, fut = std::move(fut), region, region_group, schema] (auto permit) mutable {
        return std::move(fut).then_wrapped([this, region, region_group, schema, permit = std::move(permit)] (auto f) {
            // There are two cases in which we may still need to remove the permits from here.
            //
            // 1) Some exception happenend, and we can't know at which point. It could be that because
            //    of that, the permits are still dangling. We have to remove it.
            // 2) If we are using a memory-only Column Family. That will never create a memtable
            //    flush object, and we'll never get rid of the permits. So we have to remove it
            //    here.
            dirty_memory_manager::from_region_group(region_group).remove_from_flush_manager(region);
            if (f.failed()) {
                dblog.error("Failed to flush memtable, {}:{}", schema->ks_name(), schema->cf_name());
            }
            return std::move(f);
        });
    });
}

future<> dirty_memory_manager::flush_when_needed() {
    if (!_db) {
        return make_ready_future<>();
    }
    // If there are explicit flushes requested, we must wait for them to finish before we stop.
    return do_until([this] { return _db_shutdown_requested; }, [this] {
        auto has_work = [this] { return over_soft_limit() || _db_shutdown_requested; };
        return _should_flush.wait(std::move(has_work)).then([this] {
            return get_flush_permit().then([this] (auto permit) {
                // We give priority to explicit flushes. They are mainly user-initiated flushes,
                // flushes coming from a DROP statement, or commitlog flushes.
                if (_flush_serializer.waiters()) {
                    return make_ready_future<>();
                }
                // condition abated while we waited for the semaphore
                if (!this->over_soft_limit() || _db_shutdown_requested) {
                    return make_ready_future<>();
                }
                // There are many criteria that can be used to select what is the best memtable to
                // flush. Most of the time we want some coordination with the commitlog to allow us to
                // release commitlog segments as early as we can.
                //
                // But during pressure condition, we'll just pick the CF that holds the largest
                // memtable. The advantage of doing this is that this is objectively the one that will
                // release the biggest amount of memory and is less likely to be generating tiny
                // SSTables.
                memtable& biggest_memtable = memtable::from_region(*(this->_region_group.get_largest_region()));
                auto mtlist = biggest_memtable.get_memtable_list();
                // Do not wait. The semaphore will protect us against a concurrent flush. But we
                // want to start a new one as soon as the permits are destroyed and the semaphore is
                // made ready again, not when we are done with the current one.
                this->flush_one(*mtlist, std::move(permit));
                return make_ready_future<>();
            });
        });
    }).finally([this] {
        // We'll try to acquire the permit here to make sure we only really stop when there are no
        // in-flight flushes. Our stop condition checks for the presence of waiters, but it could be
        // that we have no waiters, but a flush still in flight. We wait for all background work to
        // stop. When that stops, we know that the foreground work in the _flush_serializer has
        // stopped as well.
        return get_units(_background_work_flush_serializer, _max_background_work);
    });
}

void dirty_memory_manager::start_reclaiming() {
    _should_flush.signal();
}

future<> database::apply_in_memory(const frozen_mutation& m, schema_ptr m_schema, db::replay_position rp) {
    return _dirty_memory_manager.region_group().run_when_memory_available([this, &m, m_schema = std::move(m_schema), rp = std::move(rp)] {
        try {
            auto& cf = find_column_family(m.column_family_id());
            cf.apply(m, m_schema, rp);
        } catch (no_such_column_family&) {
            dblog.error("Attempting to mutate non-existent table {}", m.column_family_id());
        }
    });
}

future<> database::do_apply(schema_ptr s, const frozen_mutation& m) {
    // I'm doing a nullcheck here since the init code path for db etc
    // is a little in flux and commitlog is created only when db is
    // initied from datadir.
    auto uuid = m.column_family_id();
    auto& cf = find_column_family(uuid);
    if (!s->is_synced()) {
        throw std::runtime_error(sprint("attempted to mutate using not synced schema of %s.%s, version=%s",
                                 s->ks_name(), s->cf_name(), s->version()));
    }
    if (cf.commitlog() != nullptr) {
        commitlog_entry_writer cew(s, m);
        return cf.commitlog()->add_entry(uuid, cew).then([&m, this, s](auto rp) {
            return this->apply_in_memory(m, s, rp).handle_exception([this, s, &m] (auto ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (replay_position_reordered_exception&) {
                    // expensive, but we're assuming this is super rare.
                    // if we failed to apply the mutation due to future re-ordering
                    // (which should be the ever only reason for rp mismatch in CF)
                    // let's just try again, add the mutation to the CL once more,
                    // and assume success in inevitable eventually.
                    dblog.debug("replay_position reordering detected");
                    return this->apply(s, m);
                }
            });
        });
    }
    return apply_in_memory(m, s, db::replay_position());
}

future<> database::apply(schema_ptr s, const frozen_mutation& m) {
    if (dblog.is_enabled(logging::log_level::trace)) {
        dblog.trace("apply {}", m.pretty_printer(s));
    }
    return do_apply(std::move(s), m).then([this, s = _stats] {
        ++s->total_writes;
    });
}

future<> database::apply_streaming_mutation(schema_ptr s, utils::UUID plan_id, const frozen_mutation& m, bool fragmented) {
    if (!s->is_synced()) {
        throw std::runtime_error(sprint("attempted to mutate using not synced schema of %s.%s, version=%s",
                                 s->ks_name(), s->cf_name(), s->version()));
    }
    return _streaming_dirty_memory_manager.region_group().run_when_memory_available([this, &m, plan_id, fragmented, s = std::move(s)] {
        auto uuid = m.column_family_id();
        auto& cf = find_column_family(uuid);
        cf.apply_streaming_mutation(s, plan_id, std::move(m), fragmented);
    });
}

keyspace::config
database::make_keyspace_config(const keyspace_metadata& ksm) {
    // FIXME support multiple directories
    keyspace::config cfg;
    if (_cfg->data_file_directories().size() > 0) {
        cfg.datadir = sprint("%s/%s", _cfg->data_file_directories()[0], ksm.name());
        cfg.enable_disk_writes = !_cfg->enable_in_memory_data_store();
        cfg.enable_disk_reads = true; // we allways read from disk
        cfg.enable_commitlog = ksm.durable_writes() && _cfg->enable_commitlog() && !_cfg->enable_in_memory_data_store();
        cfg.enable_cache = _cfg->enable_cache();

    } else {
        cfg.datadir = "";
        cfg.enable_disk_writes = false;
        cfg.enable_disk_reads = false;
        cfg.enable_commitlog = false;
        cfg.enable_cache = false;
    }
    cfg.dirty_memory_manager = &_dirty_memory_manager;
    cfg.streaming_dirty_memory_manager = &_streaming_dirty_memory_manager;
    cfg.read_concurrency_config.sem = &_read_concurrency_sem;
    cfg.read_concurrency_config.timeout = _cfg->read_request_timeout_in_ms() * 1ms;
    // Assume a queued read takes up 10kB of memory, and allow 2% of memory to be filled up with such reads.
    cfg.read_concurrency_config.max_queue_length = memory::stats().total_memory() * 0.02 / 10000;
    cfg.read_concurrency_config.raise_queue_overloaded_exception = [this] {
        ++_stats->sstable_read_queue_overloaded;
        throw std::runtime_error("sstable inactive read queue overloaded");
    };
    cfg.streaming_read_concurrency_config = cfg.read_concurrency_config;
    cfg.streaming_read_concurrency_config.timeout = {};
    cfg.cf_stats = &_cf_stats;
    cfg.enable_incremental_backups = _enable_incremental_backups;
    return cfg;
}

namespace db {

std::ostream& operator<<(std::ostream& os, db::consistency_level cl) {
    switch (cl) {
    case db::consistency_level::ANY: return os << "ANY";
    case db::consistency_level::ONE: return os << "ONE";
    case db::consistency_level::TWO: return os << "TWO";
    case db::consistency_level::THREE: return os << "THREE";
    case db::consistency_level::QUORUM: return os << "QUORUM";
    case db::consistency_level::ALL: return os << "ALL";
    case db::consistency_level::LOCAL_QUORUM: return os << "LOCAL_QUORUM";
    case db::consistency_level::EACH_QUORUM: return os << "EACH_QUORUM";
    case db::consistency_level::SERIAL: return os << "SERIAL";
    case db::consistency_level::LOCAL_SERIAL: return os << "LOCAL_SERIAL";
    case db::consistency_level::LOCAL_ONE: return os << "LOCAL_ONE";
    default: abort();
    }
}

}

std::ostream&
operator<<(std::ostream& os, const exploded_clustering_prefix& ecp) {
    // Can't pass to_hex() to transformed(), since it is overloaded, so wrap:
    auto enhex = [] (auto&& x) { return to_hex(x); };
    return fprint(os, "prefix{%s}", ::join(":", ecp._v | boost::adaptors::transformed(enhex)));
}

std::ostream&
operator<<(std::ostream& os, const atomic_cell_view& acv) {
    if (acv.is_live()) {
        return fprint(os, "atomic_cell{%s;ts=%d;expiry=%d,ttl=%d}",
            to_hex(acv.value()),
            acv.timestamp(),
            acv.is_live_and_has_ttl() ? acv.expiry().time_since_epoch().count() : -1,
            acv.is_live_and_has_ttl() ? acv.ttl().count() : 0);
    } else {
        return fprint(os, "atomic_cell{DEAD;ts=%d;deletion_time=%d}",
            acv.timestamp(), acv.deletion_time().time_since_epoch().count());
    }
}

std::ostream&
operator<<(std::ostream& os, const atomic_cell& ac) {
    return os << atomic_cell_view(ac);
}

future<>
database::stop() {
    return _compaction_manager.stop().then([this] {
        // try to ensure that CL has done disk flushing
        if (_commitlog != nullptr) {
            return _commitlog->shutdown();
        }
        return make_ready_future<>();
    }).then([this] {
        return parallel_for_each(_column_families, [this] (auto& val_pair) {
            return val_pair.second->stop();
        });
    }).then([this] {
        return _system_dirty_memory_manager.shutdown();
    }).then([this] {
        return _dirty_memory_manager.shutdown();
    }).then([this] {
        return _streaming_dirty_memory_manager.shutdown();
    });
}

future<> database::flush_all_memtables() {
    return parallel_for_each(_column_families, [this] (auto& cfp) {
        return cfp.second->flush();
    });
}

future<> database::truncate(sstring ksname, sstring cfname, timestamp_func tsf) {
    auto& ks = find_keyspace(ksname);
    auto& cf = find_column_family(ksname, cfname);
    return truncate(ks, cf, std::move(tsf));
}

future<> database::truncate(const keyspace& ks, column_family& cf, timestamp_func tsf)
{
    const auto durable = ks.metadata()->durable_writes();
    const auto auto_snapshot = get_config().auto_snapshot();

    future<> f = make_ready_future<>();
    if (durable || auto_snapshot) {
        // TODO:
        // this is not really a guarantee at all that we've actually
        // gotten all things to disk. Again, need queue-ish or something.
        f = cf.flush();
    } else {
        f = cf.clear();
    }

    return cf.run_with_compaction_disabled([f = std::move(f), &cf, auto_snapshot, tsf = std::move(tsf)]() mutable {
        return f.then([&cf, auto_snapshot, tsf = std::move(tsf)] {
            dblog.debug("Discarding sstable data for truncated CF + indexes");
            // TODO: notify truncation

            return tsf().then([&cf, auto_snapshot](db_clock::time_point truncated_at) {
                future<> f = make_ready_future<>();
                if (auto_snapshot) {
                    auto name = sprint("%d-%s", truncated_at.time_since_epoch().count(), cf.schema()->cf_name());
                    f = cf.snapshot(name);
                }
                return f.then([&cf, truncated_at] {
                    return cf.discard_sstables(truncated_at).then([&cf, truncated_at](db::replay_position rp) {
                        // TODO: indexes.
                        return db::system_keyspace::save_truncation_record(cf, truncated_at, rp);
                    });
                });
            });
        });
    });
}

const sstring& database::get_snitch_name() const {
    return _cfg->endpoint_snitch();
}

// For the filesystem operations, this code will assume that all keyspaces are visible in all shards
// (as we have been doing for a lot of the other operations, like the snapshot itself).
future<> database::clear_snapshot(sstring tag, std::vector<sstring> keyspace_names) {
    std::vector<std::reference_wrapper<keyspace>> keyspaces;

    if (keyspace_names.empty()) {
        // if keyspace names are not given - apply to all existing local keyspaces
        for (auto& ks: _keyspaces) {
            keyspaces.push_back(std::reference_wrapper<keyspace>(ks.second));
        }
    } else {
        for (auto& ksname: keyspace_names) {
            try {
                keyspaces.push_back(std::reference_wrapper<keyspace>(find_keyspace(ksname)));
            } catch (no_such_keyspace& e) {
                return make_exception_future(std::current_exception());
            }
        }
    }

    return parallel_for_each(keyspaces, [this, tag] (auto& ks) {
        return parallel_for_each(ks.get().metadata()->cf_meta_data(), [this, tag] (auto& pair) {
            auto& cf = this->find_column_family(pair.second);
            return cf.clear_snapshot(tag);
         }).then_wrapped([] (future<> f) {
            dblog.debug("Cleared out snapshot directories");
         });
    });
}

future<> update_schema_version_and_announce(distributed<service::storage_proxy>& proxy)
{
    return db::schema_tables::calculate_schema_digest(proxy).then([&proxy] (utils::UUID uuid) {
        return proxy.local().get_db().invoke_on_all([uuid] (database& db) {
            db.update_version(uuid);
            return make_ready_future<>();
        }).then([uuid] {
            return db::system_keyspace::update_schema_version(uuid).then([uuid] {
                dblog.info("Schema version changed to {}", uuid);
                return service::get_local_migration_manager().passive_announce(uuid);
            });
        });
    });
}

// Snapshots: snapshotting the files themselves is easy: if more than one CF
// happens to link an SSTable twice, all but one will fail, and we will end up
// with one copy.
//
// The problem for us, is that the snapshot procedure is supposed to leave a
// manifest file inside its directory.  So if we just call snapshot() from
// multiple shards, only the last one will succeed, writing its own SSTables to
// the manifest leaving all other shards' SSTables unaccounted for.
//
// Moreover, for things like drop table, the operation should only proceed when the
// snapshot is complete. That includes the manifest file being correctly written,
// and for this reason we need to wait for all shards to finish their snapshotting
// before we can move on.
//
// To know which files we must account for in the manifest, we will keep an
// SSTable set.  Theoretically, we could just rescan the snapshot directory and
// see what's in there. But we would need to wait for all shards to finish
// before we can do that anyway. That is the hard part, and once that is done
// keeping the files set is not really a big deal.
//
// This code assumes that all shards will be snapshotting at the same time. So
// far this is a safe assumption, but if we ever want to take snapshots from a
// group of shards only, this code will have to be updated to account for that.
struct snapshot_manager {
    std::unordered_set<sstring> files;
    semaphore requests;
    semaphore manifest_write;
    snapshot_manager() : requests(0), manifest_write(0) {}
};
static thread_local std::unordered_map<sstring, lw_shared_ptr<snapshot_manager>> pending_snapshots;

static future<>
seal_snapshot(sstring jsondir) {
    std::ostringstream ss;
    int n = 0;
    ss << "{" << std::endl << "\t\"files\" : [ ";
    for (auto&& rf: pending_snapshots.at(jsondir)->files) {
        if (n++ > 0) {
            ss << ", ";
        }
        ss << "\"" << rf << "\"";
    }
    ss << " ]" << std::endl << "}" << std::endl;

    auto json = ss.str();
    auto jsonfile = jsondir + "/manifest.json";

    dblog.debug("Storing manifest {}", jsonfile);

    return io_check(recursive_touch_directory, jsondir).then([jsonfile, json = std::move(json)] {
        return open_checked_file_dma(general_disk_error_handler, jsonfile, open_flags::wo | open_flags::create | open_flags::truncate).then([json](file f) {
            return do_with(make_file_output_stream(std::move(f)), [json] (output_stream<char>& out) {
                return out.write(json.c_str(), json.size()).then([&out] {
                   return out.flush();
                }).then([&out] {
                   return out.close();
                });
            });
        });
    }).then([jsondir] {
        return io_check(sync_directory, std::move(jsondir));
    }).finally([jsondir] {
        pending_snapshots.erase(jsondir);
        return make_ready_future<>();
    });
}

future<> column_family::snapshot(sstring name) {
    return flush().then([this, name = std::move(name)]() {
        auto tables = boost::copy_range<std::vector<sstables::shared_sstable>>(*_sstables->all());
        return do_with(std::move(tables), [this, name](std::vector<sstables::shared_sstable> & tables) {
            auto jsondir = _config.datadir + "/snapshots/" + name;

            return parallel_for_each(tables, [name](sstables::shared_sstable sstable) {
                auto dir = sstable->get_dir() + "/snapshots/" + name;
                return io_check(recursive_touch_directory, dir).then([sstable, dir] {
                    return sstable->create_links(dir).then_wrapped([] (future<> f) {
                        // If the SSTables are shared, one of the CPUs will fail here.
                        // That is completely fine, though. We only need one link.
                        try {
                            f.get();
                        } catch (std::system_error& e) {
                            if (e.code() != std::error_code(EEXIST, std::system_category())) {
                                throw;
                            }
                        }
                        return make_ready_future<>();
                    });
                });
            }).then([jsondir, &tables] {
                // This is not just an optimization. If we have no files, jsondir may not have been created,
                // and sync_directory would throw.
                if (tables.size()) {
                    return io_check(sync_directory, std::move(jsondir));
                } else {
                    return make_ready_future<>();
                }
            }).finally([this, &tables, jsondir] {
                auto shard = std::hash<sstring>()(jsondir) % smp::count;
                std::unordered_set<sstring> table_names;
                for (auto& sst : tables) {
                    auto f = sst->get_filename();
                    auto rf = f.substr(sst->get_dir().size() + 1);
                    table_names.insert(std::move(rf));
                }
                return smp::submit_to(shard, [requester = engine().cpu_id(), jsondir = std::move(jsondir),
                                              tables = std::move(table_names), datadir = _config.datadir] {

                    if (pending_snapshots.count(jsondir) == 0) {
                        pending_snapshots.emplace(jsondir, make_lw_shared<snapshot_manager>());
                    }
                    auto snapshot = pending_snapshots.at(jsondir);
                    for (auto&& sst: tables) {
                        snapshot->files.insert(std::move(sst));
                    }

                    snapshot->requests.signal(1);
                    auto my_work = make_ready_future<>();
                    if (requester == engine().cpu_id()) {
                        my_work = snapshot->requests.wait(smp::count).then([jsondir = std::move(jsondir),
                                                                            snapshot] () mutable {
                            return seal_snapshot(jsondir).then([snapshot] {
                                snapshot->manifest_write.signal(smp::count);
                                return make_ready_future<>();
                            });
                        });
                    }
                    return my_work.then([snapshot] {
                        return snapshot->manifest_write.wait(1);
                    }).then([snapshot] {});
                });
            });
        });
    });
}

future<bool> column_family::snapshot_exists(sstring tag) {
    sstring jsondir = _config.datadir + "/snapshots/" + tag;
    return open_checked_directory(general_disk_error_handler, std::move(jsondir)).then_wrapped([] (future<file> f) {
        try {
            f.get0();
            return make_ready_future<bool>(true);
        } catch (std::system_error& e) {
            if (e.code() != std::error_code(ENOENT, std::system_category())) {
                throw;
            }
            return make_ready_future<bool>(false);
        }
    });
}

enum class missing { no, yes };
static missing
file_missing(future<> f) {
    try {
        f.get();
        return missing::no;
    } catch (std::system_error& e) {
        if (e.code() != std::error_code(ENOENT, std::system_category())) {
            throw;
        }
        return missing::yes;
    }
}

future<> column_family::clear_snapshot(sstring tag) {
    sstring jsondir = _config.datadir + "/snapshots/";
    sstring parent = _config.datadir;
    if (!tag.empty()) {
        jsondir += tag;
        parent += "/snapshots/";
    }

    lister::dir_entry_types dir_and_files = { directory_entry_type::regular, directory_entry_type::directory };
    return lister::scan_dir(jsondir, dir_and_files, [this, curr_dir = jsondir, dir_and_files, tag] (directory_entry de) {
        // FIXME: We really need a better directory walker. This should eventually be part of the seastar infrastructure.
        // It's hard to write this in a fully recursive manner because we need to keep information about the parent directory,
        // so we can remove the file. For now, we'll take advantage of the fact that we will at most visit 2 levels and keep
        // it ugly but simple.
        auto recurse = make_ready_future<>();
        if (de.type == directory_entry_type::directory) {
            // Should only recurse when tag is empty, meaning delete all snapshots
            if (!tag.empty()) {
                throw std::runtime_error(sprint("Unexpected directory %s found at %s! Aborting", de.name, curr_dir));
            }
            auto newdir = curr_dir + "/" + de.name;
            recurse = lister::scan_dir(newdir, dir_and_files, [this, curr_dir = newdir] (directory_entry de) {
                return io_check(remove_file, curr_dir + "/" + de.name);
            });
        }
        return recurse.then([fname = curr_dir + "/" + de.name] {
            return io_check(remove_file, fname);
        });
    }).then_wrapped([jsondir] (future<> f) {
        // Fine if directory does not exist. If it did, we delete it
        if (file_missing(std::move(f)) == missing::no) {
            return io_check(remove_file, jsondir);
        }
        return make_ready_future<>();
    }).then([parent] {
        return io_check(sync_directory, parent).then_wrapped([] (future<> f) {
            // Should always exist for empty tags, but may not exist for a single tag if we never took
            // snapshots. We will check this here just to mask out the exception, without silencing
            // unexpected ones.
            file_missing(std::move(f));
            return make_ready_future<>();
        });
    });
}

future<std::unordered_map<sstring, column_family::snapshot_details>> column_family::get_snapshot_details() {
    std::unordered_map<sstring, snapshot_details> all_snapshots;
    return do_with(std::move(all_snapshots), [this] (auto& all_snapshots) {
        return io_check([&] { return engine().file_exists(_config.datadir + "/snapshots"); }).then([this, &all_snapshots](bool file_exists) {
            if (!file_exists) {
                return make_ready_future<>();
            }
            return lister::scan_dir(_config.datadir + "/snapshots",  { directory_entry_type::directory }, [this, &all_snapshots] (directory_entry de) {
            auto snapshot_name = de.name;
            auto snapshot = _config.datadir + "/snapshots/" + snapshot_name;
            all_snapshots.emplace(snapshot_name, snapshot_details());
            return lister::scan_dir(snapshot,  { directory_entry_type::regular }, [this, &all_snapshots, snapshot, snapshot_name] (directory_entry de) {
                return io_check(file_size, snapshot + "/" + de.name).then([this, &all_snapshots, snapshot_name, name = de.name] (auto size) {
                    // The manifest is the only file expected to be in this directory not belonging to the SSTable.
                    // For it, we account the total size, but zero it for the true size calculation.
                    //
                    // All the others should just generate an exception: there is something wrong, so don't blindly
                    // add it to the size.
                    if (name != "manifest.json") {
                        sstables::entry_descriptor::make_descriptor(name);
                        all_snapshots.at(snapshot_name).total += size;
                    } else {
                        size = 0;
                    }
                    return make_ready_future<uint64_t>(size);
                }).then([this, &all_snapshots, snapshot_name, name = de.name] (auto size) {
                    // FIXME: When we support multiple data directories, the file may not necessarily
                    // live in this same location. May have to test others as well.
                    return io_check(file_size, _config.datadir + "/" + name).then_wrapped([&all_snapshots, snapshot_name, size] (auto fut) {
                        try {
                            // File exists in the main SSTable directory. Snapshots are not contributing to size
                            fut.get0();
                        } catch (std::system_error& e) {
                            if (e.code() != std::error_code(ENOENT, std::system_category())) {
                                throw;
                            }
                            all_snapshots.at(snapshot_name).live += size;
                        }
                        return make_ready_future<>();
                    });
                });
            });
        });
        }).then([&all_snapshots] {
            return std::move(all_snapshots);
        });
    });
}

future<> column_family::flush() {
    _stats.pending_flushes++;

    // highest_flushed_rp is only updated when we flush. If the memtable is currently alive, then
    // the most up2date replay position is the one that's in there now. Otherwise, if the memtable
    // hasn't received any writes yet, that's the one from the last flush we made.
    auto desired_rp = _memtables->back()->empty() ? _highest_flushed_rp : _memtables->back()->replay_position();
    return _memtables->request_flush().finally([this, desired_rp] {
        _stats.pending_flushes--;
        // In origin memtable_switch_count is incremented inside
        // ColumnFamilyMeetrics Flush.run
        _stats.memtable_switch_count++;
        // wait for all up until us.
        return _flush_queue->wait_for_pending(desired_rp);
    });
}

future<> column_family::flush(const db::replay_position& pos) {
    // Technically possible if we've already issued the
    // sstable write, but it is not done yet.
    if (pos < _highest_flushed_rp) {
        return make_ready_future<>();
    }

    // TODO: Origin looks at "secondary" memtables
    // It also consideres "minReplayPosition", which is simply where
    // the CL "started" (the first ever RP in this run).
    // We ignore this for now and just say that if we're asked for
    // a CF and it exists, we pretty much have to have data that needs
    // flushing. Let's do it.
    return _memtables->request_flush();
}

// FIXME: We can do much better than this in terms of cache management. Right
// now, we only have to flush the touched ranges because of the possibility of
// streaming containing token ownership changes.
//
// Right now we can't differentiate between that and a normal repair process,
// so we always flush. When we can differentiate those streams, we should not
// be indiscriminately touching the cache during repair. We will just have to
// invalidate the entries that are relevant to things we already have in the cache.
future<> column_family::flush_streaming_mutations(utils::UUID plan_id, std::vector<query::partition_range> ranges) {
    // This will effectively take the gate twice for this call. The proper way to fix that would
    // be to change seal_active_streaming_memtable_delayed to take a range parameter. However, we
    // need this code to go away as soon as we can (see FIXME above). So the double gate is a better
    // temporary counter measure.
    return with_gate(_streaming_flush_gate, [this, plan_id, ranges = std::move(ranges)] {
        return flush_streaming_big_mutations(plan_id).then([this] {
            return _streaming_memtables->seal_active_memtable(memtable_list::flush_behavior::delayed);
        }).finally([this] {
            return _streaming_flush_phaser.advance_and_await();
        }).finally([this, ranges = std::move(ranges)] {
            if (!_config.enable_cache) {
                return make_ready_future<>();
            }
            return do_with(std::move(ranges), [this] (auto& ranges) {
                return parallel_for_each(ranges, [this](auto&& range) {
                    return _cache.invalidate(range);
                });
            });
        });
    });
}

future<> column_family::flush_streaming_big_mutations(utils::UUID plan_id) {
    auto it = _streaming_memtables_big.find(plan_id);
    if (it == _streaming_memtables_big.end()) {
        return make_ready_future<>();
    }
    auto entry = it->second;
    _streaming_memtables_big.erase(it);
    return entry->memtables->request_flush().then([entry] {
        return entry->flush_in_progress.close();
    }).then([this, entry] {
        return parallel_for_each(entry->sstables, [this] (auto& sst) {
            return sst->seal_sstable(this->incremental_backups_enabled()).then([sst] {
                return sst->open_data();
            });
        }).then([this, entry] {
            for (auto&& sst : entry->sstables) {
                add_sstable(sst);
            }
            trigger_compaction();
        });
    });
}

future<> column_family::fail_streaming_mutations(utils::UUID plan_id) {
    auto it = _streaming_memtables_big.find(plan_id);
    if (it == _streaming_memtables_big.end()) {
        return make_ready_future<>();
    }
    auto entry = it->second;
    _streaming_memtables_big.erase(it);
    return entry->flush_in_progress.close().then([this, entry] {
        for (auto&& sst : entry->sstables) {
            sst->mark_for_deletion();
        }
    });
}

future<> column_family::clear() {
    _memtables->clear();
    _memtables->add_memtable();
    _streaming_memtables->clear();
    _streaming_memtables->add_memtable();
    _streaming_memtables_big.clear();
    return _cache.clear();
}

// NOTE: does not need to be futurized, but might eventually, depending on
// if we implement notifications, whatnot.
future<db::replay_position> column_family::discard_sstables(db_clock::time_point truncated_at) {
    assert(_compaction_disabled > 0);

    return with_lock(_sstables_lock.for_read(), [this, truncated_at] {
        db::replay_position rp;
        auto gc_trunc = to_gc_clock(truncated_at);

        auto pruned = make_lw_shared(_compaction_strategy.make_sstable_set(_schema));
        std::vector<sstables::shared_sstable> remove;

        for (auto&p : *_sstables->all()) {
            if (p->max_data_age() <= gc_trunc) {
                rp = std::max(p->get_stats_metadata().position, rp);
                remove.emplace_back(p);
                continue;
            }
            pruned->insert(p);
        }

        _sstables = std::move(pruned);
        dblog.debug("cleaning out row cache");
        return _cache.clear().then([rp, remove = std::move(remove)] () mutable {
            return parallel_for_each(remove, [](sstables::shared_sstable s) {
                return sstables::delete_atomically({s});
            }).then([rp] {
                return make_ready_future<db::replay_position>(rp);
            }).finally([remove] {}); // keep the objects alive until here.
        });
    });
}


std::ostream& operator<<(std::ostream& os, const user_types_metadata& m) {
    os << "org.apache.cassandra.config.UTMetaData@" << &m;
    return os;
}

std::ostream& operator<<(std::ostream& os, const keyspace_metadata& m) {
    os << "KSMetaData{";
    os << "name=" << m._name;
    os << ", strategyClass=" << m._strategy_name;
    os << ", strategyOptions={";
    int n = 0;
    for (auto& p : m._strategy_options) {
        if (n++ != 0) {
            os << ", ";
        }
        os << p.first << "=" << p.second;
    }
    os << "}";
    os << ", cfMetaData={";
    n = 0;
    for (auto& p : m._cf_meta_data) {
        if (n++ != 0) {
            os << ", ";
        }
        os << p.first << "=" << p.second;
    }
    os << "}";
    os << ", durable_writes=" << m._durable_writes;
    os << ", userTypes=" << m._user_types;
    os << "}";
    return os;
}

void column_family::set_schema(schema_ptr s) {
    dblog.debug("Changing schema version of {}.{} ({}) from {} to {}",
                _schema->ks_name(), _schema->cf_name(), _schema->id(), _schema->version(), s->version());

    for (auto& m : *_memtables) {
        m->set_schema(s);
    }

    for (auto& m : *_streaming_memtables) {
        m->set_schema(s);
    }

    for (auto smb : _streaming_memtables_big) {
        for (auto m : *smb.second->memtables) {
            m->set_schema(s);
        }
    }

    _cache.set_schema(s);
    _schema = std::move(s);

    set_compaction_strategy(_schema->compaction_strategy());
    trigger_compaction();
}
