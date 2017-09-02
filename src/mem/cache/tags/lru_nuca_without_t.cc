/**
 * @file
 * Definitions of LRU_NUCA tag store.
 */

#include <string>

#include "base/intmath.hh"
#include "debug/Cache.hh"
#include "debug/CacheRepl.hh"
#include "mem/cache/tags/lru_nuca_without_t.hh"
#include "mem/cache/base.hh"
#include "sim/core.hh"

using namespace std;

LRU_NUCA::LRU_NUCA(const Params *p)
    :BaseTags(p), assoc(p->assoc),
     numSets(p->size / (p->block_size * p->assoc)),
     sequentialAccess(p->sequential_access),
     basicLatency(2),
     deltaLatency(1),
     localReadLatency(0),
     localWriteLatency(27),
     hotZoneSize(4)                        // change here
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
    if (localReadLatency<0 || localWriteLatency<=0
      || deltaLatency <=0 ){
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

LRU_NUCA::~LRU_NUCA()
{
    delete [] dataBlks;
    delete [] blks;
    delete [] sets;
}

void
LRU_NUCA::regStats()
{
  using namespace Stats;
  BaseTags::regStats();
  lru_nuca_access_num
      .name(name() + ".lru_nuca_access_number")
      .desc("The number of read type accesses in LRU_NUCA cache.");
  lru_nuca_access_hotzone_cost
      .name(name() + ".lru_nuca_access_hotzone_cost")
      .desc("The number of data changing in hotzone caused by read/write accesses in LRU_NUCA cache.");
  lru_nuca_access_coolzone_cost
      .name(name() + ".lru_nuca_access_coolzone_cost")
      .desc("The number of data changing in coolzone caused by read/write accesses in LRU_NUCA cache.");
}


LRU_NUCA::BlkType*
LRU_NUCA::accessBlock(Addr addr, bool is_secure, Cycles &lat,
    int master_id, bool is_read)
{
    Addr tag = extractTag(addr);
    unsigned set = extractSet(addr);
    BlkType *blk = sets[set].findBlk(tag, is_secure);
    // initial latency is detemined by Cache.
    // lat = calcLatency(addr, is_secure, is_read);

    // Access all tags in parallel, hence one in each way.  The data side
    // either accesses all blocks in parallel, or one block sequentially on
    // a hit.  Sequential access with a miss doesn't access data.
    tagAccesses += assoc;
    //lru_nuca_access_num += 1;
    if (sequentialAccess) {
        if (blk != NULL) {
            dataAccesses += 1;
        }
    } else {
        dataAccesses += assoc;
    }

    if (blk != NULL) {
        //statistics
		unsigned posi = getBlockPosition(addr, is_secure);
		if (posi > hotZoneSize){//in coolzone
          lru_nuca_access_coolzone_cost += 2;
        }
		else if (posi == hotZoneSize){//at the border
          ++lru_nuca_access_coolzone_cost;
          ++lru_nuca_access_hotzone_cost;
        }
        else{//hotzone
			if (posi != 0){//not in header
            lru_nuca_access_hotzone_cost += 2;
          }
          else{
            lru_nuca_access_hotzone_cost = lru_nuca_access_hotzone_cost;
          }
        }
        // move this block to head of the MRU list
        sets[set].bubble(blk);
        //count the access (not include miss)
        lru_nuca_access_num += 1;

        DPRINTF(CacheRepl, "set %x: moving blk %x (%s) to MRU\n",
                set, regenerateBlkAddr(tag, set), is_secure ? "s" : "ns");
        if (blk->whenReady > curTick()
            && cache->ticksToCycles(blk->whenReady - curTick()) > hitLatency) {
            lat = cache->ticksToCycles(blk->whenReady - curTick());
        }
        blk->refCount += 1;
    }

    return blk;
}

unsigned
LRU_NUCA::getBlockPosition(Addr addr, bool is_secure) const {
    Addr tag = extractTag(addr);
    unsigned set = extractSet(addr);
    unsigned posi = sets[set].findBlkPosition(tag, is_secure);
    return posi;
}

Cycles
LRU_NUCA::calcLatency(Addr addr, bool is_secure, bool is_read) const {
    unsigned posi = getBlockPosition(addr, is_secure);
    // if element is not found
    if (posi < 0)
        return Cycles(hitLatency);
    // latency caused by write or position update
    unsigned lat;
    // latency cause by read
    if (is_read)
        lat = (unsigned)(basicLatency + localReadLatency + localWriteLatency
            + posi * deltaLatency + 0.001);
    else
        lat = (unsigned)(basicLatency + localWriteLatency + localWriteLatency
            + posi * deltaLatency + 0.001);
    return Cycles(lat);
}

LRU_NUCA::BlkType*
LRU_NUCA::findBlock(Addr addr, bool is_secure) const
{
    Addr tag = extractTag(addr);
    unsigned set = extractSet(addr);
    BlkType *blk = sets[set].findBlk(tag, is_secure);
    return blk;
}

LRU_NUCA::BlkType*
LRU_NUCA::findVictim(Addr addr)
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
LRU_NUCA::insertBlock(PacketPtr pkt, BlkType *blk)
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

    unsigned set = extractSet(addr);
    sets[set].moveToHead(blk);


    // We only need to write into one tag and one data block.
    tagAccesses += 1;
    dataAccesses += 1;
}

void
LRU_NUCA::invalidate(BlkType *blk)
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
LRU_NUCA::clearLocks()
{
    for (int i = 0; i < numBlocks; i++){
        blks[i].clearLoadLocks();
    }
}

LRU_NUCA *
LRU_NUCAParams::create()
{
    return new LRU_NUCA(this);
}
std::string
LRU_NUCA::print() const {
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
LRU_NUCA::cleanupRefs()
{
    for (unsigned i = 0; i < numSets*assoc; ++i) {
        if (blks[i].isValid()) {
            totalRefs += blks[i].refCount;
            ++sampledRefs;
        }
    }
}

void
LRU_NUCA::computeStats()
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
