/*
htop - OpenBSDProcessList.c
(C) 2014 Hisham H. Muhammad
(C) 2015 Michael McConville
Released under the GNU GPL, see the COPYING file
in the source distribution for its full text.
*/

#include "ProcessList.h"
#include "OpenBSDProcessList.h"
#include "OpenBSDProcess.h"

#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <fcntl.h>
#include <string.h>
#include <sys/resource.h>

/*{

#include <kvm.h>

typedef struct CPUData_ {
   unsigned long long int totalTime;
   unsigned long long int totalPeriod;
} CPUData;

typedef struct OpenBSDProcessList_ {
   ProcessList super;
   kvm_t* kd;

   CPUData* cpus;

} OpenBSDProcessList;

}*/

static int pageSizeKb;
static long fscale;

ProcessList* ProcessList_new(UsersTable* usersTable, Hashtable* pidWhiteList, uid_t userId) {
   int mib[] = { CTL_HW, HW_NCPU };
   int fmib[] = { CTL_KERN, KERN_FSCALE };
   int i;
   OpenBSDProcessList* fpl = calloc(1, sizeof(OpenBSDProcessList));
   ProcessList* pl = (ProcessList*) fpl;
   size_t size = sizeof(pl->cpuCount);

   ProcessList_init(pl, Class(OpenBSDProcess), usersTable, pidWhiteList, userId);
   pl->cpuCount = 1;    // default to 1 on sysctl() error
   (void)sysctl(mib, 2, &pl->cpuCount, &size, NULL, 0);
   fpl->cpus = realloc(fpl->cpus, pl->cpuCount * sizeof(CPUData));

   size = sizeof(fscale);
   if (sysctl(fmib, 2, &fscale, &size, NULL, 0) < 0)
     CRT_fatalError("fscale sysctl call failed");

   for (i = 0; i < pl->cpuCount; i++) {
      fpl->cpus[i].totalTime = 1;
      fpl->cpus[i].totalPeriod = 1;
   }

   pageSizeKb = PAGE_SIZE_KB;

   // XXX: last arg should eventually be an errbuf
   fpl->kd = kvm_open(NULL, NULL, NULL, KVM_NO_FILES, NULL);
   assert(fpl->kd);

   return pl;
}

void ProcessList_delete(ProcessList* this) {
   const OpenBSDProcessList* fpl = (OpenBSDProcessList*) this;
   if (fpl->kd) kvm_close(fpl->kd);

   ProcessList_done(this);
   free(this);
}

static inline void OpenBSDProcessList_scanMemoryInfo(ProcessList* pl) {
   static int uvmexp_mib[] = {CTL_VM, VM_UVMEXP};
   struct uvmexp uvmexp;
   size_t size = sizeof(uvmexp);

   if (sysctl(uvmexp_mib, 2, &uvmexp, &size, NULL, 0) < 0) {
      CRT_fatalError("uvmexp sysctl call failed");
   }

   //kb_pagesize = uvmexp.pagesize / 1024;
   pl->usedMem = uvmexp.active * pageSizeKb;
   pl->totalMem = uvmexp.npages * pageSizeKb;

   /*
   const OpenBSDProcessList* fpl = (OpenBSDProcessList*) pl;

   size_t len = sizeof(pl->totalMem);
   sysctl(MIB_hw_physmem, 2, &(pl->totalMem), &len, NULL, 0);
   pl->totalMem /= 1024;
   sysctl(MIB_vm_stats_vm_v_wire_count, 4, &(pl->usedMem), &len, NULL, 0);
   pl->usedMem *= pageSizeKb;
   pl->freeMem = pl->totalMem - pl->usedMem;
   sysctl(MIB_vm_stats_vm_v_cache_count, 4, &(pl->cachedMem), &len, NULL, 0);
   pl->cachedMem *= pageSizeKb;

   struct kvm_swap swap[16];
   int nswap = kvm_getswapinfo(fpl->kd, swap, sizeof(swap)/sizeof(swap[0]), 0);
   pl->totalSwap = 0;
   pl->usedSwap = 0;
   for (int i = 0; i < nswap; i++) {
      pl->totalSwap += swap[i].ksw_total;
      pl->usedSwap += swap[i].ksw_used;
   }
   pl->totalSwap *= pageSizeKb;
   pl->usedSwap *= pageSizeKb;

   pl->sharedMem = 0;  // currently unused
   pl->buffersMem = 0; // not exposed to userspace
   */
}

