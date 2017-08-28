/**
 * @file
 * Definitions of T_NUCA tag store.
 */

#include <string>

#include "base/intmath.hh"
#include "debug/Cache.hh"
#include "debug/CacheRepl.hh"
#include "mem/cache/tags/t_nuca.hh"
#include "mem/cache/base.hh"
#include "sim/core.hh"

using namespace std;

T_NUCA::T_NUCA(const Params *p)
    :BaseTags(p),
     basicLatency(10),
     deltaLatency(1),
     localReadLatency(0),
     localWriteLatency_hot(20),
     localWriteLatency_cool(27),
     zoneSwapFlag(false),
     costRatio(6),                         // change here
     maxNumOfCounter(16),
     assoc(p->assoc),
     numSets(p->size / (p->block_size * p->assoc)),
     sequentialAccess(p->sequential_access)
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
    if (basicLatency <= 0) {
        fatal("access latency must be greater than zero");
    }
    if (deltaLatency<=0 || localReadLatency <0 
      || localWriteLatency_hot<=0 || localWriteLatency_cool<=0){
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
    hot_zone = new unsigned[numSets];
    hot_zone[0] = 4;
    hot_zone[1] = 4;
    hot_zone[2] = 4;
    hot_zone[3] = 4;
    for (int i = 4;i<numSets;++i)
        hot_zone[i] = 4;
    hotZoneSize = hot_zone;
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
    delete [] hotZoneSize;
}

void
T_NUCA::regStats()
{
    using namespace Stats;
    BaseTags::regStats();
    t_nuca_read
	.init(assoc)
	.name(name() + ".t_nuca_read")
	.desc("The number of read request in each position.")
	.flags(total | nozero | nonan);
    t_nuca_write
	.init(assoc)
	.name(name() + ".t_nuca_write")
	.desc("The number of write request in each position.")
	.flags(total | nozero | nonan);
    t_nuca_cost
        .init(assoc)
	.name(name() + ".t_nuca_cost")
	.desc("The cost in each position.")
	.flags(total | nozero | nonan);
//    t_nuca_cost_by_read
//	.init(assoc)
//	.name(name() + ".t_nuca_cost_by_read")
//	.desc("The cost caused by read request in each position.")
//	.flags(total);
//    t_nuca_cost_by_write
//	.init(assoc)
//	.name(name() + ".t_nuca_cost_by_write")
//	.desc("The cost caused by write request in each position.")
//	.flag(total);
    t_nuca_access_num
        .name(name() + ".t_nuca_access_number")
        .desc("The total number of hit accesses in T_NUCA cache.");
    t_nuca_cost_num
        .name(name() + ".t_nuca_cost_number")
        .desc("The total number of cost caused by hit access in T_NUCA cache.");

    t_nuca_insert_num
        .name(name() + ".t_nuca_insert_number")
        .desc("The number of insert in T_NUCA cache.");

    for (int ii = 0;ii<assoc;++ii) {
	t_nuca_read.subname(ii, std::string("Position-") + std::to_string(ii));
	t_nuca_write.subname(ii, std::string("Position-") + std::to_string(ii));
	t_nuca_cost.subname(ii, std::string("Position-") + std::to_string(ii));
    }

    t_nuca_hotzone_cost
        .name(name() + ".t_nuca_hotzone_cost")
        .desc("Total cost of hot zone in T_NUCA cache.");
    t_nuca_coolzone_cost
        .name(name() + ".t_nuca_coolzone_cost")
        .desc("Total cost of cool zone in T_NUCA cache.");
    
    for(int ii = 0;ii < hotZoneSize; ++ii) {
        t_nuca_hotzone_cost += t_nuca_cost[ii];
    }
    for(int ii = hotZoneSize;ii < assoc; ++ii) {
        t_nuca_coolzone_cost += t_nuca_cost[ii];
    }
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
        incCostCount(addr, is_secure, is_read, master_id);
        // change position
        updatePosition(addr, is_secure, is_read);
        ///
        DPRINTF(CacheRepl, "set %x: moving blk %x (%s) to MRU\n",
                set, regenerateBlkAddr(tag, set), is_secure ? "s" : "ns");
        if (blk->whenReady > curTick()
            && cache->ticksToCycles(blk->whenReady - curTick()) > hitLatency) {
            lat = cache->ticksToCycles(blk->whenReady - curTick());
        }
    }
    return blk;
}

// changed position
void
T_NUCA::clearAllCounter(unsigned set) {
    for (unsigned ii = 0;ii < assoc;++ii) {
        BlkType* blk = sets[set].getBlk(ii);
        totalRefs += blk->refCount;
        blk->refCount = 0;
    }
}

int
T_NUCA::getBlockPosition(Addr addr, bool is_secure) const {
    Addr tag = extractTag(addr);
    unsigned set = extractSet(addr);
    int posi = sets[set].findBlkPosition(tag, is_secure);
    return posi;
}

