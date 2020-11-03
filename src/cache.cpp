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

#include "cache.h"
#include "hash.h"

#include "event_recorder.h"
#include "timing_event.h"
#include "zsim.h"

Cache::Cache(uint32_t _numLines, CC* _cc, CacheArray* _array, ReplPolicy* _rp, uint32_t _accLat, uint32_t _invLat, const g_string& _name
               )
    : cc(_cc), array(_array), rp(_rp), numLines(_numLines), accLat(_accLat), invLat(_invLat), name(_name) {
     cur_set = 0;
     num_sets = array->getNumSets();
      ;}

const char* Cache::getName() {
    return name.c_str();
}

void Cache::setParents(uint32_t childId, const g_vector<MemObject*>& parents, Network* network) {
    cc->setParents(childId, parents, network);
}

void Cache::setChildren(const g_vector<BaseCache*>& children, Network* network) {
    cc->setChildren(children, network);
}

void Cache::initStats(AggregateStat* parentStat) {
    AggregateStat* cacheStat = new AggregateStat();
    cacheStat->init(name.c_str(), "Cache stats");
    initCacheStats(cacheStat);
    parentStat->append(cacheStat);
}

void Cache::initCacheStats(AggregateStat* cacheStat) {
    cc->initStats(cacheStat);
    array->initStats(cacheStat);
    rp->initStats(cacheStat);
}

//Check the skew and find out whether cache line is in there
int Cache::checkSkew(MemReq& req, int *raceDetected) {
   int lineId = array->lookup(req.lineAddr,&req,false);
   return lineId;
}

uint64_t Cache::accessSkew(MemReq& req) {
    /* *** FTM variables */
    bool sameOwner = true;
    bool hit = true;
    /* *** End of FTM variables */

    uint64_t respCycle = req.cycle;
    bool skipAccess = false;
    skipAccess = cc->startSkewAccess(req); //may need to skip access due to races (NOTE: may change req.type!)
    if (likely(!skipAccess)) {
        bool updateReplacement = (req.type == GETS) || (req.type == GETX);
        int32_t lineId = array->lookup(req.lineAddr, &req, updateReplacement);
        respCycle += accLat;

        if (lineId == -1 && cc->shouldAllocate(req)) {
            hit = false;
            //Make space for new line
            Address wbLineAddr;
            lineId = array->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
            trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);

            //Evictions are not in the critical path in any sane implementation -- we do not include their delays
            //NOTE: We might be "evicting" an invalid line for all we know. Coherence controllers will know what to do
            cc->processEviction(req, wbLineAddr, lineId, respCycle); //1. if needed, send invalidates/downgrades to lower level

            //start evicting all the cache lines in the set
            //increment the current set
            //if (isLLC){
            //   if (cur_set >= 8192){
            //       //cur_set = 0;
            //   }else {
            //       for (int i=0; i<16; i++){
            //          cc->processRefresh(cur_set);
            //       }
            //   }
            //}
            //cur_set++;

            array->postinsert(req.lineAddr, &req, lineId); //do the actual insertion. NOTE: Now we must split insert into a 2-phase thing because cc unlocks us.
        }

        /* ** FTM set variables */
        if ( isLLC && hit && ((req.type == GETS) || (req.type == GETX)) ) {
           sameOwner = cc->checkSameOwner(req.lineAddr, lineId, req.srcId); 
        }else{
           //do nothing
        }
        /* ** End of FTM set variables */


        // Enforce single-record invariant: Writeback access may have a timing
        // record. If so, read it.
        EventRecorder* evRec = zinfo->eventRecorders[req.srcId];
        TimingRecord wbAcc;
        wbAcc.clear();
        if (unlikely(evRec && evRec->hasRecord())) {
            wbAcc = evRec->popRecord();
        }

        respCycle = cc->processAccess(req, lineId, respCycle);

        // Access may have generated another timing record. If *both* access
        // and wb have records, stitch them together
        if (unlikely(wbAcc.isValid())) {
            if (!evRec->hasRecord()) {
                // Downstream should not care about endEvent for PUTs
                wbAcc.endEvent = nullptr;
                evRec->pushRecord(wbAcc);
            } else {
                // Connect both events
                TimingRecord acc = evRec->popRecord();
                assert(wbAcc.reqCycle >= req.cycle);
                assert(acc.reqCycle >= req.cycle);
                DelayEvent* startEv = new (evRec) DelayEvent(0);
                DelayEvent* dWbEv = new (evRec) DelayEvent(wbAcc.reqCycle - req.cycle);
                DelayEvent* dAccEv = new (evRec) DelayEvent(acc.reqCycle - req.cycle);
                startEv->setMinStartCycle(req.cycle);
                dWbEv->setMinStartCycle(req.cycle);
                dAccEv->setMinStartCycle(req.cycle);
                startEv->addChild(dWbEv, evRec)->addChild(wbAcc.startEvent, evRec);
                startEv->addChild(dAccEv, evRec)->addChild(acc.startEvent, evRec);

                acc.reqCycle = req.cycle;
                acc.startEvent = startEv;
                // endEvent / endCycle stay the same; wbAcc's endEvent not connected
                evRec->pushRecord(acc);
            }
        }
    }

    cc->endSkewAccess(req);

    assert_msg(respCycle >= req.cycle, "[%s] resp < req? 0x%lx type %s childState %s, respCycle %ld reqCycle %ld",
            name.c_str(), req.lineAddr, AccessTypeName(req.type), MESIStateName(*req.state), respCycle, req.cycle);

    /*** FTM changes **/
    //Get the changes in the FTM status

    //if (isLLC){
    //   info ("The ftmEnable is %d, the scattercache is %d", zinfo->ftmEnable, zinfo->scatterCache);
    //}
    if (zinfo->ftmEnable && isLLC){ //&& (!(zinfo->scatterCache))){
      if ( req.type == GETS ){
        //info ("Here");
        if (zinfo->ftmTypeFlag & 0x1 ){
           if (!sameOwner){
             updateFTMStats(req);
             if ( (req.flags & (1 << 11)) || (req.flags & (1<<11)) ) {
                 return respCycle + 200;
             }else{
                 return respCycle;
             }
           }else{
             return respCycle;
           }
        }
      } else if (req.type == GETX) { 

        if (zinfo->ftmTypeFlag & 0x2 ){
           if (!sameOwner){
             updateFTMStats(req);
             if ( (req.flags & (1 << 11)) || (req.flags & (1<<11)) ) {
                 return respCycle + 200;
             }else{
                 return respCycle;
             }
           }else{
             return respCycle;
           }
        }

      } else {
         return respCycle;
      }

    }

    return respCycle;

}

