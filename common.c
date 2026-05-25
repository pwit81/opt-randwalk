#define _GNU_SOURCE
#include <stdarg.h>
#include <sched.h>
#include <sys/mman.h>
#include "common.h"

void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  exit(EXIT_FAILURE);
}

/* The state must be seeded with a nonzero value. */
static uint64_t __state;

/* This procedure gets called before main(). */
__constructor void __init_fast_random(void) {
  __state = time(NULL);
}

void fast_srandom(uint64_t seed) {
  __state = seed;
}

uint64_t fast_random(void) {
  __state ^= __state >> 12;
  __state ^= __state << 25;
  __state ^= __state >> 27;
  return __state * 2685821657736338717UL;
}

__constructor void __init_cpu(void) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(0, &set);
  if (sched_setaffinity(getpid(), sizeof(set), &set) == -1)
    die("Error: Failed to set process CPU affinity!\n");
}

void flush_cache() {
  void *tmp = malloc_page_aligned(CACHE_SIZE);
  bzero(tmp, CACHE_SIZE);
  free(tmp);
}

typedef struct timespec timespec_t;

#define SECOND 1000000000 /* in nanoseconds */

static void timespec_add(timespec_t *a, timespec_t *b, timespec_t *res) {
  if (a->tv_nsec + b->tv_nsec >= SECOND) {
    res->tv_sec = a->tv_sec + b->tv_sec + 1;
    res->tv_nsec = a->tv_nsec + b->tv_nsec - SECOND;
  } else {
    res->tv_sec = a->tv_sec + b->tv_sec;
    res->tv_nsec = a->tv_nsec + b->tv_nsec;
  }
}

static void timespec_sub(timespec_t *a, timespec_t *b, timespec_t *res) {
  if (a->tv_nsec - b->tv_nsec < 0) {
    res->tv_sec = a->tv_sec - b->tv_sec - 1;
    res->tv_nsec = a->tv_nsec - b->tv_nsec + SECOND;
  } else {
    res->tv_sec = a->tv_sec - b->tv_sec;
    res->tv_nsec = a->tv_nsec - b->tv_nsec;
  }
}

void timer_reset(_timer_t *t) {
  memset(t, 0, sizeof(_timer_t));
}

void timer_start(_timer_t *t) {
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t->start);
}

void timer_stop(_timer_t *t) {
  timespec_t ts_end, ts_res;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts_end);
  timespec_sub(&ts_end, &t->start, &ts_res);
  timespec_add(&t->sum, &ts_res, &t->sum);
}

void timer_print(_timer_t *t) {
  printf("Time elapsed: %ld.%06ld seconds\n",
         t->sum.tv_sec, t->sum.tv_nsec / 1000);
}

void read_bytes(const char *pathname, void *buf, size_t n) {
  int fd = open(pathname, 0);
  read(fd, buf, n);
  close(fd);
}

void *malloc_page_aligned(size_t size) {
  void *ptr = NULL;
  posix_memalign((void **)&ptr, getpagesize(), size);
  if (ptr == NULL)
    die("Error: Cannot allocate %ld bytes!\n", size);
  // madvise(ptr, size, MADV_HUGEPAGE);
  return ptr;
}

#if PAPI
#include <papi.h>

/*
 * We use generic perf events (perf::) wherever possible for cross-platform
 * compatibility (Intel & AMD). For events without generic equivalents (L2
 * cache misses), we try multiple alternatives at init time.
 *
 * Generic (work on all x86 with Linux perf):
 *   perf::INSTRUCTIONS          -> total instructions
 *   perf::CYCLES                -> total cycles
 *   perf::BRANCH-INSTRUCTIONS   -> branch instructions
 *   perf::BRANCH-MISSES         -> mispredicted branches
 *   perf::L1-DCACHE-LOADS       -> load instructions (denominator)
 *   perf::L1-DCACHE-STORES      -> store instructions (denominator)
 *   perf::L1-DCACHE-LOAD-MISSES -> L1 Data Cache misses
 *   perf::LLC-LOAD-MISSES       -> Last-Level Cache misses
 *   perf::DTLB-LOAD-MISSES      -> data TLB misses
 *
 * Non-generic (L2 cache misses — no perf:: equivalent):
 *   Tried in order: PAPI_L2_DCM (preset),
 *                   L2_RQSTS:DEMAND_DATA_RD_MISS (Intel native),
 *                   REQUESTS_TO_L2_GROUP:L2_HW_PF (AMD native, approximate)
 *   The first one that succeeds is used; the actual name is stored in
 *   the evset so that pmc_read() can find it.
 */

