
/**
 * @file
 * Declaration of a LRU_NUCA tag store.
 */

#ifndef __MEM_CACHE_TAGS_LRU_NUCA_HH__
#define __MEM_CACHE_TAGS_LRU_NUCA_HH__

#include <cassert>
#include <cstring>
#include <list>

#include "mem/cache/tags/base.hh"
#include "mem/cache/tags/cacheset.hh"
#include "mem/cache/blk.hh"
#include "mem/packet.hh"
#include "params/LRU_NUCA.hh" //

class BaseCache;


/**
 * A LRU_NUCA cache tag store.
 * @sa  \ref gem5MemorySystem "gem5 Memory System"
 */
class LRU_NUCA : public BaseTags
{
  public:
    /** Typedef the block type used in this tag store. */
    typedef CacheBlk BlkType;
    /** Typedef for a list of pointers to the local block class. */
    typedef std::list<BlkType*> BlkList;
    /** Typedef the set type used in this tag store. */
    typedef CacheSet<CacheBlk> SetType;


  protected:

    /** The associativity of the cache. */
    const unsigned assoc;
    /** The number of sets in the cache. */
    const unsigned numSets;
    /** Whether tags and data are accessed sequentially. */
    const bool sequentialAccess;

    /** The latency of the cache. */
    const double basicReadLatency;
    const double basicWriteLatency;
    const double deltaReadLatency;
    const double deltaWriteLatency;
    const double localWriteLatency;
    const double hotZoneSize;
    /** The cache sets. */
    SetType *sets;

    /** The cache blocks. */
    BlkType *blks;
    /** The data blocks, 1 per cache block. */
    uint8_t *dataBlks;

    /** The amount to shift the address to get the set. */
    int setShift;
    /** The amount to shift the address to get the tag. */
    int tagShift;
    /** Mask out all bits that aren't part of the set index. */
    unsigned setMask;
    /** Mask out all bits that aren't part of the block offset. */
    unsigned blkMask;

    // statistics variables
    Stats::Scalar lru_nuca_access_num;
    Stats::Scalar lru_nuca_access_hotzone_cost;
    Stats::Scalar lru_nuca_access_coolzone_cost;

public:

    /** Convenience typedef. */
     typedef LRU_NUCAParams Params; //???

    /**
     * Construct and initialize this tag store.
     */
    LRU_NUCA(const Params *p);

    /**
     * Destructor
     */
    virtual ~LRU_NUCA();

    /**
     * Register the stats for this object.
     * @param name The name to prepend to the stats name.
     */
    void regStats();

    /**
     * Return the block size.
     * @return the block size.
     */
    unsigned
    getBlockSize() const
    {
        return blkSize;
    }

    /**
     * Return the subblock size. In the case of LRU_NUCA it is always the block
     * size.
     * @return The block size.
     */
    unsigned
    getSubBlockSize() const
    {
        return blkSize;
    }

    unsigned
    getBlockPosition(Addr addr, bool is_secure) const;
    Cycles
    calcLatency(Addr addr, bool is_secure, bool is_read) const;

    /**
     * Invalidate the given block.
     * @param blk The block to invalidate.
     */
    void invalidate(BlkType *blk);

    /**
     * Access block and update replacement data.  May not succeed, in which case
     * NULL pointer is returned.  This has all the implications of a cache
     * access and should only be used as such. Returns the access latency as a side effect.
     * @param addr The address to find.
     * @param is_secure True if the target memory space is secure.
     * @param asid The address space ID.
     * @param lat The access latency.
     * @return Pointer to the cache block if found.
     */
    BlkType* accessBlock(Addr addr, bool is_secure, Cycles &lat,
                         int context_src, bool is_read);

    /**
     * Finds the given address in the cache, do not update replacement data.
     * i.e. This is a no-side-effect find of a block.
     * @param addr The address to find.
     * @param is_secure True if the target memory space is secure.
     * @param asid The address space ID.
     * @return Pointer to the cache block if found.
     */
    BlkType* findBlock(Addr addr, bool is_secure) const;

    /**
     * Find a block to evict for the address provided.
     * @param addr The addr to a find a replacement candidate for.
     * @return The candidate block.
     */
    BlkType* findVictim(Addr addr);

    /**
     * Insert the new block into the cache.  For LRU_NUCA this means inserting into
     * the MRU position of the set.
     * @param pkt Packet holding the address to update
     * @param blk The block to update.
     */
     void insertBlock(PacketPtr pkt, BlkType *blk);

    /**
     * Generate the tag from the given address.
     * @param addr The address to get the tag from.
     * @return The tag of the address.
     */
    Addr extractTag(Addr addr) const
    {
        return (addr >> tagShift);
    }

    /**
     * Calculate the set index from the address.
     * @param addr The address to get the set from.
     * @return The set index of the address.
     */
    int extractSet(Addr addr) const
    {
        return ((addr >> setShift) & setMask);
    }

    /**
     * Get the block offset from an address.
     * @param addr The address to get the offset of.
     * @return The block offset.
     */
    int extractBlkOffset(Addr addr) const
    {
        return (addr & blkMask);
    }

    /**
     * Align an address to the block size.
     * @param addr the address to align.
     * @return The block address.
     */
    Addr blkAlign(Addr addr) const
    {
        return (addr & ~(Addr)blkMask);
    }

    /**
     * Regenerate the block address from the tag.
     * @param tag The tag of the block.
     * @param set The set of the block.
     * @return The block address.
     */
    Addr regenerateBlkAddr(Addr tag, unsigned set) const
    {
        return ((tag << tagShift) | ((Addr)set << setShift));
    }

    /**
     * Return the hit latency.
     * @return the hit latency.
     */
    Cycles getHitLatency() const
    {
        return hitLatency;
    }
    /**
     *iterated through all blocks and clear all locks
     *Needed to clear all lock tracking at once
     */
    virtual void clearLocks();

    /**
     * Called at end of simulation to complete average block reference stats.
     */
    virtual void cleanupRefs();

    /**
     * Print all tags used
     */
    virtual std::string print() const;

    /**
     * Called prior to dumping stats to compute task occupancy
     */
    virtual void computeStats();

    /**
     * Visit each block in the tag store and apply a visitor to the
     * block.
     *
     * The visitor should be a function (or object that behaves like a
     * function) that takes a cache block reference as its parameter
     * and returns a bool. A visitor can request the traversal to be
     * stopped by returning false, returning true causes it to be
     * called for the next block in the tag store.
     *
     * \param visitor Visitor to call on each block.
     */
    template <typename V>
    void forEachBlk(V &visitor) {
        for (unsigned i = 0; i < numSets * assoc; ++i) {
            if (!visitor(blks[i]))
                return;
        }
    }
};

#endif // __MEM_CACHE_TAGS_LRU_HH__
