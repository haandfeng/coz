/*
 * Copyright (c) 2015, Charlie Curtsinger and Emery Berger,
 *                     University of Massachusetts Amherst
 * This file is part of the Coz project. See LICENSE.md file at the top-level
 * directory of this distribution and at http://github.com/plasma-umass/coz.
 */

#if !defined(CAUSAL_RUNTIME_THREAD_STATE_H)
#define CAUSAL_RUNTIME_THREAD_STATE_H

#include <atomic>
#include <fstream>

#include "ccutil/timer.h"
#include "perf.h"

#ifndef __APPLE__
#include "hit_callchains.h"
#endif

class thread_state {
public:
  bool in_use = false;      //< Set by the main thread to prevent signal handler from racing
  std::atomic<size_t> local_delay{0};   //< The count of delays (or selected line visits) in the thread
  perf_event sampler;       //< The sampler object for this thread
  timer process_timer;      //< The timer that triggers sample processing for this thread
  size_t pre_block_time;    //< The time saved before (possibly) blocking
  std::atomic<bool> is_blocked{false};  //< True between pre_block() and post_block(); skip delays

  size_t delayed_local_delay = 0;
  size_t based_local_delay = 0;
  size_t pre_local_time = 0;
  std::atomic<int> process_samples{0};
  uint64_t ex_count = 1;
  uint64_t last_sample_time = 0;
  bool in_wait = false;
  std::atomic<bool> sync_local_with_global{false};
  bool enable_print_log = false;
  std::ofstream fout;

#ifndef __APPLE__
  hit_callchains::Table _hit_callchain_table;
#endif
  
  inline void set_in_use(bool value) {
    in_use = value;
    std::atomic_signal_fence(std::memory_order_seq_cst);
  }
  
  bool check_in_use() {
    return in_use;
  }
};

#endif