#define MAX_EVENT 10
#define MIN_HWCTRS 4

/* Sentinel names for events resolved via fallback at init time. */
#define L1_LOADS_PLACEHOLDER  "<<L1_LD>>"
#define L1_STORES_PLACEHOLDER "<<L1_SR>>"
#define L1_DCM_PLACEHOLDER    "<<L1_DCM>>"
#define L2_DCM_PLACEHOLDER    "<<L2_DCM>>"
#define L3_TCM_PLACEHOLDER    "<<L3_TCM>>"
#define DTLB_DM_PLACEHOLDER   "<<DTLB_DM>>"

static int eventset = PAPI_NULL;
static long long eventcount[MAX_EVENT];

typedef struct evset {
  const char *name;
  long idx;
} evset_t;

static evset_t empty_evset[] = {
  { NULL, -1 }
};

static evset_t ipc_evset[] = {
  { "perf::INSTRUCTIONS", -1 },
  { "perf::CYCLES", -1 },
  { NULL, - 1 }
};

static evset_t branch_evset[] = {
  { "perf::BRANCH-INSTRUCTIONS", -1 },
  { "perf::BRANCH-MISSES", -1 },
  { NULL, -1 }
};

static evset_t memory_evset[] = {
  { L1_LOADS_PLACEHOLDER, -1 },
  { L1_STORES_PLACEHOLDER, -1 },
  { L1_DCM_PLACEHOLDER, -1 },
  { L2_DCM_PLACEHOLDER, -1 },
  { NULL, -1 },
};

static evset_t l1cache_evset[] = {
  { L1_LOADS_PLACEHOLDER, -1 },
  { L1_STORES_PLACEHOLDER, -1 },
  { L1_DCM_PLACEHOLDER, -1 },
  { NULL, -1 },
};

static evset_t l2cache_evset[] = {
  { L1_LOADS_PLACEHOLDER, -1 },
  { L1_STORES_PLACEHOLDER, -1 },
  { L2_DCM_PLACEHOLDER, -1 },
  { NULL, -1 },
};

static evset_t l3cache_evset[] = {
  { L1_LOADS_PLACEHOLDER, -1 },
  { L1_STORES_PLACEHOLDER, -1 },
  { L3_TCM_PLACEHOLDER, -1 },
  { NULL, -1 },
};

static evset_t tlb_evset[] = {
  { L1_LOADS_PLACEHOLDER, -1 },
  { L1_STORES_PLACEHOLDER, -1 },
  { DTLB_DM_PLACEHOLDER, -1 },
  { NULL, -1 },
};

static evset_t *all_evset[] = {
  empty_evset, ipc_evset, branch_evset, memory_evset,
  l1cache_evset, l2cache_evset, l3cache_evset, tlb_evset
};

static evset_t *cur_evset = NULL;

/*
 * Try to add one of several alternative event names. The first one that
 * succeeds is stored in es->name so that pmc_read() can find it later.
 * Returns PAPI_OK on success, or the last error code on failure.
 */
static int pmc_try_add(evset_t *es, const char **alternatives, int nalts) {
  int retval = PAPI_ENOEVNT;
  for (int i = 0; i < nalts; i++) {
    retval = PAPI_add_named_event(eventset, alternatives[i]);
    if (retval == PAPI_OK) {
      es->name = alternatives[i];
      return PAPI_OK;
    }
  }
  return retval;
}
#endif

pmc_evset_t pmc_evset_by_name(const char *name) {
  if (strcmp(name, "ipc") == 0)
    return PMC_IPC;
  if (strcmp(name, "branch") == 0)
    return PMC_BRANCH;
  if (strcmp(name, "memory") == 0)
    return PMC_MEMORY;
  if (strcmp(name, "l1") == 0)
    return PMC_L1;
  if (strcmp(name, "l2") == 0)
    return PMC_L2;
  if (strcmp(name, "l3") == 0)
    return PMC_L3;
  if (strcmp(name, "tlb") == 0)
    return PMC_TLB;
  return PMC_NONE;
}

