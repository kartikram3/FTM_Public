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

#ifndef FILTER_CACHE_H_
#define FILTER_CACHE_H_

#include "bithacks.h"
#include "cache.h"
#include "galloc.h"
#include "zsim.h"
#include<fstream>

/* Extends Cache with an L0 direct-mapped cache, optimized to hell for hits
 *
 * L1 lookups are dominated by several kinds of overhead (grab the cache locks,
 * several virtual functions for the replacement policy, etc.). This
 * specialization of Cache solves these issues by having a filter array that
 * holds the most recently used line in each set. Accesses check the filter array,
 * and then go through the normal access path. Because there is one line per set,
 * it is fine to do this without grabbing a lock.
 */

class FilterCache : public Cache {
    private:
        struct FilterEntry {
            volatile Address rdAddr;
            volatile Address wrAddr;
            volatile uint64_t availCycle;

            void clear() {wrAddr = 0; rdAddr = 0; availCycle = 0;}
        };

        //Replicates the most accessed line of each set in the cache
        FilterEntry* filterArray;
        Address setMask;
        uint32_t numSets;
        uint32_t srcId; //should match the core
        uint32_t reqFlags;

        lock_t filterLock;
        uint64_t fGETSHit, fGETXHit;
        Counter procTableHit;
        Counter procTableMiss;
        Counter unlabelledAccess;   
        Counter translatedAccessCount;
        Counter rxpCount;
        Counter rwpCount;
        Counter rpCount;
        Counter rwxpCount;

    public:
        FilterCache(uint32_t _numSets, uint32_t _numLines, CC* _cc, CacheArray* _array,
                ReplPolicy* _rp, uint32_t _accLat, uint32_t _invLat, g_string& _name)
            : Cache(_numLines, _cc, _array, _rp, _accLat, _invLat, _name)
        {
            numSets = _numSets;
            setMask = numSets - 1;
            filterArray = gm_memalign<FilterEntry>(CACHE_LINE_BYTES, numSets);
            for (uint32_t i = 0; i < numSets; i++) filterArray[i].clear();
            futex_init(&filterLock);
            fGETSHit = fGETXHit = 0;
            srcId = -1;
            reqFlags = 0;
        }

        void setSourceId(uint32_t id) {
            srcId = id;
        }

        void setFlags(uint32_t flags) {
            reqFlags = flags;
        }

        void initStats(AggregateStat* parentStat) {
            AggregateStat* cacheStat = new AggregateStat();
            cacheStat->init(name.c_str(), "Filter cache stats");

            ProxyStat* fgetsStat = new ProxyStat();
            fgetsStat->init("fhGETS", "Filtered GETS hits", &fGETSHit);
            ProxyStat* fgetxStat = new ProxyStat();
            fgetxStat->init("fhGETX", "Filtered GETX hits", &fGETXHit);
            procTableHit.init("pTableHit", "Proc Table Hit");
            procTableMiss.init("pTableMiss", "Proc Table Miss");
            unlabelledAccess.init("unlabelledAcc", "Unlabelled Access");
            translatedAccessCount.init("translatedAcc", "Translated Accesses");
            rxpCount.init("rxpAcc","rxp accesses");
            rwpCount.init("rwpAcc","rwp accesses");
            rpCount.init("rpAcc","rp accesses");
            rwxpCount.init("rwxpAcc","rwxp accesses");


            cacheStat->append(fgetsStat);
            cacheStat->append(fgetxStat);
            cacheStat->append(&procTableHit);
            cacheStat->append(&procTableMiss);
            cacheStat->append(&unlabelledAccess);
            cacheStat->append(&translatedAccessCount);
            cacheStat->append(&rxpCount);
            cacheStat->append(&rwpCount);
            cacheStat->append(&rpCount);
            cacheStat->append(&rwxpCount);

            initCacheStats(cacheStat);
            parentStat->append(cacheStat);
        }

        inline uint64_t load(Address vAddr, uint64_t curCycle) {
            Address vLineAddr = vAddr >> lineBits;
            uint32_t idx = vLineAddr & setMask;
            //uint64_t availCycle = filterArray[idx].availCycle; //read before, careful with ordering to avoid timing races
            //if (vLineAddr == filterArray[idx].rdAddr) {
            //    fGETSHit++;
            //    return MAX(curCycle, availCycle);
            //} else {
                return replace(vLineAddr, idx, true, curCycle);
            //}
        }

