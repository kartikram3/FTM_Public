/** $glic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 * Copyright (C) 2011 Google Inc.
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

#include "cpuenum.h"
#include "log.h"
#include "virt/common.h"
#include <sys/mman.h>
void populateProcessMaps();
void addSharedLib();

// SYS_getcpu

// Call without CPU from vdso, with CPU from syscall version
void VirtGetcpu(uint32_t tid, uint32_t cpu, ADDRINT arg0, ADDRINT arg1) {
    unsigned resCpu;
    unsigned resNode = 0;
    if (!arg0) {
        info("getcpu() called with null cpu arg");
    }
    if (!safeCopy((unsigned*)arg0, &resCpu)) {
        info("getcpu() called with invalid cpu arg");
        return;
    }
    if (arg1 && !safeCopy((unsigned*)arg1, &resNode)) {
        info("getcpu() called with invalid node arg");
        return;
    }

    trace(TimeVirt, "Patching getcpu()");
    trace(TimeVirt, "Orig cpu %d, node %d, patching core %d / node 0", resCpu, resNode, cpu);
    resCpu = cpu;
    resNode = 0;

    safeCopy(&resCpu, (unsigned*)arg0);
    if (arg1) safeCopy(&resNode, (unsigned*)arg1);
}

PostPatchFn PatchGetcpu(PrePatchArgs args) {
    uint32_t cpu = cpuenumCpu(procIdx, getCid(args.tid));  // still valid, may become invalid when we leave()
    assert(cpu != (uint32_t)-1);
    return [cpu](PostPatchArgs args) {
        trace(TimeVirt, "[%d] Post-patching SYS_getcpu", tid);
        ADDRINT arg0 = PIN_GetSyscallArgument(args.ctxt, args.std, 0);
        ADDRINT arg1 = PIN_GetSyscallArgument(args.ctxt, args.std, 1);
        VirtGetcpu(args.tid, cpu, arg0, arg1);
        return PPA_NOTHING;
    };
}

// Scheduler affinity

PostPatchFn PatchSchedGetaffinity(PrePatchArgs args) {
    return [](PostPatchArgs args) {
        uint32_t size = PIN_GetSyscallArgument(args.ctxt, args.std, 1);
        cpu_set_t* set = (cpu_set_t*)PIN_GetSyscallArgument(args.ctxt, args.std, 2);
        if (set) { //TODO: use SafeCopy, this can still segfault
            CPU_ZERO_S(size, set);
            std::vector<bool> cpumask = cpuenumMask(procIdx);
            for (uint32_t i = 0; i < MIN(cpumask.size(), size*8 /*size is in bytes, supports 1 cpu/bit*/); i++) {
                if (cpumask[i]) CPU_SET_S(i, (size_t)size, set);
            }
        }
        info("[%d] Post-patching SYS_sched_getaffinity size %d cpuset %p", args.tid, size, set);
        return PPA_NOTHING;
    };
}

PostPatchFn PatchSchedSetaffinity(PrePatchArgs args) {
    PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT) SYS_getpid);  // squash
    return [](PostPatchArgs args) {
        PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT)-EPERM);  // make it a proper failure
        return PPA_NOTHING;
    };
}

//FTM system calls

//mmap function call
PostPatchFn PatchMmap(PrePatchArgs args) {
  return [](PostPatchArgs args) { 
    //uint64_t address = (uint64_t)PIN_GetSyscallArgument(args.ctxt, args.std, 0);
    //uint64_t length = (uint64_t)PIN_GetSyscallArgument(args.ctxt, args.std, 1);
    zinfo->flag = 1;
    zinfo->remakePmap = 1;
    //info("Detected an mmap at address %lx, with length %lu\n", address, length);
    futex_lock(&zinfo->global_lock);
    populateProcessMaps();
    addSharedLib();
    futex_unlock(&zinfo->global_lock);
    return PPA_NOTHING; 
  };
}


//mprotect function call
PostPatchFn PatchMprotect(PrePatchArgs args) {
  return [](PostPatchArgs args) { 
    //uint64_t address = (uint64_t)PIN_GetSyscallArgument(args.ctxt, args.std, 0);
    //uint64_t length = (uint64_t)PIN_GetSyscallArgument(args.ctxt, args.std, 1);
    int prot = (int)PIN_GetSyscallArgument(args.ctxt, args.std, 2);
    zinfo->flag = 1;
    zinfo->remakePmap = 1;
    //update it immediately
    if (zinfo->firstPhase){
      //uint64_t name_ul;
      std::string perm_str("");
      if ( prot & PROT_READ ) perm_str = perm_str + "r"; else perm_str = perm_str + "-";
      if ( prot & PROT_WRITE ) perm_str = perm_str + "w"; else perm_str = perm_str + "-";
      if ( prot & PROT_EXEC ) perm_str = perm_str + "x"; else perm_str = perm_str + "-";
      perm_str = perm_str + "p";
      //uint64_t perm_ul = create_ul(perm_str.c_str());
      //fprintf(stderr, "The permissions changed to %s for address %lx and size %lx, read is %d, write is %d, exec is %d\n",
      //                 perm_str.c_str(), address, length, prot & PROT_READ, prot & PROT_WRITE, prot & PROT_EXEC); 

      futex_lock(&zinfo->global_lock);
      populateProcessMaps();
      addSharedLib();
      futex_unlock(&zinfo->global_lock);
      //updateProcessMap(pid,address,length,perm_ul); 
    }

    //info("Detected an mprotect at address %lx, with length %lx\n", address, length);
    return PPA_NOTHING; 
  };
}

//munmap function call
PostPatchFn PatchMunmap(PrePatchArgs args) {
  return [](PostPatchArgs args) { 
    //uint64_t address = (uint64_t)PIN_GetSyscallArgument(args.ctxt, args.std, 0);
    //uint64_t length = (uint64_t)PIN_GetSyscallArgument(args.ctxt, args.std, 1);
    zinfo->flag = 1;
    zinfo->remakePmap = 1;
    //info("Detected an munmap at address %lx, with length %lu\n", address, length);
    return PPA_NOTHING; 
  };
}