const char pmc_evset_string[] = "ipc|branch|memory|l1|l2|l3|tlb";

void pmc_init(pmc_evset_t evset) {
#if PAPI
  int retval;

	retval = PAPI_library_init(PAPI_VER_CURRENT);
	if (retval != PAPI_VER_CURRENT)
		die("PAPI_library_init failed: %s!\n", PAPI_strerror(retval));

  const PAPI_hw_info_t *hwinfo = PAPI_get_hardware_info();
  if (hwinfo == NULL)
    die("Could not fetch PAPI hardware information!\n");
  if (hwinfo->virtualized)
    die("Running inside virtual machine is not supported!\n");

  retval = PAPI_get_opt(PAPI_MAX_HWCTRS, NULL);
  if (retval < 0)
    die("Number of hardware counters: %s\n", PAPI_strerror(retval));
  if (retval == 0)
    die("No hardware counters available! Maybe they are disabled for user?\n"
        "Try to run 'sysctl kernel.perf_event_paranoid=-1' as root.\n");
  if (retval < MIN_HWCTRS)
    die("Configuration not supported: "
        "need at least %d hardware counters, got only %d.\n"
        "Try disabling hyper-threading in BIOS settings if possible!\n",
        MIN_HWCTRS, retval);

	retval = PAPI_create_eventset(&eventset);
	if (retval != PAPI_OK)
		die("PAPI_create_eventset failed: %s!\n", PAPI_strerror(retval));

  /* Fallback alternatives for various metrics to maximize compatibility.
   * Order: generic perf:: -> PAPI preset -> Intel native -> AMD native */
  static const char *alts_loads[] = {
    "perf::L1-DCACHE-LOADS", "PAPI_LD_INS", "MEM_INST_RETIRED:ALL_LOADS", "LS_DISPATCH:LOAD_DISPATCH", "PAPI_L1_DCA"
  };
  static const char *alts_stores[] = {
    "perf::L1-DCACHE-STORES", "PAPI_SR_INS", "MEM_INST_RETIRED:ALL_STORES", "LS_DISPATCH:STORE_DISPATCH"
  };
  static const char *alts_l1_dcm[] = {
    "perf::L1-DCACHE-LOAD-MISSES", "PAPI_L1_DCM"
  };
  static const char *alts_l2_dcm[] = {
    "PAPI_L2_DCM", "L2_RQSTS:DEMAND_DATA_RD_MISS", "REQUESTS_TO_L2_GROUP:L2_HW_PF"
  };
  static const char *alts_l3_tcm[] = {
    "perf::LLC-LOAD-MISSES", "PAPI_L3_TCM", "ix86arch::LLC_MISSES"
  };
  static const char *alts_dtlb[] = {
    "perf::DTLB-LOAD-MISSES", "PAPI_TLB_DM"
  };

  cur_evset = all_evset[evset];

  int idx = 0;
  for (evset_t *es = cur_evset; es->name; es++) {
    const char *orig_name = es->name;
    
    if (strcmp(es->name, L1_LOADS_PLACEHOLDER) == 0) {
      retval = pmc_try_add(es, alts_loads, sizeof(alts_loads)/sizeof(alts_loads[0]));
    } else if (strcmp(es->name, L1_STORES_PLACEHOLDER) == 0) {
      retval = pmc_try_add(es, alts_stores, sizeof(alts_stores)/sizeof(alts_stores[0]));
    } else if (strcmp(es->name, L1_DCM_PLACEHOLDER) == 0) {
      retval = pmc_try_add(es, alts_l1_dcm, sizeof(alts_l1_dcm)/sizeof(alts_l1_dcm[0]));
    } else if (strcmp(es->name, L2_DCM_PLACEHOLDER) == 0) {
      retval = pmc_try_add(es, alts_l2_dcm, sizeof(alts_l2_dcm)/sizeof(alts_l2_dcm[0]));
    } else if (strcmp(es->name, L3_TCM_PLACEHOLDER) == 0) {
      retval = pmc_try_add(es, alts_l3_tcm, sizeof(alts_l3_tcm)/sizeof(alts_l3_tcm[0]));
    } else if (strcmp(es->name, DTLB_DM_PLACEHOLDER) == 0) {
      retval = pmc_try_add(es, alts_dtlb, sizeof(alts_dtlb)/sizeof(alts_dtlb[0]));
    } else {
      retval = PAPI_add_named_event(eventset, es->name);
    }
    
    if (retval != PAPI_OK) {
      fprintf(stderr, "Warning: Failed to add event for %s: %s\n", orig_name, PAPI_strerror(retval));
      es->name = NULL; // Mark as failed
    } else {
      es->idx = idx++;
    }
  }

  pmc_clear();
#endif
}

