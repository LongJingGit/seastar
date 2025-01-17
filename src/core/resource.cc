
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

#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <regex>
#include <seastar/core/resource.hh>
#include <seastar/core/align.hh>
#include <seastar/core/print.hh>
#include <seastar/util/read_first_line.hh>
#include <stdlib.h>
#include <limits>
#include "cgroup.hh"
#include <seastar/util/log.hh>

#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm/copy.hpp>

namespace seastar {

extern logger seastar_logger;

// This function was made optional because of validate. It needs to
// throw an error when a non parseable input is given.
compat::optional<resource::cpuset> parse_cpuset(std::string value) {
    static std::regex r("(\\d+-)?(\\d+)(,(\\d+-)?(\\d+))*");

    std::smatch match;
    if (std::regex_match(value, match, r)) {
        std::vector<std::string> ranges;
        boost::split(ranges, value, boost::is_any_of(","));
        resource::cpuset ret;
        for (auto&& range: ranges) {
            std::string beg = range;
            std::string end = range;
            auto dash = range.find('-');
            if (dash != range.npos) {
                beg = range.substr(0, dash);
                end = range.substr(dash + 1);
            }
            auto b = boost::lexical_cast<unsigned>(beg);
            auto e = boost::lexical_cast<unsigned>(end);

            if (b > e) {
                return seastar::compat::nullopt;
            }

            for (auto i = b; i <= e; ++i) {
                ret.insert(i);
            }
        }
        return ret;
    }
    return seastar::compat::nullopt;
}

// Overload for boost program options parsing/validation
void validate(boost::any& v,
              const std::vector<std::string>& values,
              cpuset_bpo_wrapper* target_type, int) {
    using namespace boost::program_options;
    validators::check_first_occurrence(v);

    // Extract the first string from 'values'. If there is more than
    // one string, it's an error, and exception will be thrown.
    auto&& s = validators::get_single_string(values);
    auto parsed_cpu_set = parse_cpuset(s);

    if (parsed_cpu_set) {
        cpuset_bpo_wrapper ret;
        ret.value = *parsed_cpu_set;
        v = std::move(ret);
    } else {
        throw validation_error(validation_error::invalid_option_value);
    }
}

namespace cgroup {

namespace fs = seastar::compat::filesystem;

optional<cpuset> cpu_set() {
    auto cpuset = read_setting_V1V2_as<std::string>(
                              "cpuset/cpuset.cpus",
                              "cpuset.cpus.effective");
    if (cpuset) {
        return seastar::parse_cpuset(*cpuset);
    }

    seastar_logger.warn("Unable to parse cgroup's cpuset. Ignoring.");
    return seastar::compat::nullopt;
}

size_t memory_limit() {
    return read_setting_V1V2_as<size_t>(
                             "memory/memory.limit_in_bytes",
                             "memory.max")
        .value_or(std::numeric_limits<size_t>::max());
}

template <typename T>
optional<T> read_setting_as(std::string path) {
    try {
        auto line = read_first_line(path);
        return boost::lexical_cast<T>(line);
    } catch (...) {
        seastar_logger.warn("Couldn't read cgroup file {}.", path);
    }

    return seastar::compat::nullopt;
}

/*
 * what cgroup do we belong to?
 *
 * For cgroups V2, /proc/self/cgroup should read "0::<cgroup-dir-path>"
 * Note: true only for V2-only systems, but there is no reason to support
 * a hybrid configuration.
 */
static optional<fs::path> cgroup2_path_my_pid() {
    seastar::sstring cline;
    try {
        cline = read_first_line(fs::path{"/proc/self/cgroup"});
    } catch (...) {
        // '/proc/self/cgroup' must be there. If not - there is an issue
        // with the system configuration.
        throw std::runtime_error("no cgroup data for our process");
    }

    // for a V2-only system, we expect exactly one line:
    // 0::<abs-path-to-cgroup>
    if (cline.at(0) != '0') {
        // This is either a v1 system, or system configured with a hybrid of v1 & v2.
        // We do not support such combinations of v1 and v2 at this point.
        seastar_logger.debug("Not a cgroups-v2-only system");
        return seastar::compat::nullopt;
    }

    // the path is guaranteed to start with '0::/'
    return fs::path{"/sys/fs/cgroup/" + cline.substr(4)};
}

/*
 * traverse the cgroups V2 hierarchy bottom-up, starting from our process'
 * specific cgroup up to /sys/fs/cgroup, looking for the named file.
 */
static optional<fs::path> locate_lowest_cgroup2(fs::path lowest_subdir, std::string filename) {
    // locate the lowest subgroup containing the named file (i.e.
    // handles the requested control by itself)
    do {
        //  does the cgroup settings file exist?
        auto set_path = lowest_subdir / filename;
        if (fs::exists(set_path) ) {
            return set_path;
        }

        lowest_subdir = lowest_subdir.parent_path();
    } while (lowest_subdir.compare("/sys/fs"));

    return seastar::compat::nullopt;
}

/*
 * Read a settings value from either the cgroups V2 or the corresponding
 * cgroups V1 files.
 * For V2, look for the lowest cgroup in our hierarchy that manages the
 * requested settings.
 */
template <typename T>
optional<T> read_setting_V1V2_as(std::string cg1_path, std::string cg2_fname) {
    // on v2-systems, cg2_path will be initialized with the leaf cgroup that
    // controls this process
    static optional<fs::path> cg2_path{cgroup2_path_my_pid()};

    if (cg2_path) {
        // this is a v2 system
        seastar::sstring line;
        try {
            line = read_first_line(locate_lowest_cgroup2(*cg2_path, cg2_fname).value());
        } catch (...) {
            seastar_logger.warn("Could not read cgroups v2 file ({}).", cg2_fname);
            return seastar::compat::nullopt;
        }
        if (line.compare("max")) {
            try {
                return boost::lexical_cast<T>(line);
            } catch (...) {
                seastar_logger.warn("Malformed cgroups file ({}) contents.", cg2_fname);
            }
        }
        return seastar::compat::nullopt;
    }

    // try cgroups v1:
    try {
        auto line = read_first_line(fs::path{"/sys/fs/cgroup"} / cg1_path);
        return boost::lexical_cast<T>(line);
    } catch (...) {
        seastar_logger.warn("Could not parse cgroups v1 file ({}).", cg1_path);
    }

    return seastar::compat::nullopt;
}

}

namespace resource {

size_t calculate_memory(configuration c, size_t available_memory, float panic_factor = 1) {
    size_t default_reserve_memory = std::max<size_t>(1536 * 1024 * 1024, 0.07 * available_memory) * panic_factor;
    auto reserve = c.reserve_memory.value_or(default_reserve_memory);
    size_t min_memory = 500'000'000;
    if (available_memory >= reserve + min_memory) {
        available_memory -= reserve;
    } else {
        // Allow starting up even in low memory configurations (e.g. 2GB boot2docker VM)
        available_memory = min_memory;
    }
    size_t mem = c.total_memory.value_or(available_memory);
    if (mem > available_memory) {
        throw std::runtime_error(format("insufficient physical memory: needed {} available {}", mem, available_memory));
    }
    return mem;
}

}

}