void Cache::updateFTMStats(MemReq& req){

   //info("Updating FTM Stats");

   cc->incrementFirstTimeMiss(); 

   if ( req.flags & (1 << 9)) {
      cc->incrementIcacheFirstTimeMiss();
   }
   if ( req.flags & (1 << 10)) {
      cc->incrementDcacheFirstTimeMiss();
   }
   if ( req.flags & (1 << 8)) {
      cc->incrementRXPFirstTimeMiss();
   }
   if ( req.flags & (1 << 11)) {
      cc->incrementRPFirstTimeMiss();
   }
   if ( req.flags & (1 << 7)) {
      cc->incrementRWPFirstTimeMiss();
     // info ("FTM misses on RWP %lx\n", req.lineAddr << 6);
     // while (true);
   }
   if ( req.flags & (1 << 12)) {
      cc->incrementRWXPFirstTimeMiss();
   }
   if ( req.flags & (1 << 13)) {
      cc->incrementBinaryFirstTimeMiss();
   }
   if ( req.flags & (1 << 14)) {
      cc->incrementHeapFirstTimeMiss();
   }
   if ( req.flags & (1 << 15)) {
      cc->incrementSLFirstTimeMiss();
   }
   if ( req.flags & (1 << 16)) {
      cc->incrementMMAPFirstTimeMiss();
   }
   if ( req.flags & (1 << 17)) {
      cc->incrementSTACKFirstTimeMiss();
   }
   if ( req.flags & (1 << 18)) {
      cc->incrementVVARFirstTimeMiss();
   }
   if ( req.flags & (1 << 19)) {
      cc->incrementVDSOFirstTimeMiss();
   }
   if ( req.flags & (1 << 20)) {
      cc->incrementVSYSCALLFirstTimeMiss();
   }

   return;
}

