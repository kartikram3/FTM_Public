/** $lic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 *
 * This file is part of zsim.
 *
 * zsim is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2.
 *
 * If you use this software in your research, we request that you reference
 * the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
 * Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2013) as the
 * source of the simulator in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef CACHE_ARRAYS_H_
#define CACHE_ARRAYS_H_

#include "memory_hierarchy.h"
#include "stats.h"
#include "mtrand.h"

/* General interface of a cache array. The array is a fixed-size associative container that
 * translates addresses to line IDs. A line ID represents the position of the tag. The other
 * cache components store tag data in non-associative arrays indexed by line ID.
 */
class CacheArray : public GlobAlloc {
    public:
        /* Returns tag's ID if present, -1 otherwise. If updateReplacement is set, call the replacement policy's update() on the line accessed*/
        virtual int32_t lookup(const Address lineAddr, const MemReq* req, bool updateReplacement) = 0;

        /* Runs replacement scheme, returns tag ID of new pos and address of line to write back*/
        virtual uint32_t preinsert(const Address lineAddr, const MemReq* req, Address* wbLineAddr) = 0;

        /* Actually do the replacement, writing the new address in lineId.
         * NOTE: This method is guaranteed to be called after preinsert, although
         * there may be some intervening calls to lookup. The implementation is
         * allowed to keep internal state in preinsert() and use it in postinsert()
         */
        virtual void postinsert(const Address lineAddr, const MemReq* req, uint32_t lineId) = 0;

        virtual void initStats(AggregateStat* parent) {}

        virtual uint64_t getAddr(uint32_t set, int way) { return 0;}

        virtual int getSwitch(uint32_t set){ return 0;};
        virtual int setSwitch(uint32_t set){ return 1;};
        virtual uint32_t getReplSet(uint64_t lineAddr){ return 0; };
        virtual uint64_t getReplAddr(uint32_t set){ return 0; };
        virtual uint64_t getReplId(uint32_t set, uint32_t & lineId){ return 0; };
        virtual void switchHash(){;};
        virtual void resetSwitches(){;};
        virtual void moveAddr(Address repl_addr, uint32_t repl_id, 
                              Address lineAddr, uint32_t lineId){;};
        int* switch_array;

        virtual int getAssoc(){return 0;};
        virtual int getNumSets(){return 0;};

        /* FTM  */
        virtual void setLLC(bool _isLLC){;};
        /* FTM  */

};

class ReplPolicy;
class HashFamily;

/* Set-associative cache array */
class SetAssocArray : public CacheArray {
    protected:
        Address* array;
        ReplPolicy* rp;
        HashFamily* hf;
        uint32_t numLines;
        uint32_t numSets;
        uint32_t assoc;
        uint32_t setMask;


    public:

        /** Set and Way Partitioning **/
        /** ****** **/

        int isLLC;
        void setLLC(bool _isLLC) {isLLC = _isLLC;}

        int partitionSetCount;
        uint32_t partitionMask; 
        uint32_t partitionAssoc;
        /** ****** **/


        SetAssocArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* _hf);

        int32_t lookup(const Address lineAddr, const MemReq* req, bool updateReplacement);
        uint32_t preinsert(const Address lineAddr, const MemReq* req, Address* wbLineAddr);
        void postinsert(const Address lineAddr, const MemReq* req, uint32_t candidate);
};

/* CEASER style random cache array */
class CEASERArray : public CacheArray {
    protected:
        Address* array;
        Address* array_reuse;
        ReplPolicy* rp;
        HashFamily* hf;
        HashFamily* hf_current;
        HashFamily* hf_target;
        uint32_t numLines;
        uint32_t numSets;
        uint32_t assoc;
        uint32_t setMask;
        MTRand *rng;

    public:
        CEASERArray(uint32_t _numLines, uint32_t _assoc, ReplPolicy* _rp, HashFamily* hf_1, HashFamily *hf_2);