char *OpenBSDProcessList_readProcessName(kvm_t* kd, struct kinfo_proc* kproc, int* basenameEnd) {
   char *str, *buf, **argv;
   size_t cpsz;
   size_t len = 500;

   argv = kvm_getargv(kd, kproc, 500);

   if (argv == NULL)
      CRT_fatalError("kvm call failed");

   str = buf = malloc(len+1);
   if (str == NULL)
      CRT_fatalError("out of memory");

   while (*argv != NULL) {
      cpsz = MIN(len, strlen(*argv));
      strncpy(buf, *argv, cpsz);
     buf += cpsz;
     len -= cpsz;
     argv++;
     if (len > 0) {
        *buf = ' ';
        buf++;
        len--;
     }
   }

   *buf = '\0';
   return str;
}

/*
 * Taken from OpenBSD's ps(1).
 */
double getpcpu(const struct kinfo_proc *kp) {
   if (fscale == 0)
      return (0.0);

#define   fxtofl(fixpt)   ((double)(fixpt) / fscale)

   return (100.0 * fxtofl(kp->p_pctcpu));
}

void ProcessList_goThroughEntries(ProcessList* this) {
   OpenBSDProcessList* fpl = (OpenBSDProcessList*) this;
   Settings* settings = this->settings;
   bool hideKernelThreads = settings->hideKernelThreads;
   bool hideUserlandThreads = settings->hideUserlandThreads;
   struct kinfo_proc* kproc;
   bool preExisting;
   Process* proc;
   OpenBSDProcess* fp;
   int count = 0;
   int i;

   OpenBSDProcessList_scanMemoryInfo(this);

   // use KERN_PROC_KTHREAD to also include kernel threads
   struct kinfo_proc* kprocs = kvm_getprocs(fpl->kd, KERN_PROC_ALL, 0, sizeof(struct kinfo_proc), &count);
   //struct kinfo_proc* kprocs = getprocs(KERN_PROC_ALL, 0, &count);

   for (i = 0; i < count; i++) {
      kproc = &kprocs[i];

      preExisting = false;
      proc = ProcessList_getProcess(this, kproc->p_pid, &preExisting, (Process_New) OpenBSDProcess_new);
      fp = (OpenBSDProcess*) proc;

      proc->show = ! ((hideKernelThreads && Process_isKernelThread(proc))
                  || (hideUserlandThreads && Process_isUserlandThread(proc)));

      if (!preExisting) {
         proc->ppid = kproc->p_ppid;
         proc->tpgid = kproc->p_tpgid;
         proc->tgid = kproc->p_pid;
         proc->session = kproc->p_sid;
         proc->tty_nr = kproc->p_tdev;
         proc->pgrp = kproc->p__pgid;
         proc->st_uid = kproc->p_uid;
         proc->starttime_ctime = kproc->p_ustart_sec;
         proc->user = UsersTable_getRef(this->usersTable, proc->st_uid);
         ProcessList_add((ProcessList*)this, proc);
         proc->comm = OpenBSDProcessList_readProcessName(fpl->kd, kproc, &proc->basenameOffset);
      } else {
         if (settings->updateProcessNames) {
            free(proc->comm);
            proc->comm = OpenBSDProcessList_readProcessName(fpl->kd, kproc, &proc->basenameOffset);
         }
      }

      proc->m_size = kproc->p_vm_dsize;
      proc->m_resident = kproc->p_vm_rssize;
      proc->percent_mem = (proc->m_resident * PAGE_SIZE_KB) / (double)(this->totalMem) * 100.0;
      proc->percent_cpu = MAX(MIN(getpcpu(kproc), this->cpuCount*100.0), 0.0);
      //proc->nlwp = kproc->p_numthreads;
      //proc->time = kproc->p_rtime_sec + ((kproc->p_rtime_usec + 500000) / 10);
      proc->nice = kproc->p_nice - 20;
      proc->time = kproc->p_rtime_sec + ((kproc->p_rtime_usec + 500000) / 1000000);
      proc->time *= 100;
      proc->priority = kproc->p_priority - PZERO;

      switch (kproc->p_stat) {
         case SIDL:    proc->state = 'I'; break;
         case SRUN:    proc->state = 'R'; break;
         case SSLEEP:  proc->state = 'S'; break;
         case SSTOP:   proc->state = 'T'; break;
         case SZOMB:   proc->state = 'Z'; break;
         case SDEAD:   proc->state = 'D'; break;
         case SONPROC: proc->state = 'P'; break;
         default:      proc->state = '?';
      }

      if (Process_isKernelThread(proc)) {
         this->kernelThreads++;
      }

      this->totalTasks++;
      // SRUN ('R') means runnable, not running
      if (proc->state == 'P') {
         this->runningTasks++;
      }
      proc->updated = true;
   }
}
