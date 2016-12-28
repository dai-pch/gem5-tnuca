/**
 * @file
 * Definitions of T_NUCA tag store.
 */

#include <string>

#include "base/intmath.hh"
#include "debug/Cache.hh"
#include "debug/CacheRepl.hh"
#include "mem/cache/tags/T_NUCA.hh"
#include "mem/cache/base.hh"
#include "sim/core.hh"

using namespace std;

T_NUCA::T_NUCA(const Params *p)
    :BaseTags(p), assoc(p->assoc),
     numSets(p->size / (p->block_size * p->assoc)),
     sequentialAccess(p->sequential_access),
     zoneSwapFlag(false),
     basicReadLatency(0),
     basicWriteLatency(0),
     deltaReadLatency(0),
     deltaWriteLatency(0),
     localWriteLatency(0),
     hotZoneSize(4),
     maxNumOfCounter(64),
     costRatio(10)                          // change here
{
    // Check parameters
    if (blkSize < 4 || !isPowerOf2(blkSize)) {
        fatal("Block size must be at least 4 and a power of 2");
    }
    if (numSets <= 0 || !isPowerOf2(numSets)) {
        fatal("# of sets must be non-zero and a power of 2");
    }
    if (assoc <= 0) {
        fatal("associativity must be greater than zero");
    }
    if (hitLatency <= 0) {
        fatal("access latency must be greater than zero");
    }
    if (basicReadLatency<=0 || basicWriteLatency<=0
      || deltaReadLatency <=0 || deltaWriteLatency<=0){
        fatal("access latency must be positive");
    }

    blkMask = blkSize - 1;
    setShift = floorLog2(blkSize);
    setMask = numSets - 1;
    tagShift = setShift + floorLog2(numSets);
    warmedUp = false;
    /** @todo Make warmup percentage a parameter. */
    warmupBound = numSets * assoc;

    sets = new SetType[numSets];
    blks = new BlkType[numSets * assoc];
    // allocate data storage in one big chunk
    numBlocks = numSets * assoc;
    dataBlks = new uint8_t[numBlocks * blkSize];

    unsigned blkIndex = 0;       // index into blks array
    for (unsigned i = 0; i < numSets; ++i) {
        sets[i].assoc = assoc;

        sets[i].blks = new BlkType*[assoc];

        // link in the data blocks
        for (unsigned j = 0; j < assoc; ++j) {
            // locate next cache block
            BlkType *blk = &blks[blkIndex];
            blk->data = &dataBlks[blkSize*blkIndex];
            ++blkIndex;

            // invalidate new cache block
            blk->invalidate();

            //EGH Fix Me : do we need to initialize blk?

            // Setting the tag to j is just to prevent long chains in the hash
            // table; won't matter because the block is invalid
            blk->tag = j;
            blk->whenReady = 0;
            blk->isTouched = false;
            blk->size = blkSize;
            sets[i].blks[j]=blk;
            blk->set = i;
        }
    }
}

T_NUCA::~T_NUCA()
{
    delete [] dataBlks;
    delete [] blks;
    delete [] sets;
}

T_NUCA::BlkType*
T_NUCA::accessBlock(Addr addr, bool is_secure, Cycles &lat,
    int master_id, bool is_read)
{
    Addr tag = extractTag(addr);
    unsigned set = extractSet(addr);
    BlkType *blk = sets[set].findBlk(tag, is_secure);
    lat = calcLatency(addr, is_secure, is_read);

    // Access all tags in parallel, hence one in each way.  The data side
    // either accesses all blocks in parallel, or one block sequentially on
    // a hit.  Sequential access with a miss doesn't access data.
    tagAccesses += assoc;
    if (sequentialAccess) {
        if (blk != NULL) {
            dataAccesses += 1;
        }
    } else {
        dataAccesses += assoc;
    }

    if (blk != NULL) {
        // changed position
        updatePosition(addr, is_secure);
        ///
        DPRINTF(CacheRepl, "set %x: moving blk %x (%s) to MRU\n",
                set, regenerateBlkAddr(tag, set), is_secure ? "s" : "ns");
        if (blk->whenReady > curTick()
            && cache->ticksToCycles(blk->whenReady - curTick()) > hitLatency) {
            lat = cache->ticksToCycles(blk->whenReady - curTick());
        }
        // changed position
        if (is_read)
            blk.refCount += 1;
        else
            blk.refCount += costRatio;
        if (blk.refCount >= maxNumOfCounter)
            clearAllCounter(set);
        ///
    }
    return blk;
}

