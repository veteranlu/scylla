/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * Modified by ScyllaDB
 * Copyright (C) 2015 ScyllaDB
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

#include "batch_statement.hh"
#include "raw/batch_statement.hh"
#include "db/config.hh"

namespace cql3 {

namespace statements {

logging::logger batch_statement::_logger("BatchStatement");

bool batch_statement::depends_on_keyspace(const sstring& ks_name) const
{
    return false;
}

bool batch_statement::depends_on_column_family(const sstring& cf_name) const
{
    return false;
}

void batch_statement::verify_batch_size(const std::vector<mutation>& mutations) {
    size_t warn_threshold = service::get_local_storage_proxy().get_db().local().get_config().batch_size_warn_threshold_in_kb() * 1024;

    class my_partition_visitor : public mutation_partition_visitor {
    public:
        void accept_partition_tombstone(tombstone) override {}
        void accept_static_cell(column_id, atomic_cell_view v)  override {
            size += v.value().size();
        }
        void accept_static_cell(column_id, collection_mutation_view v) override {
            size += v.data.size();
        }
        void accept_row_tombstone(const range_tombstone&) override {}
        void accept_row(clustering_key_view, tombstone, const row_marker&) override {}
        void accept_row_cell(column_id, atomic_cell_view v) override {
            size += v.value().size();
        }
        void accept_row_cell(column_id id, collection_mutation_view v) override {
            size += v.data.size();
        }

        size_t size = 0;
    };

    my_partition_visitor v;

    for (auto&m : mutations) {
        m.partition().accept(*m.schema(), v);
    }

    if (v.size > warn_threshold) {
        std::unordered_set<sstring> ks_cf_pairs;
        for (auto&& m : mutations) {
            ks_cf_pairs.insert(m.schema()->ks_name() + "." + m.schema()->cf_name());
        }
        _logger.warn(
                        "Batch of prepared statements for {} is of size {}, exceeding specified threshold of {} by {}.{}",
                        join(", ", ks_cf_pairs), v.size, warn_threshold,
                        v.size - warn_threshold, "");
    }
}

namespace raw {

shared_ptr<prepared_statement>
batch_statement::prepare(database& db, cql_stats& stats) {
    auto&& bound_names = get_bound_variables();

    std::vector<shared_ptr<cql3::statements::modification_statement>> statements;
    for (auto&& parsed : _parsed_statements) {
        statements.push_back(parsed->prepare(db, bound_names, stats));
    }

    auto&& prep_attrs = _attrs->prepare(db, "[batch]", "[batch]");
    prep_attrs->collect_marker_specification(bound_names);

    cql3::statements::batch_statement batch_statement_(bound_names->size(), _type, std::move(statements), std::move(prep_attrs), stats);
    batch_statement_.validate();

    return ::make_shared<prepared>(make_shared(std::move(batch_statement_)),
                                                     bound_names->get_specifications());
}

}


}

}