#ifdef SEASTAR_HAVE_HWLOC

#include <seastar/util/defer.hh>
#include <seastar/core/print.hh>
#include <hwloc.h>
#include <unordered_map>
#include <boost/range/irange.hpp>

namespace seastar {

cpu_set_t cpuid_to_cpuset(unsigned cpuid) {
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(cpuid, &cs);
    return cs;
}

namespace resource {

size_t div_roundup(size_t num, size_t denom) {
    return (num + denom - 1) / denom;
}

static size_t alloc_from_node(cpu& this_cpu, hwloc_obj_t node, std::unordered_map<hwloc_obj_t, size_t>& used_mem, size_t alloc) {
#if HWLOC_API_VERSION >= 0x00020000
    // FIXME: support nodes with multiple NUMA nodes, whatever that means
    auto local_memory = node->total_memory;
#else
    auto local_memory = node->memory.local_memory;
#endif
    auto taken = std::min(local_memory - used_mem[node], alloc);
    if (taken) {
        used_mem[node] += taken;
        auto node_id = hwloc_bitmap_first(node->nodeset);       // NUMA node index
        assert(node_id != -1);
        this_cpu.mem.push_back({taken, unsigned(node_id)});
    }
    return taken;
}

// Find the numa node that contains a specific PU.
static hwloc_obj_t get_numa_node_for_pu(hwloc_topology_t& topology, hwloc_obj_t pu) {
    // Can't use ancestry because hwloc 2.0 NUMA nodes are not ancestors of PUs
    hwloc_obj_t tmp = NULL;
    auto depth = hwloc_get_type_or_above_depth(topology, HWLOC_OBJ_NUMANODE);
    while ((tmp = hwloc_get_next_obj_by_depth(topology, depth, tmp)) != NULL) {
        if (hwloc_bitmap_intersects(tmp->cpuset, pu->cpuset)) {
            return tmp;
        }
    }
    assert(false && "PU not inside any NUMA node");
    abort();
}

struct distribute_objects {
    std::vector<hwloc_cpuset_t> cpu_sets;
    hwloc_obj_t root;