Cycles
T_NUCA::calcLatency(Addr addr, bool is_secure, bool is_read) const {
    int posi = getBlockPosition(addr, is_secure);
    // if element is not found
    if (posi < 0)
        return Cycles(basicLatency);
    // latency caused by write or position update
    unsigned lat;

    // calculate the destination of a swap, to judge whether it will be moved
    bool zone_swap_flag = zoneSwapFlag; // can't use zoneSwapFlag directly
                                        // because calcUpdatePosition may
                                        // change its value
    int des_posi;  //result
    calcUpdatePosition(addr, is_secure, is_read,
        zone_swap_flag, posi, des_posi);

    double localWriteLatency;
    // local write latency are different in different zone
    if (posi < hotZoneSize) // if in hot zone.
        localWriteLatency = localWriteLatency_hot;
    else
        localWriteLatency = localWriteLatency_cool;
    // latency cause by read
    if (is_read) {
        lat = (unsigned)(basicLatency + localReadLatency
            + posi * deltaLatency + 0.001);
    }
    else
        lat = (unsigned)(basicLatency + localWriteLatency
            + posi * deltaLatency + 0.001);
    // if block will be moved
    if (des_posi != posi)
        lat += localWriteLatency;
    return Cycles(lat);
}

void
T_NUCA::updatePosition(Addr addr, bool is_secure, bool is_read) {
    unsigned set = extractSet(addr);
    int posi1, posi2;
    calcUpdatePosition(addr, is_secure, is_read, zoneSwapFlag,
        posi1, posi2);
    // change position
    BlkType *blk = sets[set].getBlk(posi1);
    if (is_read)
        blk->refCount += 1;
    else
        blk->refCount += costRatio;
    if (blk->refCount >= maxNumOfCounter)
        clearAllCounter(set);
    sets[set].swap(posi1, posi2);

}

void
T_NUCA::calcUpdatePosition(Addr addr, bool is_secure, bool is_read,
    bool& zone_swap_flag, int& src_posi, int& des_posi) const
{
    unsigned set = extractSet(addr);
    src_posi = getBlockPosition(addr, is_secure);

    unsigned hot_zone_size = hotZoneSize[set];
    // if srcoure block is in hot zone,
    // it should be swaped by the block before it
    if (src_posi == 0) {
        des_posi = 0;
        return;
    } 
    if (src_posi < hot_zone_size){
        des_posi = src_posi - 1;
        return;
    }

    // cool zone
    // if it's the top of cool zone
    if (src_posi == hot_zone_size) {
        // if it's been swaped last time
        if (zone_swap_flag) {
            des_posi = hotZoneSize - 2;
            zone_swap_flag = false;
        }
        else
        {
            des_posi = hot_zone_size - 1;
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
    } while(ii >= hot_zone_size);
    des_posi = ii + 1;
    return;
}

void 
T_NUCA::incCostCount(Addr addr, bool is_secure, bool is_read, int id) {
    int posi1, posi2;
    bool tempFlag = zoneSwapFlag;
    calcUpdatePosition(addr, is_secure, is_read, tempFlag,
        posi1, posi2);

    ++t_nuca_access_num;
    if (is_read)
	++t_nuca_read[posi1];
    else
	++t_nuca_write[posi1];

    if (is_read) // read access dosen't have cost itself
    {
        if (posi1 != posi2) // swap occurs will cause cost
        {
            ++t_nuca_cost[posi1];
            ++t_nuca_cost[posi2];
	    t_nuca_cost_num += 2;
        }
    }
    else { // write access
        // there is one cost whether swap occurs
        ++t_nuca_cost[posi1];
	++t_nuca_cost_num;
        if (posi1 != posi2) { // swap occurs
            ++t_nuca_cost[posi2];
	    ++t_nuca_cost_num;
        }
    }
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
        assert(blk->srcMasterId < ContextSwitchTaskId::NumTaskId);
        occupancies[blk->srcMasterId]--;

        blk->invalidate();
    }

    blk->isTouched = true;
    // Set tag for new block.  Caller is responsible for setting status.
    blk->tag = extractTag(addr);
    if (is_secure)
        blk->status |= BlkSecure;

    // deal with what we are bringing in
    assert(master_id < ContextSwitchTaskId::NumTaskId);
    occupancies[master_id]++;
    blk->srcMasterId = master_id;
    blk->task_id = task_id;
    blk->tickInserted = curTick();

    //updatePosition(addr, is_secure, false);
    ++t_nuca_insert_num;
    unsigned set = blk->set;
    clearAllCounter(set);

    // We only need to write into one tag and one data block.
    tagAccesses += 1;
    dataAccesses += 1;
}

void
T_NUCA::invalidate(BlkType *blk)
{
    assert(blk);
    assert(blk->isValid());
    tagsInUse--;
    assert(blk->srcMasterId < ContextSwitchTaskId::NumTaskId);
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