        inline uint64_t store(Address vAddr, uint64_t curCycle) {
            Address vLineAddr = vAddr >> lineBits;
            uint32_t idx = vLineAddr & setMask;
            //uint64_t availCycle = filterArray[idx].availCycle; //read before, careful with ordering to avoid timing races
            //if (vLineAddr == filterArray[idx].wrAddr) {
            //    fGETXHit++;
            //    //NOTE: Stores don't modify availCycle; we'll catch matches in the core
            //    //filterArray[idx].availCycle = curCycle; //do optimistic store-load forwarding
            //    return MAX(curCycle, availCycle);
            //} else {
                return replace(vLineAddr, idx, false, curCycle);
            //}
        }

        uint64_t checkSharedLib(uint64_t lineAddr){
           //get the offset from the per process map and get the final offset from the shared table
           uint64_t addr = lineAddr << 6;


           ProcMapInfo *process_map;
           int idx=-1;
           for (uint32_t i=0; i<zinfo->numCores; i++){
              int cur_pid=zinfo->perProcessInfo_idx[i];
              if (cur_pid == pid){ idx=i; break; }
              if (cur_pid == 0){ idx=i; panic("Process not found while checking protection");  break;};
           }
           assert (idx != -1);
           process_map=zinfo->perProcessInfo_info[idx];

           for (int ii=0; ii<1000; ii++){
             uint64_t start_range = process_map[ii].start_range;
             uint64_t end_range = process_map[ii].end_range;
             if ((start_range == 0) && (end_range == 0) ) break;
             if ( (addr >= start_range) && (addr < end_range) ){
                uint64_t name_ul = process_map[ii].name;  
                uint64_t baseline = 0;

                for (int jj=0; jj<1000; jj++){
                   if (process_map[jj].name == name_ul){
                      baseline = process_map[jj].start_range;
                      break;
                   }
                   assert_msg(jj < 1000, "Didn't find the section");
                }

                //assert(baseline != 0);
                uint64_t offset = addr-baseline;
                //if (!(name_ul == 0x3000 )){
                if (offset <= 0x100000000){
                     //std::ifstream new_file; 
                     //new_file.open("/proc/" + std::to_string(pid) + std::string("/maps")); 
                     //std::string line;
                     //while (std::getline(new_file,line)){
                     //   fprintf(stderr,"%s\n",line.c_str());
                     //}
                }
                assert_msg(offset <= 0x100000000, "The addr is %lx, baseline is %lx, start range is %lx, end range is %lx\n, name_ul is %lx, region is %d",
                                                addr, baseline, start_range, end_range, name_ul, (int)process_map[ii].result);
                //}                //assert(zinfo->sharedLibInfo->find(name_ul) != zinfo->sharedLibInfo->end());
               
                int idx_1=-1; 
                for (int i=0; i<1000; i++){ 
                   if (zinfo->sharedLibInfo_idx[i] == name_ul) {idx_1=i; break;}
                   if (zinfo->sharedLibInfo_idx[i] == (unsigned long)-1) { break;}
                }
                assert_msg(idx_1 != -1, "Accessed address %lx, name_ul is %lx", addr, name_ul );
                uint64_t new_baseline = zinfo->sharedLibInfo_info[idx_1].libAddr; 

                return (offset + new_baseline) >> 6;
             }
           }
 
           unlabelledAccess.inc();   
           return -1;
        }

