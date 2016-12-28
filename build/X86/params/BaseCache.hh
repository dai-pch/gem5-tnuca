#ifndef __PARAMS__BaseCache__
#define __PARAMS__BaseCache__

class BaseCache;

#include <cstddef>
#include "base/types.hh"
#include <cstddef>
#include "params/BasePrefetcher.hh"
#include <cstddef>
#include <cstddef>
#include "base/types.hh"
#include <cstddef>
#include "base/types.hh"
#include <cstddef>
#include <cstddef>
#include "base/types.hh"
#include <cstddef>
#include "base/types.hh"
#include <cstddef>
#include "params/System.hh"
#include <cstddef>
#include "base/types.hh"
#include <cstddef>
#include "base/types.hh"
#include <cstddef>
#include <cstddef>
#include "params/BaseTags.hh"
#include <cstddef>
#include "base/types.hh"
#include <vector>
#include "base/types.hh"
#include "base/addr_range.hh"
#include <cstddef>
#include "base/types.hh"
#include <cstddef>
#include <cstddef>
#include "base/types.hh"
#include <cstddef>
#include "base/types.hh"
#include <cstddef>
#include "base/types.hh"
#include <cstddef>
#include <cstddef>

#include "params/MemObject.hh"

struct BaseCacheParams
    : public MemObjectParams
{
    BaseCache * create();
    int num_banks;
    BasePrefetcher * prefetcher;
    bool enable_bank_model;
    int write_buffers;
    Cycles response_latency;
    bool is_top_level;
    uint64_t size;
    Cycles write_latency;
    System * system;
    Counter max_miss_count;
    int mshrs;
    bool forward_snoops;
    BaseTags * tags;
    int tgts_per_mshr;
    std::vector< AddrRange > addr_ranges;
    int assoc;
    bool prefetch_on_access;
    Cycles read_latency;
    int Num_bank_per_group;
    int bank_intlv_high_bit;
    bool sequential_access;
    bool two_queue;
    unsigned int port_mem_side_connection_count;
    unsigned int port_cpu_side_connection_count;
};

#endif // __PARAMS__BaseCache__