    distribute_objects(hwloc_topology_t& topology, size_t nobjs) : cpu_sets(nobjs), root(hwloc_get_root_obj(topology)) {
#if HWLOC_API_VERSION >= 0x00010900
        hwloc_distrib(topology, &root, 1, cpu_sets.data(), cpu_sets.size(), INT_MAX, 0);
#else
        hwloc_distribute(topology, root, cpu_sets.data(), cpu_sets.size(), INT_MAX);
#endif
    }

    ~distribute_objects() {
        for (auto&& cs : cpu_sets) {
            hwloc_bitmap_free(cs);
        }
    }
    std::vector<hwloc_cpuset_t>& operator()() {
        return cpu_sets;
    }
};

static io_queue_topology
allocate_io_queues(hwloc_topology_t& topology, std::vector<cpu> cpus, unsigned num_io_queues, unsigned& last_node_idx) {
    auto node_of_shard = [&topology, &cpus] (unsigned shard) {
        auto pu = hwloc_get_pu_obj_by_os_index(topology, cpus[shard].cpu_id);
        auto node = get_numa_node_for_pu(topology, pu);
        return hwloc_bitmap_first(node->nodeset);
    };

    // There are two things we are trying to achieve by populating a numa_nodes map.
    //
    // The first is to find out how many nodes we have in the system. We can't use
    // hwloc for that, because at this point we are not longer talking about the physical system,
    // but the actual booted seastar server instead. So if we have restricted the run to a subset
    // of the available processors, counting topology nodes won't spur the same result.
    // 第一个是找出系统中有多少个节点。我们不能使用hwloc，因为在这一点上，我们不再讨论物理系统，而是实际启动的seastar服务器。因此，如果我们将运行限制在可用处理器的一个子集内，那么计算拓扑节点将不会得到相同的结果。
    //
    // Secondly, we need to find out which processors live in each node. For a reason similar to the
    // above, hwloc won't do us any good here. Later on, we will use this information to assign
    // shards to coordinators that are node-local to themselves.
    // 其次，我们需要找出每个节点上有哪些处理器。出于类似于上面的原因，hwloc在这里不会对我们有任何帮助。稍后，我们将使用这些信息来将分片分配给自身是节点本地的协调器。
    std::unordered_map<unsigned, std::set<unsigned>> numa_nodes;
    for (auto shard: boost::irange(0, int(cpus.size()))) {
        auto node_id = node_of_shard(shard);

        if (numa_nodes.count(node_id) == 0) {
            numa_nodes.emplace(node_id, std::set<unsigned>());
        }
        numa_nodes.at(node_id).insert(shard);
    }

    io_queue_topology ret;
    ret.shard_to_coordinator.resize(cpus.size());
    ret.coordinator_to_idx.resize(cpus.size());
    ret.coordinator_to_idx_valid.resize(cpus.size());

    // User may be playing with --smp option, but num_io_queues was independently
    // determined by iotune, so adjust for any conflicts.
    if (num_io_queues > cpus.size()) {
        fmt::print("Warning: number of IO queues ({:d}) greater than logical cores ({:d}). Adjusting downwards.\n", num_io_queues, cpus.size());
        num_io_queues = cpus.size();
    }

    auto find_shard = [&cpus] (unsigned cpu_id) {
        auto idx = 0u;
        for (auto& c: cpus) {
            if (c.cpu_id == cpu_id) {
                return idx;
            }
            idx++;
        }
        assert(0);
    };

    auto cpu_sets = distribute_objects(topology, num_io_queues);
    ret.coordinators.reserve(cpu_sets().size());

    // First step: distribute the IO queues given the information returned in cpu_sets.
    // If there is one IO queue per processor, only this loop will be executed.
    std::unordered_map<unsigned, std::vector<unsigned>> node_coordinators;
    for (auto&& cs : cpu_sets()) {
        auto io_coordinator = find_shard(hwloc_bitmap_first(cs));

        ret.coordinator_to_idx[io_coordinator] = ret.coordinators.size();
        assert(!ret.coordinator_to_idx_valid[io_coordinator]);
        ret.coordinator_to_idx_valid[io_coordinator] = true;
        ret.coordinators.emplace_back(io_coordinator);
        // If a processor is a coordinator, it is also obviously a coordinator of itself
        ret.shard_to_coordinator[io_coordinator] = io_coordinator;

        auto node_id = node_of_shard(io_coordinator);
        if (node_coordinators.count(node_id) == 0) {
            node_coordinators.emplace(node_id, std::vector<unsigned>());
        }
        node_coordinators.at(node_id).push_back(io_coordinator);
        numa_nodes[node_id].erase(io_coordinator);
    }


    auto available_nodes = boost::copy_range<std::vector<unsigned>>(node_coordinators | boost::adaptors::map_keys);

    // If there are more processors than coordinators, we will have to assign them to existing
    // coordinators. We prefer do that within the same NUMA node, but if not possible we assign
    // the shard to a random node.
    for (auto& node: numa_nodes) {
        auto cid_idx = 0;
        for (auto& remaining_shard: node.second) {
            auto my_node = node.first;
            // No I/O queue in this node, round-robin shards from this node into existing ones.
            if (!node_coordinators.count(node.first)) {
                my_node = available_nodes[last_node_idx++ % available_nodes.size()];
            }
            auto idx = cid_idx++ % node_coordinators.at(my_node).size();
            auto io_coordinator = node_coordinators.at(my_node)[idx];
            ret.shard_to_coordinator[remaining_shard] = io_coordinator;
        }
    }

    return ret;
}


resources allocate(configuration c) {
    hwloc_topology_t topology;
    hwloc_topology_init(&topology);
    auto free_hwloc = defer([&] { hwloc_topology_destroy(topology); });
    hwloc_topology_load(topology);
    if (c.cpu_set) {
        auto bm = hwloc_bitmap_alloc();
        auto free_bm = defer([&] { hwloc_bitmap_free(bm); });
        for (auto idx : *c.cpu_set) {
            hwloc_bitmap_set(bm, idx);
        }
        auto r = hwloc_topology_restrict(topology, bm,
#if HWLOC_API_VERSION >= 0x00020000
                0
#else
                HWLOC_RESTRICT_FLAG_ADAPT_DISTANCES
#endif
                | HWLOC_RESTRICT_FLAG_ADAPT_MISC
                | HWLOC_RESTRICT_FLAG_ADAPT_IO);
        if (r == -1) {
            if (errno == ENOMEM) {
                throw std::bad_alloc();
            }
            if (errno == EINVAL) {
                throw std::runtime_error("bad cpuset");
            }
            abort();
        }
    }
    auto machine_depth = hwloc_get_type_depth(topology, HWLOC_OBJ_MACHINE);
    assert(hwloc_get_nbobjs_by_depth(topology, machine_depth) == 1);
    auto machine = hwloc_get_obj_by_depth(topology, machine_depth, 0);
#if HWLOC_API_VERSION >= 0x00020000
    auto available_memory = machine->total_memory;
#else
    auto available_memory = machine->memory.total_memory;
#endif
    size_t mem = calculate_memory(c, std::min(available_memory, cgroup::memory_limit()));       // 计算系统剩余的内存
    unsigned available_procs = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PU);        //
    unsigned procs = c.cpus.value_or(available_procs);      // 计算 seastar 服务器启动需要的 cpu core 数量
    if (procs > available_procs) {
        throw std::runtime_error("insufficient processing units");
    }
    // 计算将系统内存按照 cpu core 的数量分为相同的若干份，每一个 cpu core 需要的内存的大小
    auto mem_per_proc = align_down<size_t>(mem / procs, 2 << 20);