        uint64_t protection(uint64_t lineAddr, int &location, uint32_t &region_type){
          uint64_t addr = lineAddr << 6;

           ProcMapInfo *process_map;
           int idx=-1;
           for (uint32_t i=0; i<zinfo->numCores; i++){
              int cur_pid=zinfo->perProcessInfo_idx[i];
              if (cur_pid == pid){ idx=i; break; }
              if (cur_pid == 0){ idx=i; panic("Process not found while checking protection");};
           }
           assert (idx != -1);
           process_map=zinfo->perProcessInfo_info[idx];

          for (int ii=0; ii<1000; ii++){
            uint64_t start_range = process_map[ii].start_range;
            uint64_t end_range = process_map[ii].end_range;
            if ((start_range == 0) && (end_range == 0) ) {
                  //info("Start and End are 0, addr is %lx", addr) ;
                  break;
                 // info ("Sleeping ...");
                 // while (true);  

            }
            //info("Searching table, target addr is %lx, start addr is %lx, end addr is %lx \n", addr,start_range, end_range );
            if ( (addr >= start_range) && (addr < end_range) ){
              //std::string y ((*it).permissions);
              procTableHit.inc();
              location = process_map[ii].result;
              uint64_t permissions = process_map[ii].permissions;

              region_type = region_type | (process_map[ii].binary);
              region_type = region_type | (process_map[ii].heap << 1);
              region_type = region_type | (process_map[ii].sl << 2 );
              region_type = region_type | (process_map[ii].mmap << 3);
              region_type = region_type | (process_map[ii].stack << 4);
              region_type = region_type | (process_map[ii].vvar << 5);
              region_type = region_type | (process_map[ii].vdso << 6);
              region_type = region_type | (process_map[ii].vsyscall << 7);
              //info("The permissions for %lx are %lx\n", addr, permissions);
              
              //if (addr == 0x7ffff7ffa000) fprintf(stderr,"The region_type is %d, name_ul is %lx, start_range is %lx ", (int)region_type, process_map[ii].name, process_map[ii].start_range);
              return permissions;
              //return ( y.compare(std::string("r-xp")) == 0 );
            }
          }

          procTableMiss.inc();
          //info("Proc table miss on %lx",addr);
          //zinfo->remakePmap = 1;

          return 10;
        }