void pmc_kill(void) {
#if PAPI
  PAPI_shutdown();
#endif
}

void pmc_start(void) {
#if PAPI
  PAPI_start(eventset);
#endif
}

void pmc_stop(void) {
#if PAPI
  if (cur_evset == empty_evset)
    return;
  long long count[MAX_EVENT];
  int retval = PAPI_stop(eventset, count);
  if (retval != PAPI_OK)
    die("PAPI_stop failed with %s!\n", PAPI_strerror(retval));
  for (int i = 0; i < MAX_EVENT; i++)
    eventcount[i] += count[i];
#endif
}

void pmc_clear(void) {
#if PAPI
  PAPI_reset(eventset);

  memset(eventcount, 0, sizeof(eventcount));
#endif
}

static long long pmc_read_by_index(int idx) {
#if PAPI
  if (idx >= 0 && idx < MAX_EVENT && cur_evset[idx].name != NULL) {
    return eventcount[cur_evset[idx].idx];
  }
#endif
  return 0;
}

void pmc_print(void) {
#if PAPI
  if (cur_evset == ipc_evset) {
    printf("> Total instructions: %ld\n", (long)pmc_read_by_index(0));
    printf("> Instructions per cycle: %2.3f\n",
           (double)pmc_read_by_index(0) / (double)pmc_read_by_index(1));
  } else if (cur_evset == branch_evset) {
    printf("> Branch misprediction ratio: %2.3f%%\n",
           100.0 * (double)pmc_read_by_index(1) /
           (double)pmc_read_by_index(0));
  } else if (cur_evset != empty_evset) {
    double ldst_insns = pmc_read_by_index(0) + pmc_read_by_index(1);
    
    if (ldst_insns == 0.0) {
      ldst_insns = 1.0; // avoid division by zero if loads/stores not available
    }

    if (cur_evset == memory_evset || cur_evset == l1cache_evset) {
      if (cur_evset[2].name != NULL) {
        printf("> L1 Data Cache miss ratio: %2.3f%%\n",
               100.0 * pmc_read_by_index(2) / ldst_insns);
      } else {
        printf("> L1 Data Cache miss ratio: [Not Available]\n");
      }
    }
    if (cur_evset == memory_evset || cur_evset == l2cache_evset) {
      int l2_idx = (cur_evset == memory_evset) ? 3 : 2;
      if (cur_evset[l2_idx].name != NULL) {
        printf("> L2 Data Cache miss ratio: %2.3f%%\n",
               100.0 * pmc_read_by_index(l2_idx) / ldst_insns);
      } else {
        printf("> L2 Data Cache miss ratio: [Not Available]\n");
      }
    }
    if (cur_evset == l3cache_evset) {
      if (cur_evset[2].name != NULL) {
        printf("> L3 Cache miss ratio: %2.3f%%\n",
               100.0 * pmc_read_by_index(2) / ldst_insns);
      } else {
        printf("> L3 Cache miss ratio: [Not Available]\n");
      }
    }
    if (cur_evset == tlb_evset) {
      if (cur_evset[2].name != NULL) {
        printf("> Data TLB miss ratio: %2.3f%%\n",
               100.0 * pmc_read_by_index(2) / ldst_insns);
      } else {
        printf("> Data TLB miss ratio: [Not Available]\n");
      }
    }
  }
#endif
}