    resources ret;
    std::unordered_map<hwloc_obj_t, size_t> topo_used_mem;      // hwloc_obj_t: NUMA node index
    std::vector<std::pair<cpu, size_t>> remains;
    size_t remain;

    auto cpu_sets = distribute_objects(topology, procs);

    // Divide local memory to cpus. 将本地内存分配给不同的 cpu core
    for (auto&& cs : cpu_sets()) {
        auto cpu_id = hwloc_bitmap_first(cs);
        assert(cpu_id != -1);
        auto pu = hwloc_get_pu_obj_by_os_index(topology, cpu_id);
        auto node = get_numa_node_for_pu(topology, pu);     // node 实际上就是 NUMA node
        cpu this_cpu;
        this_cpu.cpu_id = cpu_id;
        remain = mem_per_proc - alloc_from_node(this_cpu, node, topo_used_mem, mem_per_proc);

        remains.emplace_back(std::move(this_cpu), remain);
    }

    // Divide the rest of the memory
    auto depth = hwloc_get_type_or_above_depth(topology, HWLOC_OBJ_NUMANODE);
    for (auto&& r : remains) {
        cpu this_cpu;
        size_t remain;
        std::tie(this_cpu, remain) = r;
        auto pu = hwloc_get_pu_obj_by_os_index(topology, this_cpu.cpu_id);
        auto node = get_numa_node_for_pu(topology, pu);
        auto obj = node;

        while (remain) {
            remain -= alloc_from_node(this_cpu, obj, topo_used_mem, remain);
            do {
                obj = hwloc_get_next_obj_by_depth(topology, depth, obj);
            } while (!obj);
            if (obj == node)
                break;
        }
        assert(!remain);
        ret.cpus.push_back(std::move(this_cpu));
    }