        int32_t lookup(const Address lineAddr, const MemReq* req, bool updateReplacement);
        uint32_t preinsert(const Address lineAddr, const MemReq* req, Address* wbLineAddr);
        void postinsert(const Address lineAddr, const MemReq* req, uint32_t candidate);
        uint64_t getAddr(uint32_t set, int way);
        int getSwitch(uint32_t set);
        int setSwitch(uint32_t set);
        uint32_t getReplSet(uint64_t lineAddr);
        uint64_t getReplAddr(uint32_t set);
        uint64_t getReplId(uint32_t set, uint32_t & lineId);
        void switchHash();
        void resetSwitches();
        void moveAddr(Address repl_addr, uint32_t repl_id, Address lineAddr, uint32_t lineId);
        int getAssoc(){ return assoc;}
        int getNumSets(){ return numSets;}
};

/* The cache array that started this simulator :) */
class ZArray : public CacheArray {
    private:
        Address* array; //maps line id to address
        uint32_t* lookupArray; //maps physical position to lineId
        ReplPolicy* rp;
        HashFamily* hf;
        uint32_t numLines;
        uint32_t numSets;
        uint32_t ways;
        uint32_t cands;
        uint32_t setMask;

        //preinsert() stores the swaps that must be done here, postinsert() does the swaps
        uint32_t* swapArray; //contains physical positions
        uint32_t swapArrayLen; //set in preinsert()

        uint32_t lastCandIdx;

        Counter statSwaps;

    public:
        ZArray(uint32_t _numLines, uint32_t _ways, uint32_t _candidates, ReplPolicy* _rp, HashFamily* _hf);

        int32_t lookup(const Address lineAddr, const MemReq* req, bool updateReplacement);
        uint32_t preinsert(const Address lineAddr, const MemReq* req, Address* wbLineAddr);
        void postinsert(const Address lineAddr, const MemReq* req, uint32_t candidate);

        //zcache-specific, since timing code needs to know the number of swaps, and these depend on idx
        //Should be called after preinsert(). Allows intervening lookups
        uint32_t getLastCandIdx() const {return lastCandIdx;}

        void initStats(AggregateStat* parentStat);
};

// Simple wrapper classes and iterators for candidates in each case; simplifies replacement policy interface without sacrificing performance
// NOTE: All must implement the same interface and be POD (we pass them by value)
struct SetAssocCands {
    struct iterator {
        uint32_t x;
        explicit inline iterator(uint32_t _x) : x(_x) {}
        inline void inc() {x++;} //overloading prefix/postfix too messy
        inline uint32_t operator*() const { return x; }
        inline bool operator==(const iterator& it) const { return it.x == x; }
        inline bool operator!=(const iterator& it) const { return it.x != x; }
    };

    uint32_t b, e;
    inline SetAssocCands(uint32_t _b, uint32_t _e) : b(_b), e(_e) {}
    inline iterator begin() const {return iterator(b);}
    inline iterator end() const {return iterator(e);}
    inline uint32_t numCands() const { return e-b; }
};


struct ZWalkInfo {
    uint32_t pos;
    uint32_t lineId;
    int32_t parentIdx;

    inline void set(uint32_t p, uint32_t i, int32_t x) {pos = p; lineId = i; parentIdx = x;}
};

struct ZCands {
    struct iterator {
        ZWalkInfo* x;
        explicit inline iterator(ZWalkInfo* _x) : x(_x) {}
        inline void inc() {x++;} //overloading prefix/postfix too messy
        inline uint32_t operator*() const { return x->lineId; }
        inline bool operator==(const iterator& it) const { return it.x == x; }
        inline bool operator!=(const iterator& it) const { return it.x != x; }
    };

    ZWalkInfo* b;
    ZWalkInfo* e;
    inline ZCands(ZWalkInfo* _b, ZWalkInfo* _e) : b(_b), e(_e) {}
    inline iterator begin() const {return iterator(b);}
    inline iterator end() const {return iterator(e);}
    inline uint32_t numCands() const { return e-b; }
};

#endif  // CACHE_ARRAYS_H_
