#ifndef __PARAMS__LRU_NUCA__
#define __PARAMS__LRU_NUCA__

class LRU_NUCA;

#include <cstddef>
#include <cstddef>
#include "base/types.hh"

#include "params/BaseTags.hh"

struct LRU_NUCAParams
    : public BaseTagsParams
{
    LRU_NUCA * create();
    bool sequential_access;
    int assoc;
};

#endif // __PARAMS__LRU_NUCA__