    unsigned last_node_idx = 0;
    for (auto d : c.num_io_queues) {
        auto devid = d.first;
        auto num_io_queues = d.second;
        ret.ioq_topology.emplace(devid, allocate_io_queues(topology, ret.cpus, num_io_queues, last_node_idx));
    }
    return ret;
}

unsigned nr_processing_units() {
    hwloc_topology_t topology;
    hwloc_topology_init(&topology);
    auto free_hwloc = defer([&] { hwloc_topology_destroy(topology); });
    hwloc_topology_load(topology);
    return hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PU);
}

}

}

#else

#include <seastar/core/resource.hh>
#include <unistd.h>

namespace seastar {

namespace resource {

// Without hwloc, we don't support tuning the number of IO queues. So each CPU gets their.
static io_queue_topology
allocate_io_queues(configuration c, std::vector<cpu> cpus) {
    io_queue_topology ret;

    unsigned nr_cpus = unsigned(cpus.size());
    ret.shard_to_coordinator.resize(nr_cpus);
    ret.coordinators.resize(nr_cpus);
    ret.coordinator_to_idx.resize(nr_cpus);
    ret.coordinator_to_idx_valid.resize(nr_cpus);

    for (unsigned shard = 0; shard < nr_cpus; ++shard) {
        ret.shard_to_coordinator[shard] = shard;
        ret.coordinators[shard] = shard;
        ret.coordinator_to_idx[shard] = shard;
        ret.coordinator_to_idx_valid[shard] = true;
    }
    return ret;
}


resources allocate(configuration c) {
    resources ret;

    auto available_memory = ::sysconf(_SC_PAGESIZE) * size_t(::sysconf(_SC_PHYS_PAGES));
    auto mem = calculate_memory(c, available_memory);
    auto cpuset_procs = c.cpu_set ? c.cpu_set->size() : nr_processing_units();
    auto procs = c.cpus.value_or(cpuset_procs);
    ret.cpus.reserve(procs);
    if (c.cpu_set) {
        for (auto cpuid : *c.cpu_set) {
            ret.cpus.push_back(cpu{cpuid, {{mem / procs, 0}}});
        }
    } else {
        for (unsigned i = 0; i < procs; ++i) {
            ret.cpus.push_back(cpu{i, {{mem / procs, 0}}});
        }
    }

    ret.ioq_topology.emplace(0, allocate_io_queues(c, ret.cpus));
    return ret;
}

unsigned nr_processing_units() {
    return ::sysconf(_SC_NPROCESSORS_ONLN);
}

}

}

#endif