        uint64_t replace(Address vLineAddr, uint32_t idx, bool isLoad, uint64_t curCycle) {
            #if defined GPG_ATTACK || defined PDFTOPS_ATTACK
               Address pLineAddr;
               if (vLineAddr == (0x7f73e7480000 >> 6)){
                  pLineAddr = vLineAddr; 
               } 
               else if (vLineAddr == (0x7f73e7481000 >> 6)){
                  pLineAddr = vLineAddr; 
               } else {
                  pLineAddr = procMask | vLineAddr;
               }
            #else
            Address pLineAddr = procMask | vLineAddr;

            
            uint64_t permission = 1;
            uint64_t new_pLineAddr = 0;
            int location = 0;
            uint32_t region_type = 0;
            //change this so that pLineAddr is found using a smarter strategy 
            //pLineAddr = checkSharedLib(vLineAddr); 
           
            //info ("Here doing the first access"); 
            if (zinfo->firstPhase && (!zinfo->noSharing)){
              //if (zinfo->remakePmap) futex_lock(&zinfo->global_lock);
              futex_lock(&zinfo->global_lock);
               if (zinfo->scatterCache ){
                    new_pLineAddr = pLineAddr;
               }else{
                    permission = protection(vLineAddr, location, region_type);
                     //info("The encoded location is %x",location);
                     //assert (location > 0);
                     //protection_slow(vLineAddr,p);
                     //info("Permissions is %lx and rxp permissions in %lx", permission, zinfo->perm_rxp );
                     if ((region_type & (1<<3)) || (permission == 10) ){
                       //fprintf(stderr,"The region_type is %lx\n");
                       new_pLineAddr = pLineAddr;
                     } else if ((region_type & (1<<2)) && ( (permission == zinfo->perm_rxp) ||  (permission == zinfo->perm_rp) ) ) {
                       new_pLineAddr = checkSharedLib(vLineAddr);
                       //info ("New pLineAddr %lx for pid %d", new_pLineAddr, pid);
                       //new_pLineAddr = pLineAddr;
                     }else {
                       new_pLineAddr = pLineAddr;
                     }
               }
              //if (zinfo->remakePmap) futex_unlock(&zinfo->global_lock);
              futex_unlock(&zinfo->global_lock);
              if ((new_pLineAddr != 0) && (new_pLineAddr != (uint64_t)-1)){
                 pLineAddr = new_pLineAddr;
                 translatedAccessCount.inc();
              }
            }

            #endif

            MESIState dummyState = MESIState::I;
            futex_lock(&filterLock);
            MemReq req = {pLineAddr, isLoad? GETS : GETX, 0, &dummyState, curCycle, &filterLock, dummyState, srcId, reqFlags};

            //if ((pLineAddr << 6) == 0xc10448c40 ){
            //   info ("vAddr is %lx", vLineAddr <<6); 
            //}

            /*  FTM flags for the request */
            /* **************************  */

            req.flags = req.flags | (region_type << 13);

            if (permission == zinfo->perm_rxp){
              req.flags = req.flags | (1 << 8); 
              //std::string p_str(p);
              //assert(p_str.compare(std::string("r-xp")) == 0);
              rxpCount.inc();
            }
            if (permission == zinfo->perm_rwp){
              req.flags = req.flags | (1 << 7);
              //std::string p_str(p);
              //assert_msg((p_str.compare(std::string("rw-p")) == 0) || zinfo->remakePmap, "Expected rw-p, but the permission is %s, addr is %lx", p_str.c_str(), vLineAddr << 6);
              rwpCount.inc();
            }else if (permission == zinfo->perm_rp){
              req.flags = req.flags | (1 << 11);
              rpCount.inc();
            }else if (permission == zinfo->perm_rwxp){
              req.flags = req.flags | (1 << 12);
              rwxpCount.inc();
            }
            const char *name_c = name.c_str() ;
            //int iflag = 0;
            //int dflag = 0;
            if (!zinfo->scatterCache) {
               if ( name_c[0] == 'l'  &&  name_c[1] == '1' &&  name_c[2] == 'i' ){
                 req.flags = req.flags | (1 << 9);
                 if (zinfo->firstPhase){
                   //assert_msg(permission == zinfo->perm_rxp, "VAddress is %lx, PAddress is %lx, region type is %d", vLineAddr << lineBits, pLineAddr << lineBits, region_type );
                   //info("Region is %d, address is %lx", region_type, vLineAddr << lineBits);
                   //while (true); 
                 }
                 //iflag = 1;
               }
               if ( name_c[0] == 'l'  &&  name_c[1] == '1' &&  name_c[2] == 'd' ){
                 req.flags = req.flags | (1 << 10);

                 if (zinfo->firstPhase){
                   //dflag = 1;
                   //if (permission == zinfo->perm_rxp) {
                      //info("The RXP address in the Dcache is %lx", vLineAddr << 6);
                      //while(true);
                   //}
                 }
               }
            }
            /* **************************  */
            /*  End of FTM flags for the request */


            uint64_t respCycle  = access(req);

            //Due to the way we do the locking, at this point the old address might be invalidated, but we have the new address guaranteed until we release the lock

            //Careful with this order
            Address oldAddr = filterArray[idx].rdAddr;
            filterArray[idx].wrAddr = isLoad? -1L : vLineAddr;
            filterArray[idx].rdAddr = vLineAddr;

            //For LSU simulation purposes, loads bypass stores even to the same line if there is no conflict,
            //(e.g., st to x, ld from x+8) and we implement store-load forwarding at the core.
            //So if this is a load, it always sets availCycle; if it is a store hit, it doesn't
            if (oldAddr != vLineAddr) filterArray[idx].availCycle = respCycle;

            futex_unlock(&filterLock);
            return respCycle;
        }

        uint64_t invalidate(const InvReq& req) {
            Cache::startInvalidate();  // grabs cache's downLock
            futex_lock(&filterLock);
            uint32_t idx = req.lineAddr & setMask; //works because of how virtual<->physical is done...
            if ((filterArray[idx].rdAddr | procMask) == req.lineAddr) { //FIXME: If another process calls invalidate(), procMask will not match even though we may be doing a capacity-induced invalidation!
                filterArray[idx].wrAddr = -1L;
                filterArray[idx].rdAddr = -1L;
            }
            uint64_t respCycle = Cache::finishInvalidate(req); // releases cache's downLock
            futex_unlock(&filterLock);
            return respCycle;
        }

        void contextSwitch() {
            futex_lock(&filterLock);
            for (uint32_t i = 0; i < numSets; i++) filterArray[i].clear();
            futex_unlock(&filterLock);
        }
};

#endif  // FILTER_CACHE_H_
