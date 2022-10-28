/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#pragma once

#include <cstdlib>
#include <string>
#include <seastar/util/std-compat.hh>
#include <vector>
#include <set>
#include <sched.h>
#include <boost/any.hpp>
#include <unordered_map>

namespace seastar {

cpu_set_t cpuid_to_cpuset(unsigned cpuid);

namespace resource {

using compat::optional;

using cpuset = std::set<unsigned>;

struct configuration {
    optional<size_t> total_memory;
    optional<size_t> reserve_memory;  // if total_memory not specified
    optional<size_t> cpus;
    optional<cpuset> cpu_set;
    std::unordered_map<dev_t, unsigned> num_io_queues;      // dev_t: NUMA node index
};

struct memory {
    size_t bytes;
    unsigned nodeid;        // NUMA node index

};

// Since this is static information, we will keep a copy at each CPU.
// This will allow us to easily find who is the IO coordinator for a given
// node without a trip to a remote CPU.
struct io_queue_topology {
    std::vector<unsigned> shard_to_coordinator;
    std::vector<unsigned> coordinators;
    std::vector<unsigned> coordinator_to_idx;
    std::vector<bool> coordinator_to_idx_valid; // for validity asserts
};

struct cpu {
    unsigned cpu_id; // 实际的 cpu index. 将超线程计算在内. 比如 1 个 NUMA node 有两个 cpu, 每个 cpu 开启了超线程, 所以可以认为共有 4 个 cpu core, 这里的 cpu_id 就为 0-3
    std::vector<memory> mem;
};

struct resources {
    std::vector<cpu> cpus;
    std::unordered_map<dev_t, io_queue_topology> ioq_topology;      // dev_t: NUMA node index. 可以通过 lscpu 获取 NUMA NODE 信息
};

resources allocate(configuration c);
unsigned nr_processing_units();
}

// We need a wrapper class, because boost::program_options wants validate()
// (below) to be in the same namespace as the type it is validating.
struct cpuset_bpo_wrapper {
    resource::cpuset value;
};

// Overload for boost program options parsing/validation
extern
void validate(boost::any& v,
              const std::vector<std::string>& values,
              cpuset_bpo_wrapper* target_type, int);

}