uint64_t Cache::access(MemReq& req) {
    uint64_t respCycle = req.cycle;
    bool skipAccess = cc->startAccess(req); //may need to skip access due to races (NOTE: may change req.type!)
    if (likely(!skipAccess)) {
        bool updateReplacement = (req.type == GETS) || (req.type == GETX);
        int32_t lineId = array->lookup(req.lineAddr, &req, updateReplacement);
        respCycle += accLat;

        if (lineId == -1 && cc->shouldAllocate(req)) {
            //Make space for new line
            Address wbLineAddr;
            lineId = array->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
            trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);

            //Evictions are not in the critical path in any sane implementation -- we do not include their delays
            //NOTE: We might be "evicting" an invalid line for all we know. Coherence controllers will know what to do
            cc->processEviction(req, wbLineAddr, lineId, respCycle); //1. if needed, send invalidates/downgrades to lower level

            //start evicting all the cache lines in the set
            //increment the current set
            //if (isLLC){
            //   if (cur_set >= 8192){
            //       //cur_set = 0;
            //   }else {
            //       for (int i=0; i<16; i++){
            //          cc->processRefresh(cur_set);
            //       }
            //   }
            //}
            //cur_set++;

            array->postinsert(req.lineAddr, &req, lineId); //do the actual insertion. NOTE: Now we must split insert into a 2-phase thing because cc unlocks us.
        }
        // Enforce single-record invariant: Writeback access may have a timing
        // record. If so, read it.
        EventRecorder* evRec = zinfo->eventRecorders[req.srcId];
        TimingRecord wbAcc;
        wbAcc.clear();
        if (unlikely(evRec && evRec->hasRecord())) {
            wbAcc = evRec->popRecord();
        }

        respCycle = cc->processAccess(req, lineId, respCycle);

        // Access may have generated another timing record. If *both* access
        // and wb have records, stitch them together
        if (unlikely(wbAcc.isValid())) {
            if (!evRec->hasRecord()) {
                // Downstream should not care about endEvent for PUTs
                wbAcc.endEvent = nullptr;
                evRec->pushRecord(wbAcc);
            } else {
                // Connect both events
                TimingRecord acc = evRec->popRecord();
                assert(wbAcc.reqCycle >= req.cycle);
                assert(acc.reqCycle >= req.cycle);
                DelayEvent* startEv = new (evRec) DelayEvent(0);
                DelayEvent* dWbEv = new (evRec) DelayEvent(wbAcc.reqCycle - req.cycle);
                DelayEvent* dAccEv = new (evRec) DelayEvent(acc.reqCycle - req.cycle);
                startEv->setMinStartCycle(req.cycle);
                dWbEv->setMinStartCycle(req.cycle);
                dAccEv->setMinStartCycle(req.cycle);
                startEv->addChild(dWbEv, evRec)->addChild(wbAcc.startEvent, evRec);
                startEv->addChild(dAccEv, evRec)->addChild(acc.startEvent, evRec);

                acc.reqCycle = req.cycle;
                acc.startEvent = startEv;
                // endEvent / endCycle stay the same; wbAcc's endEvent not connected
                evRec->pushRecord(acc);
            }
        }
    }

    //add the remaining part of the information which gives the correct solution


    cc->endAccess(req);

    assert_msg(respCycle >= req.cycle, "[%s] resp < req? 0x%lx type %s childState %s, respCycle %ld reqCycle %ld",
            name.c_str(), req.lineAddr, AccessTypeName(req.type), MESIStateName(*req.state), respCycle, req.cycle);
    return respCycle;
}

void Cache::startInvalidate() {
    cc->startInv(); //note we don't grab tcc; tcc serializes multiple up accesses, down accesses don't see it
}

uint64_t Cache::finishInvalidate(const InvReq& req) {
    int32_t lineId = array->lookup(req.lineAddr, nullptr, false);
    assert_msg(lineId != -1, "[%s] Invalidate on non-existing address 0x%lx type %s lineId %d, reqWriteback %d", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback);
    uint64_t respCycle = req.cycle + invLat;
    trace(Cache, "[%s] Invalidate start 0x%lx type %s lineId %d, reqWriteback %d", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback);
    respCycle = cc->processInv(req, lineId, respCycle); //send invalidates or downgrades to children, and adjust our own state
    trace(Cache, "[%s] Invalidate end 0x%lx type %s lineId %d, reqWriteback %d, latency %ld", name.c_str(), req.lineAddr, InvTypeName(req.type), lineId, *req.writeback, respCycle - req.cycle);

    return respCycle;
}

bool Cache::finishRefresh(Address lineAddr, uint32_t lineId){
   int32_t lineId_new = array->lookup(lineAddr, nullptr, false);
   assert(lineId_new != -1);
   cc->processRefr(lineAddr, lineId_new);
   return true;
}