// changed position
void
clearAllCounter(unsigned set) {
    for (ii = 0;ii < assoc;++ii) {
        BlkType* blk = sets[set].getBlk(ii);
        totalRefs += blk->refCount;
        blk->refCount = 0;
    }
}

unsigned
T_NUCA::getBlockPosition(Addr addr, bool is_secure) const {
    Addr tag = extractTag(addr);
    unsigned set = extractSet(addr);
    unsigned posi = sets[set].findBlkPosition(tag, is_secure);
    return posi;
}

Cycles
T_NUCA::calcLatency(Addr addr, bool is_secure, bool is_read) const {
    unsigned posi = getBlockPosition(addr, is_secure);
    // if element is not found
    if (posi < 0)
        return Cycles(hitLatency);
    // latency caused by write or position update
    unsigned lat;

    // calculate the destination of a swap, to judge whether it will be moved
    bool zone_swap_falg = zoneSwapFlag; // can't use zoneSwapFlag directly
                                        // because calcUpdatePosition may
                                        // change its value
    unsigned des_posi;  //result
    calcUpdatePosition(addr, is_secure, is_read,
        zone_swap_flag, posi, des_posi);

    // latency cause by read
    if (is_read) {
        lat = (unsigned)(basicReadLatency +
            posi * deltaReadLatency + 0.001);
        // if block will be moved
        if (des_posi != posi)
            lat += localWriteLatency;
    }
    else
        lat = (unsigned)(basicWriteLatency +
            posi * deltaWriteLatency + 0.001);
    return Cycles(lat);
}

void
T_NUCA::updatePosition(Addr addr, bool is_secure) {
    unsigned set = extractSet(addr);
    unsigned posi1, posi2;
    calcUpdatePosition(addr, is_secure, zoneSwapFlag,
        posi1, posi2);
    sets[set].swap(posi1, posi2);
}

void
T_NUCA::calcUpdatePosition(Addr addr, bool is_secure, bool is_read,
    bool& zone_swap_flag, unsigned& src_posi, unsigned& des_posi)
{
    unsigned set = extractSet(addr);
    unsigned src_posi = getBlockPosition(addr, is_secure);

    // if srcoure block is in hot zone,
    // it should be swaped by head block
    if (src_posi < hotZoneSize){
        des_posi = 0;
        return;
    }

    // cool zone
    // if it's the top of cool zone
    if (src_posi == hotZoneSize) {
        // if it's been swaped last time
        if (zone_swap_flag) {
            des_posi = hotZoneSize - 2;
            zone_swap_flag = false;
        }
        else
        {
            des_posi = hotZoneSize - 1;
            zone_swap_flag = true;
        }
        return;
    }

    // if it's in cool zone but not top
    int src_count = (sets[set].getBlk(src_posi))->refCount;
    if (is_read)
        src_count += 1;
    else
        src_count += costRatio;
    src_count /= costRatio;
    unsigned ii = src_posi - 1;
    do{
        int temp_count = (sets[set].getBlk(ii))->refCount;
        temp_count /= costRatio;
        if (src_count <= temp_count)
            break;
        --ii;
    } while(ii >= hotZoneSize);
    des_posi = ii + 1;
    return;
}

///

T_NUCA::BlkType*
T_NUCA::findBlock(Addr addr, bool is_secure) const
{
    Addr tag = extractTag(addr);
    unsigned set = extractSet(addr);
    BlkType *blk = sets[set].findBlk(tag, is_secure);
    return blk;
}

T_NUCA::BlkType*
T_NUCA::findVictim(Addr addr)
{
    unsigned set = extractSet(addr);
    // grab a replacement candidate
    BlkType *blk = sets[set].blks[assoc-1];

    if (blk->isValid()) {
        DPRINTF(CacheRepl, "set %x: selecting blk %x for replacement\n",
                set, regenerateBlkAddr(blk->tag, set));
    }
    return blk;
}

void
T_NUCA::insertBlock(PacketPtr pkt, BlkType *blk)
{
    Addr addr = pkt->getAddr();
    MasterID master_id = pkt->req->masterId();
    uint32_t task_id = pkt->req->taskId();
    bool is_secure = pkt->isSecure();
    if (!blk->isTouched) {
        tagsInUse++;
        blk->isTouched = true;
        if (!warmedUp && tagsInUse.value() >= warmupBound) {
            warmedUp = true;
            warmupCycle = curTick();
        }
    }

    // If we're replacing a block that was previously valid update
    // stats for it. This can't be done in findBlock() because a
    // found block might not actually be replaced there if the
    // coherence protocol says it can't be.
    if (blk->isValid()) {
        replacements[0]++;
        totalRefs += blk->refCount;
        ++sampledRefs;
        blk->refCount = 0;

        // deal with evicted block
        assert(blk->srcMasterId < cache->system->maxMasters());
        occupancies[blk->srcMasterId]--;

        blk->invalidate();
    }

    blk->isTouched = true;
    // Set tag for new block.  Caller is responsible for setting status.
    blk->tag = extractTag(addr);
    if (is_secure)
        blk->status |= BlkSecure;

    // deal with what we are bringing in
    assert(master_id < cache->system->maxMasters());
    occupancies[master_id]++;
    blk->srcMasterId = master_id;
    blk->task_id = task_id;
    blk->tickInserted = curTick();

    updatePosition(addr, is_secure);

    // We only need to write into one tag and one data block.
    tagAccesses += 1;
    dataAccesses += 1;
}

void
T_NUCA::invalidate(BlkType *blk, Cycles& lat)
{
    assert(blk);
    assert(blk->isValid());
    tagsInUse--;
    assert(blk->srcMasterId < cache->system->maxMasters());
    occupancies[blk->srcMasterId]--;
    blk->srcMasterId = Request::invldMasterId;
    blk->task_id = ContextSwitchTaskId::Unknown;
    blk->tickInserted = curTick();

    // should be evicted before valid blocks
    unsigned set = blk->set;
    sets[set].moveToTail(blk);
}

void
T_NUCA::clearLocks()
{
    for (int i = 0; i < numBlocks; i++){
        blks[i].clearLoadLocks();
    }
}

T_NUCA *
T_NUCAParams::create()
{
    return new T_NUCA(this);
}
std::string
T_NUCA::print() const {
    std::string cache_state;
    for (unsigned i = 0; i < numSets; ++i) {
        // link in the data blocks
        for (unsigned j = 0; j < assoc; ++j) {
            BlkType *blk = sets[i].blks[j];
            if (blk->isValid())
                cache_state += csprintf("\tset: %d block: %d %s\n", i, j,
                        blk->print());
        }
    }
    if (cache_state.empty())
        cache_state = "no valid tags\n";
    return cache_state;
}

void
T_NUCA::cleanupRefs()
{
    for (unsigned i = 0; i < numSets*assoc; ++i) {
        if (blks[i].isValid()) {
            totalRefs += blks[i].refCount;
            ++sampledRefs;
        }
    }
}

void
T_NUCA::computeStats()
{
    for (unsigned i = 0; i < ContextSwitchTaskId::NumTaskId; ++i) {
        occupanciesTaskId[i] = 0;
        for (unsigned j = 0; j < 5; ++j) {
            ageTaskId[i][j] = 0;
        }
    }

    for (unsigned i = 0; i < numSets * assoc; ++i) {
        if (blks[i].isValid()) {
            assert(blks[i].task_id < ContextSwitchTaskId::NumTaskId);
            occupanciesTaskId[blks[i].task_id]++;
            Tick age = curTick() - blks[i].tickInserted;
            assert(age >= 0);

            int age_index;
            if (age / SimClock::Int::us < 10) { // <10us
                age_index = 0;
            } else if (age / SimClock::Int::us < 100) { // <100us
                age_index = 1;
            } else if (age / SimClock::Int::ms < 1) { // <1ms
                age_index = 2;
            } else if (age / SimClock::Int::ms < 10) { // <10ms
                age_index = 3;
            } else
                age_index = 4; // >10ms

            ageTaskId[blks[i].task_id][age_index]++;
        }
    }
}
