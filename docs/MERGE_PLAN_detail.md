# BCOZ → COZ Merge Migration Plan

## Goals

Migrate the core bperf-based functionality from BCOZ (`bcoz/`) into the new COZ (`coz/`), including:
1. **Blocked Samples**: perf extensions for on/off-CPU event identification
2. **Off-CPU Subclass Classification**: I/O blocking, lock waiting, CPU scheduling, other blocking
3. **Virtual Speedup Extension**: `--blocked-scope` subclass-level virtual speedup
4. **Relational Analysis**: `--based-blocked` off-CPU event correlation profiling
5. **Blocking Logic Bug Fix**: race condition fix for `pre_block`/`post_block`

All features unique to new COZ (DWARF5, callchain, warmup_delay, viewer, etc.) are **fully preserved** and unaffected.

---

## Migration Scope Overview

| File | Action | Complexity |
|------|--------|------------|
| `libcoz/perf.h` | Add off-CPU flags + method declarations | Low |
| `libcoz/perf.cpp` | Add `get_weight()` + `locate_field` weight support | Low |
| `libcoz/thread_state.h` | Add new fields, preserve COZ original fields | Medium |
| `libcoz/profiler.h` | Add new atomic counters, fields, method declarations | Medium |
| `libcoz/profiler.cpp` | Add `process_blocked_samples()`, modify multiple methods | High |
| `libcoz/libcoz.cpp` | Add environment variable parsing, update `startup()` call | Medium |
| `include/coz_block.h` | New file (copied and adapted from bcoz) | Low |
| `coz` (Python script) | Add new CLI arguments | Low |

---

## Step 1: `libcoz/perf.h` — Add Off-CPU Event Support

**File path**: `coz/libcoz/perf.h`

### 1.1 Add Off-CPU Subclass `#define` Flags

After the `#include` block at the top of the file, add the following macro definitions (corresponding to bcoz `perf.h` lines 26–29):

```cpp
// Off-CPU event subclass flags (from blocked samples kernel patch)
#define PERF_RECORD_MISC_LOCKWAIT   (64 << 0)   // L: lock-waiting
#define PERF_RECORD_MISC_SCHED      (32 << 0)   // S: CPU scheduling
#define PERF_RECORD_MISC_IOWAIT     (16 << 0)   // I: blocking I/O
#define PERF_RECORD_MISC_BLOCKED    (8  << 0)   // B: other blocking (sleep, etc.)
```

**Insertion point**: After the `#pragma once` / `#include` block, before `class perf_event`.

### 1.2 Add `weight` to the `sample` enum

> **Note**: New COZ `perf.h` line 78 already has `time = PERF_SAMPLE_TIME` — **only `weight` needs to be added**.

Locate the `sample` enum in `coz/libcoz/perf.h` (lines 75–113). Add before `_end = PERF_SAMPLE_MAX`:

```cpp
#if defined(PERF_SAMPLE_WEIGHT)
    weight = PERF_SAMPLE_WEIGHT,   // NEW: blocked sample weight
#else
    weight = 0,
#endif
```

**Insertion point**: Before `_end = PERF_SAMPLE_MAX` at line 112, consistent with the format of other conditionally compiled fields.

### 1.3 Add Method Declarations to `perf_event::record`

Locate the `record` inner class (around bcoz `perf.h` lines 148–175) and add:

```cpp
// Off-CPU classification methods (require blocked samples kernel)
inline bool is_lock()        const { return (_header->misc & PERF_RECORD_MISC_LOCKWAIT) != 0; }
inline bool is_sched()       const { return (_header->misc & PERF_RECORD_MISC_SCHED)    != 0; }
inline bool is_io()          const { return (_header->misc & PERF_RECORD_MISC_IOWAIT)   != 0; }
inline bool is_blocked()     const { return (_header->misc & PERF_RECORD_MISC_BLOCKED)  != 0; }
inline bool is_blocked_any() const {
  return (_header->misc & (PERF_RECORD_MISC_BLOCKED |
                           PERF_RECORD_MISC_IOWAIT   |
                           PERF_RECORD_MISC_SCHED    |
                           PERF_RECORD_MISC_LOCKWAIT)) != 0;
}

// Weighted sample (how many sampling periods this blocked event spans)
uint64_t get_weight() const;

// NOTE: get_time() already exists in new COZ perf.h line 166 — do NOT re-add
```

**Insertion point**: After the existing `bool is_sample()` method in the `record` class (around line 160–161).

---

## Step 2: `libcoz/perf.cpp` — Implement Weight Field Extraction

**File path**: `coz/libcoz/perf.cpp` / `coz/libcoz/perf.h`

> **Note**: New COZ `perf.h` and `perf.cpp` already fully support `time`:
> - `perf.h` line 78: `time = PERF_SAMPLE_TIME` ✓
> - `perf.h` line 166: `get_time()` declaration ✓
> - `perf.cpp` line 277: `get_time()` implementation ✓
> - `perf.cpp` line 319: `time` handling in `locate_field` ✓
>
> **Only `weight` support needs to be added** — skip all `time`-related content.

### 2.1 Add Weight Support to `locate_field` Template

Locate the `locate_field` function in `perf.cpp` (after the `stack` block at lines 415–419). Insert before the `/** end **/` block:

```cpp
  /** stack **/
  if(s == sample::stack)
    return reinterpret_cast<T>(p);
  if(_source.is_sampling(sample::stack))
    FATAL << "Stack sampling is not supported";

  // --- NEW: weight (PERF_SAMPLE_WEIGHT = 1<<14, comes after stack in kernel record format) ---
  /** weight **/
  if(s == sample::weight)
    return reinterpret_cast<T>(p);
  if(_source.is_sampling(sample::weight))
    p += sizeof(uint64_t);
  // --- END NEW ---

  /** end **/
  if(s == sample::_end)
    return reinterpret_cast<T>(p);
```

**Insertion point**: After line 419 (`if(_source.is_sampling(sample::stack)) FATAL ...`), before `/** end **/`.

The `p` pointer in `locate_field()` is the **current field cursor** walking through the sample record:
- At the start, `p` points to the first sample field immediately following `perf_event_header`
- For each field, logic follows two steps:
  - If `s == current field`, return `p` directly
  - Otherwise, if this field is present in the sample, advance `p` past it and continue

Therefore:
```cpp
if(_source.is_sampling(sample::weight))
  p += sizeof(uint64_t);
```
means: **if this record contains a `weight` field but we are not currently looking for it, skip over these 8 bytes and continue**.

> **Minor difference vs. BCOZ source**: `bcoz/libcoz/perf.cpp` uses `FATAL << "Weight sampling is not supported"` here because in BCOZ, `weight` is the last supported sample field, with no need to skip past it to find another. Using `p += sizeof(uint64_t)` as documented here is more general, consistent with the "skip if not target" pattern used before it for `ip/tid/time/.../stack`, and makes the merged COZ a more extensible field walker. This is a style difference, not a semantic one — reverting to `FATAL` is always an option.

### 2.2 Implement `get_weight()`

Add after the `get_time()` implementation in `perf.cpp` (lines 277–281):

```cpp
uint64_t perf_event::record::get_weight() const {
  ASSERT(is_sample()) << "Record is not a sample";
  if (!_source.is_sampling(sample::weight)) return 0;
  return *locate_field<sample::weight, uint64_t*>();
}
```

---

## Step 3: `libcoz/thread_state.h` — Extend Thread State

**File path**: `coz/libcoz/thread_state.h`

**Principle**: Only add new fields. **Preserve** COZ's original `callchain`-related fields and the `is_blocked` atomic variable (BCOZ deleted them, but new COZ still requires them).

Complete field comparison between the two versions:

| Field | New COZ | BCOZ | Migration Decision |
|-------|---------|------|--------------------|
| `in_use` | `bool` | `bool` | Unchanged |
| `local_delay` | `std::atomic<size_t>` | `size_t` (non-atomic) | **Keep COZ's atomic version** to avoid breaking existing logic |
| `based_local_delay` | — | `size_t = 0` | **Add** |
| `delayed_local_delay` | — | `size_t = 0` | **Add** |
| `sampler` | `perf_event` | `perf_event` | Unchanged |
| `process_timer` | `timer` | `timer` | Unchanged |
| `pre_block_time` | `size_t` | `size_t` | Unchanged |
| `pre_local_time` | — | `size_t` | **Add** |
| `process_samples` | — | `std::atomic<int>` | **Add** (missed in earlier plan) |
| `ex_count` | — | `uint64_t = 1` | **Add** (missed in earlier plan) |
| `last_sample_time` | — | `uint64_t = 0` | **Add** |
| `in_wait` | — | `bool = false` | **Add** (coexists with `is_blocked`, does not replace it) |
| `is_blocked` | `std::atomic<bool>` | — (BCOZ removed) | **Keep in COZ** — still used by new COZ |
| `sync_local_with_global` | — | `std::atomic<bool>` | **Add** (missed in earlier plan) |
| `enable_print_log` | — | `bool = false` | Optional (debug only) |
| `fout` | — | `std::ofstream` | Optional (per-thread log) |
| `_hit_callchain_lock` | `spinlock` (Linux only) | — (BCOZ removed) | **Keep in COZ** |
| `_hit_callchains` | `unordered_map` (Linux only) | — (BCOZ removed) | **Keep in COZ** |

### 3.1 Add New Fields

In `coz/libcoz/thread_state.h`, after the `is_blocked` declaration (line 29) and before the `#ifndef __APPLE__` block, add:

```cpp
  // --- BCOZ: Blocked samples extensions ---

  // Accumulates delay for on-CPU hits that occur during an off-CPU experiment;
  // applied to local_delay when the thread resumes CPU execution (post_block)
  size_t delayed_local_delay = 0;

  // Accumulates delay for relational (based) profiling experiments
  size_t based_local_delay = 0;

  // Local time snapshot taken at pre_block(); used for on-CPU delay staging
  size_t pre_local_time = 0;

  // Guard: prevents concurrent sample processing on the same thread state.
  // Named process_samples in BCOZ source; 0 = idle, 1 = processing.
  std::atomic<int> process_samples{0};

  // Experiment count at last pre_block() call; used to correlate pre/post_block
  // pairs across experiment boundaries
  uint64_t ex_count = 1;

  // Timestamp of last processed sample (prevents reprocessing stale samples
  // when blocked samples arrive out of order)
  uint64_t last_sample_time = 0;

  // True while the thread is inside a blocking wait (pre_block → post_block).
  // Non-atomic because only the thread itself writes it.
  // Coexists with is_blocked (which COZ still uses); in_wait is checked by
  // the BCOZ blocking logic, is_blocked continues to serve existing COZ logic.
  bool in_wait = false;

  // Synchronize local_delay with global when set (used by based-profiling path)
  std::atomic<bool> sync_local_with_global{false};
```

> **Priority note**: The core migration items for Step 3 are:
> - `based_local_delay`, `delayed_local_delay`, `process_samples`, `last_sample_time`, `in_wait`
>
> `pre_local_time`, `ex_count`, and `sync_local_with_global` appear in BCOZ's `thread_state.h` but have no confirmed consumer paths in this repository — they look more like residual/reserved fields and should not be placed at the same priority level as the items above.

**Optional debug fields** (add only if `COZ_LOG` support is needed):

```cpp
  // Enable per-thread debug logging (set via COZ_LOG env var)
  bool enable_print_log = false;

  // Per-thread log file (only opened when enable_print_log is true)
  std::ofstream fout;
```

If `COZ_LOG` support is not needed, both fields can be skipped and `#include <fstream>` does not need to be added.

### 3.2 Preserve COZ's Original Fields (Do Not Delete)

The following fields were deleted in BCOZ but **new COZ still uses them and they must be kept**:

- `std::atomic<bool> is_blocked` (line 29) — COZ's blocking tracker used in `pre_block()`/`post_block()`
- `spinlock _hit_callchain_lock` (line 32) — callchain concurrency lock, Linux only
- `std::unordered_map<std::string, size_t> _hit_callchains` (line 33) — callchain counts, Linux only

### 3.3 Decision on `local_delay` Type

BCOZ changed `local_delay` from `std::atomic<size_t>` to plain `size_t` on the grounds that only the thread itself writes it. **This migration does not make that change** because:
1. New COZ's `add_delays()` and other paths access `local_delay` via `.load()` / `.store()`, so changing the type would require widespread modifications
2. The `process_samples` atomic guard already prevents concurrent processing
3. This can be done as a separate follow-up optimization

---

## Step 4: `libcoz/profiler.h` — Declarations and Inline Implementation Migration

**File path**: `coz/libcoz/profiler.h`

### 4.1 Update `startup()` Signature

Change the existing signature (around lines 67–72):
```cpp
void startup(const std::string& outfile,
             line* fixed_line,
             int fixed_speedup,
             bool end_to_end,
             bool use_callchain,
             size_t warmup_delay_ns = 0);
```

To (merging both versions' parameters, preserving COZ-unique parameters):
```cpp
void startup(const std::string& outfile,
             line* fixed_line,
             int fixed_speedup,
             bool end_to_end,
             bool use_callchain,
             size_t warmup_delay_ns = 0,
             char blocked_scope = 0,
             line* based_line = nullptr,
             char based_blocked = 0,
             int based_speedup = -1);
```

### 4.2 Add New Method Declarations

**Public methods** (before existing `call_process_blocked_samples`, see bcoz `profiler.h` lines 203–211):

```cpp
// Expose blocked sample processing for external call sites (e.g. intercepted syscalls)
void call_process_blocked_samples();
```

**Private methods** (near the existing `process_samples()` declaration, see bcoz `profiler.h` lines 249–264):

```cpp
// Process blocked (off-CPU) samples with weight support (Linux/bperf only)
void process_blocked_samples(thread_state* state);

// Match a sample PC against the "based" (relational) line
bool based_match_line(perf_event::record&);
```

**Static methods** (in the existing signal handler area):

> **Note**: Do not introduce `blocked_samples_ready()` for the BCOZ migration. The current bcoz source only has a declaration comment, no implementation body, and it is not registered as a separate signal handler path; the real blocked-sample processing is done by directly calling `process_blocked_samples()` through `post_block()` / `catch_up()` / `call_process_blocked_samples()`.
> Likewise, do not add `samples_ready_empty()` just because bcoz's header has a residual declaration — neither the `coz` nor `bcoz` repository has this separate signal path.
> `start_process(size_t)` is also a residual declaration of the same kind: no implementation body or call path exists in the current repository, and it should not be a required migration item.

> Note: `based_match_line` uses a non-const ref — consistent with `match_line` to avoid `locate_field` template instantiation issues.

### 4.3 Add New Member Variables

After the existing member variable section (`_global_delay`, `_selected_line`, etc.), add:

```cpp
// --- BCOZ: Off-CPU subclass sample counters ---
std::atomic<size_t> _blocked_all{0};     // all off-CPU events
std::atomic<size_t> _blocked_io{0};      // I/O blocking
std::atomic<size_t> _blocked_lock{0};    // lock-waiting
std::atomic<size_t> _blocked_sched{0};   // CPU scheduling
std::atomic<size_t> _blocked_blocked{0}; // other blocking (sleep, etc.)
std::atomic<size_t> _blocked_oncpu{0};   // on-CPU baseline counter

// --- BCOZ: Relational profiling ---
std::atomic<size_t> _based_global_delay{0};
std::atomic<size_t> _based_delay_size{0};
line* _based_line = nullptr;

// --- BCOZ: Configuration flags ---
char _blocked_scope = 0;          // 'i'/'l'/'s'/'b'/'a'/'o', 0 = disabled
char _based_blocked = 0;          // off-CPU type for relational profiling
int  _based_fixed_delay_size = -1;// fixed speedup for relational profiling

// --- BCOZ: Experiment counter (for pre_block/post_block correlation) ---
// NOTE: variable name matches BCOZ source (no underscore prefix)
std::atomic<uint64_t> ex_count{0};

// --- BCOZ: Debug / misc ---
std::atomic<bool> print{false};   // debug print toggle
int num_tid = 0;                  // TID tracking (multi-process support)
bool omit_experiment = false;     // skip experiment flag
bool _enable_print_log = false;   // per-profiler log enable
```

### 4.4 Update `catch_up()`, `pre_block()`, `post_block()` Declarations

Keep method signatures unchanged; inline implementations are updated in the sections below.

### 4.5 Update Constructor

Append new variable initialization in the `profiler()` constructor body (see bcoz `profiler.h` lines 221–238):

```cpp
profiler() : _warmup_delay_ns(0) {
  // ... existing initialization ...

  // BCOZ: blocked profiling state
  _based_global_delay.store(0);
  _based_delay_size.store(0);
  _blocked_all.store(0);
  _blocked_io.store(0);
  _blocked_sched.store(0);
  _blocked_lock.store(0);
  _blocked_blocked.store(0);
  _blocked_oncpu.store(0);
  ex_count.store(0);
  print.store(false);
}
```

### 4.6 Update `pre_block()` — Fix Race Condition

Update the existing `pre_block()` inline implementation per the real BCOZ logic:

> **Correction**: An earlier version of this document incorrectly listed `pre_local_time` as a snapshot needed by `post_block`. The current BCOZ source does not use it. The fields that actually matter are `in_wait`, `pre_block_time`, and `ex_count`.
> Additionally, `is_blocked` as existing COZ state must still be maintained — do not remove `is_blocked.store(true/false)` just because `in_wait` is introduced.

```cpp
void profiler::pre_block() {
  thread_state* state = get_thread_state();
  if (!state) return;

  if (state->in_wait) return;

  // Preserve existing COZ blocked-state tracking.
  state->is_blocked.store(true);
  state->in_wait = true;
  state->pre_block_time = _global_delay.load();
  state->ex_count = (_experiment_active) ? ex_count.load() : 0;
}
```

### 4.7 Update `post_block()` — Fix Bug + Handle Staged Delays

> Do not gate `process_blocked_samples()` on `_blocked_scope` / `_based_blocked`. In faithful BCOZ migration, `process_blocked_samples()` should be called whenever an experiment is active.

```cpp
void profiler::post_block(bool skip_delays) {
  thread_state* state = get_thread_state();
  if (!state) return;
  if (!state->in_wait) return;

  state->set_in_use(true);  // Keep this layer if merged COZ retains signal-race protection

  if (!_experiment_active) {
    state->local_delay.store(_global_delay.load());
  } else if (skip_delays) {
    size_t global_now = _global_delay.load();
    size_t delta = global_now - state->pre_block_time;
    if (delta > 0)
      state->local_delay.fetch_add(delta);
  }

  // Keep COZ's existing blocked-state bookkeeping in sync.
  state->is_blocked.store(false);
  state->in_wait = false;

  if (_experiment_active)
    process_blocked_samples(state);

  state->set_in_use(false);
}
```

### 4.8 Update `catch_up()` — Call `process_blocked_samples`

```cpp
void profiler::catch_up() {
  thread_state* state = get_thread_state();
  if (!state) return;

  if (_experiment_active) {
    state->set_in_use(true);  // Keep if merged COZ retains in_use protection
    process_blocked_samples(state);
    state->set_in_use(false);
  }
}
```

### 4.9 Add `call_process_blocked_samples()` Inline Wrapper

```cpp
void profiler::call_process_blocked_samples() {
  thread_state* state = get_thread_state();
  if (!state) return;

  state->set_in_use(true);  // Keep if merged COZ retains in_use protection
  process_blocked_samples(state);
  state->set_in_use(false);
}
```

---

## Step 5: `libcoz/profiler.cpp` — Core Logic Migration

**File path**: `coz/libcoz/profiler.cpp`

### 5.1 Update `startup()` Implementation

In the existing `startup()` function body, add blocked parameter initialization:

```cpp
void profiler::startup(const std::string& outfile, line* fixed_line,
                       int fixed_speedup, bool end_to_end, bool use_callchain,
                       size_t warmup_delay_ns, char blocked_scope,
                       line* based_line, char based_blocked, int based_speedup) {
  // ... existing initialization ...

  // BCOZ: blocked scope initialization
  _blocked_scope = (blocked_scope == 'n') ? 0 : blocked_scope;
  _based_blocked = (based_blocked == 'n') ? 0 : based_blocked;
  _based_line    = based_line;
  if (based_speedup >= 0 && based_speedup <= 100)
    _based_fixed_delay_size = SamplePeriod * based_speedup / 100;

  // ... rest of existing code ...
}
```

### 5.2 Update `begin_sampling()` — Enable Off-CPU-Aware Sampling by Default

> **Correction**: Off-CPU sampling capability must NOT be gated on `_blocked_scope` / `_based_blocked`.
> BCOZ's real behavior is **unconditional on/off-CPU integrated sampling**:
> `PERF_SAMPLE_WEIGHT | PERF_SAMPLE_TIME` are always enabled; kernel callchains are always preserved; kernel samples are never excluded.
> `_blocked_scope` and `_based_blocked` only affect how experiments *interpret* samples — not whether off-CPU samples are *collected*.

Field semantics:
- `_blocked_scope`: **Primary experiment target** selector. `0` = default BCOZ mode (selected line), still off-CPU-aware. `'o'` = on-CPU only; `'i'/'l'/'s'/'b'/'a'` = I/O, lock-wait, scheduling, other blocked, all off-CPU.
- `_based_blocked`: **Relational profiling base event type**. `0` = no off-CPU subclass filter for base event. `'i'/'l'/'s'/'b'/'a'` = use the corresponding off-CPU subclass as the base event. Note: BCOZ runtime code has a residual check for `'o'`, but the CLI never exposed this value and it should not be treated as a formal interface.

Update the `perf_event_attr` configuration in `begin_sampling()`:

```cpp
// Original (COZ):
pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_CALLCHAIN;
pe.exclude_kernel = 1;

// Updated (faithful BCOZ migration):
pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_CALLCHAIN
               | PERF_SAMPLE_WEIGHT    // blocked sample weight
               | PERF_SAMPLE_TIME;     // sample timestamp for de-dup / ordering

// BCOZ needs to capture off-CPU (kernel-triggered) samples by default,
// so these flags should be enabled unconditionally.
// Do not gate them on _blocked_scope / _based_blocked:
//   _blocked_scope == 0  is still default BCOZ mode, not "fall back to pure on-CPU COZ"
//   _based_blocked == 0  only means relational profiling does not filter base events by off-CPU type
pe.exclude_idle             = 1;
pe.exclude_kernel           = 0;   // Include kernel samples (syscall-level blocking)
pe.exclude_user             = 0;
pe.exclude_callchain_kernel = 0;
pe.exclude_callchain_user   = 0;
```

For `wakeup_events`:
```cpp
// Faithful BCOZ migration: keep the same global 1-sample batching as bcoz
pe.wakeup_events = SampleBatchSize;  // = 1 in BCOZ (immediate processing)
```

If retaining new COZ's larger batch size (e.g., 10) for reduced overhead, treat this as an **explicit performance trade-off**, not conditional on `_blocked_scope != 0`. Otherwise, gating it on `_blocked_scope != 0` would incorrectly degrade the default BCOZ mode into "off-CPU-aware only when blocked_scope is explicitly enabled".

### 5.3 Add `process_blocked_samples()` Function

Add after the existing `process_samples()` function (corresponding to bcoz `profiler.cpp` lines 617–702).

> **Important correction**: This section must be migrated using the actual bcoz function as the baseline — do not rewrite it as "staged pseudocode" and then reassemble the logic yourself. An earlier version of this document incorrectly stated that `_based_blocked` must coexist with `_based_line`, incorrectly described the default selected-line path as `fixed-line`, and omitted the non-experiment state synchronization branch — all of which would change behavior.

Key rules when merging into new COZ:
- Preserve BCOZ's overall control flow order: `guard → read sample → based profiling → blocked_scope → default selected-line path → end settlement`
- Preserve BCOZ's based profiling priority: check `_based_blocked != 0` before `_based_line`
- Preserve default path semantics: when `_blocked_scope == 0`, off-CPU hits settle immediately; on-CPU hits are staged to `delayed_local_delay`
- Preserve the non-experiment synchronization branch: `local_delay = _global_delay` and clear `delayed_local_delay`
- Preserve new COZ's two side effects: matching samples record `hit_callchains`; wait-time accounting is maintained via `local_delay` / `delayed_local_delay` / `add_delays()`
- Preserve new COZ's warmup semantics: consume perf ring buffer records before `_warmup_complete`, but `continue` without counting

```cpp
void profiler::process_blocked_samples(thread_state* state) {
  bool process_blocked_sample = false;
  size_t local_delay_inc = 0;

  if (state->process_samples.load())
    return;

  state->process_samples.fetch_add(1);

  for (perf_event::record r : state->sampler) {
    if (!_warmup_complete.load(std::memory_order_relaxed))
      continue;

    if (r.is_sample()) {
      REQUIRE(r.get_time() >= state->last_sample_time) << "Already processed sample!";
      state->last_sample_time = r.get_time();

      size_t sample_count = static_cast<size_t>(r.get_weight() + 1);

      std::pair<line*, bool> sampled_line = match_line(r);
      if (sampled_line.first)
        sampled_line.first->add_sample(sample_count);

      if (_experiment_active.load()) {
        if (_based_blocked != 0) {
          if ((_based_blocked == 'i') && r.is_io() ||
              (_based_blocked == 'l') && r.is_lock() ||
              (_based_blocked == 's') && r.is_sched() ||
              (_based_blocked == 'b') && r.is_blocked() ||
              (_based_blocked == 'a') && r.is_blocked_any()) {
            state->based_local_delay += _based_delay_size.load() * sample_count;
          }
        } else if (_based_line && based_match_line(r)) {
          state->based_local_delay += _based_delay_size.load() * sample_count;
        }

        if (_blocked_scope != 0) {
          if ((_blocked_scope == 'i') && r.is_io()) {
            _blocked_io.fetch_add(sample_count);
          } else if ((_blocked_scope == 'l') && r.is_lock()) {
            _blocked_lock.fetch_add(sample_count);
          } else if ((_blocked_scope == 's') && r.is_sched()) {
            _blocked_sched.fetch_add(sample_count);
          } else if ((_blocked_scope == 'b') && r.is_blocked()) {
            _blocked_blocked.fetch_add(sample_count);
          } else if ((_blocked_scope == 'a') && r.is_blocked_any()) {
            _blocked_all.fetch_add(sample_count);
          } else if ((_blocked_scope == 'o') && !r.is_blocked_any()) {
            _blocked_oncpu.fetch_add(sample_count);
#ifndef __APPLE__
            if (_use_callchain && r.get_callchain().size() > 0) {
              string chain = build_callchain_string(r);
              if (!chain.empty()) {
                state->_hit_callchain_lock.lock();
                state->_hit_callchains[chain]++;
                state->_hit_callchain_lock.unlock();
              }
            }
#endif
            // On-CPU blocked-scope hit: stage until thread resumes on CPU,
            // then flush in process_samples()/post_block().
            state->delayed_local_delay += _delay_size.load() * sample_count;
            continue;
          } else {
            continue;
          }

          // COZ parity: when this sample contributes to the current experiment
          // target, keep the existing hit_callchain bookkeeping.
          // Off-CPU subclass hit: record callchain, materialize delay immediately
#ifndef __APPLE__
          if (_use_callchain && r.get_callchain().size() > 0) {
            string chain = build_callchain_string(r);
            if (!chain.empty()) {
              state->_hit_callchain_lock.lock();
              state->_hit_callchains[chain]++;
              state->_hit_callchain_lock.unlock();
            }
          }
#endif
          local_delay_inc += _delay_size.load() * sample_count;
          process_blocked_sample = true;
          continue;
        }

        if (sampled_line.second && !r.is_lock()) {
#ifndef __APPLE__
          if (_use_callchain && r.get_callchain().size() > 0) {
            string chain = build_callchain_string(r);
            if (!chain.empty()) {
              state->_hit_callchain_lock.lock();
              state->_hit_callchains[chain]++;
              state->_hit_callchain_lock.unlock();
            }
          }
#endif

          if (r.is_blocked_any()) {
            // Wait-time accounting for off-CPU selected-line hits: immediate.
            local_delay_inc += _delay_size.load() * sample_count;
            process_blocked_sample = true;
          } else {
            // Wait-time accounting for on-CPU selected-line hits: stage until
            // the thread resumes on CPU, then flush in process_samples()/post_block().
            state->delayed_local_delay += _delay_size.load() * sample_count;
          }
        }
      }
    }
  }

  if (_experiment_active.load()) {
    if (process_blocked_sample) {
      state->local_delay.fetch_add(local_delay_inc);
      add_delays(state);
    }
  } else {
    state->local_delay.store(_global_delay.load());
    state->delayed_local_delay = 0;
  }

  state->process_samples.fetch_add(-1);
}
```

The design intent here must be consistent with new COZ's `process_samples()`:

- Whenever a sample qualifies as a "current experiment target hit", do two things simultaneously:
  - Record `hit_callchains`
  - Record the corresponding wait-time debt for that hit
- Samples during the warmup phase must be consumed but ignored, preventing pre-warmup samples from leaking into the formal experiment
- Wait-time debt is split into two categories:
  - Off-CPU / blocked-scope hits: go into `local_delay_inc`, flushed into `local_delay` at the end of the current round
  - On-CPU selected-line hits: staged in `delayed_local_delay` first; only flushed into `local_delay` when the thread is back on CPU

Do not place callchain bookkeeping before the based-profiling or blocked-subclass decision — doing so would incorrectly record "ordinary samples" as "target-hit samples".

### 5.4 Update `process_samples()` — Integrate `delayed_local_delay`

> Do not simply prepend a guard and a `delayed_local_delay` flush to the existing `process_samples()`. For faithful BCOZ semantics, the Linux path of `process_samples()` should be aligned overall with bcoz `profiler.cpp` lines 704–770, then COZ's callchain / warmup / `!is_coz_header(...)` logic spliced back in.

Minimum requirements:
- Use `state->process_samples` as a concurrent processing guard at the start
- Preserve COZ's warmup guard: `continue` if `_warmup_complete == false`
- Flush `delayed_local_delay` into `local_delay` at the top
- Preserve `_based_blocked` / `_based_line` priority matching BCOZ
- Preserve `_blocked_scope != 0` subclass-filter semantics
- Preserve `!r.is_lock()` check in the default selected-line path
- Preserve COZ's callchain recording for all experiment-target hits
- Preserve COZ's `!is_coz_header(sampled_line.first)` filter when selecting the next line
- Call `add_delays(state)` at the end

```cpp
void profiler::process_samples(thread_state* state) {
  if (state->process_samples.load())
    return;

  state->process_samples.fetch_add(1);

  if (state->delayed_local_delay > 0) {
    state->local_delay.fetch_add(state->delayed_local_delay);
    state->delayed_local_delay = 0;
  }

  for (perf_event::record r : state->sampler) {
    // Preserve COZ warmup semantics: consume records but ignore samples until
    // warmup completes, so pre-warmup samples do not leak into the experiment.
    if (!_warmup_complete.load(std::memory_order_relaxed))
      continue;

    if (!r.is_sample())
      continue;

    REQUIRE(r.get_time() >= state->last_sample_time) << "Already processed sample!";
    state->last_sample_time = r.get_time();

    size_t sample_count = static_cast<size_t>(r.get_weight() + 1);

    std::pair<line*, bool> sampled_line = match_line(r);
    if (sampled_line.first)
      sampled_line.first->add_sample(sample_count);

    if (_experiment_active.load()) {
      if (_based_blocked != 0) {
        if ((_based_blocked == 'i') && r.is_io() ||
            (_based_blocked == 'l') && r.is_lock() ||
            (_based_blocked == 's') && r.is_sched() ||
            (_based_blocked == 'b') && r.is_blocked() ||
            (_based_blocked == 'a') && r.is_blocked_any()) {
          state->based_local_delay += _based_delay_size.load() * sample_count;
        }
      } else if (_based_line && based_match_line(r)) {
        state->based_local_delay += _based_delay_size.load() * sample_count;
      }

      if (_blocked_scope != 0) {
        bool target_hit = false;

        if ((_blocked_scope == 'i') && r.is_io()) {
          _blocked_io.fetch_add(sample_count); target_hit = true;
        } else if ((_blocked_scope == 'l') && r.is_lock()) {
          _blocked_lock.fetch_add(sample_count); target_hit = true;
        } else if ((_blocked_scope == 's') && r.is_sched()) {
          _blocked_sched.fetch_add(sample_count); target_hit = true;
        } else if ((_blocked_scope == 'b') && r.is_blocked()) {
          _blocked_blocked.fetch_add(sample_count); target_hit = true;
        } else if ((_blocked_scope == 'a') && r.is_blocked_any()) {
          _blocked_all.fetch_add(sample_count); target_hit = true;
        } else if ((_blocked_scope == 'o') && !r.is_blocked_any()) {
          _blocked_oncpu.fetch_add(sample_count); target_hit = true;
        } else {
          continue;
        }

#ifndef __APPLE__
        if (target_hit && _use_callchain && r.get_callchain().size() > 0) {
          string chain = build_callchain_string(r);
          if (!chain.empty()) {
            state->_hit_callchain_lock.lock();
            state->_hit_callchains[chain]++;
            state->_hit_callchain_lock.unlock();
          }
        }
#endif
        // process_samples() is the periodic path: unlike process_blocked_samples(),
        // matching hits are materialized directly into local_delay here.
        state->local_delay.fetch_add(_delay_size.load() * sample_count);
        continue;
      }

      if (sampled_line.second && !r.is_lock()) {
        state->local_delay.fetch_add(_delay_size.load() * sample_count);

#ifndef __APPLE__
        if (_use_callchain && r.get_callchain().size() > 0) {
          string chain = build_callchain_string(r);
          if (!chain.empty()) {
            state->_hit_callchain_lock.lock();
            state->_hit_callchains[chain]++;
            state->_hit_callchain_lock.unlock();
          }
        }
#endif
      }

    } else if (sampled_line.first != nullptr && _next_line.load() == nullptr
               && !is_coz_header(sampled_line.first)) {
      _next_line.store(sampled_line.first);
    }
  }

  add_delays(state);

  state->process_samples.fetch_add(-1);
}
```

Division of responsibility between 5.3 and 5.4:
- `process_blocked_samples()`: Blocking-boundary triggered "immediate settlement path" — off-CPU hits go into `local_delay_inc`, flushed to `local_delay` at the end
- `process_samples()`: Periodic sampling path — matching hits go directly via `fetch_add` to `local_delay`

Both must preserve: warmup filtering, `sample_count = weight + 1`, `based_*` logic, callchain recording, `add_delays(state)` at end.

### 5.5 Update `add_delays()` — BCOZ Dual-Channel Delay Logic

> **Important correction**: An earlier version of this document oversimplified `add_delays()`. The real BCOZ version is not simply "fix the `is_blocked` check and sync `based_local_delay`". It:
> 1. Simultaneously maintains the primary delay channel `local_delay / _global_delay`
> 2. Simultaneously maintains the based delay channel `based_local_delay / _based_global_delay`
> 3. Preserves the `local > global` branch that advances the global delay
> 4. Preserves the `100000 ns` threshold and `sampler.stop()/start()` wrapping around `wait()`

Rewrite the existing `add_delays()` implementation with the following structure:

```cpp
void profiler::add_delays(thread_state* state) {
  if (_experiment_active.load()) {
    // Preserve both "currently blocked" notions:
    // - is_blocked: existing COZ bookkeeping
    // - in_wait: BCOZ pre_block/post_block boundary flag
    if (state->is_blocked.load() || state->in_wait)
      return;

    size_t global_delay       = _global_delay.load();
    size_t local_delay        = state->local_delay.load();
    size_t based_global_delay = _based_global_delay.load();
    size_t based_local_delay  = state->based_local_delay;

    // BCOZ relational-profiling channel: keep based_local_delay and
    // _based_global_delay synchronized in parallel with the main delay path.
    if (based_local_delay > based_global_delay) {
      _based_global_delay.fetch_add(based_local_delay - based_global_delay);
    } else if (based_local_delay < based_global_delay &&
               (based_global_delay - based_local_delay > 100000)) {
      state->sampler.stop();
      size_t waited = wait(based_global_delay - based_local_delay - 100000);
      state->based_local_delay += waited;
      state->sampler.start();
    }

    // Main COZ/BCOZ delay channel.
    if (local_delay > global_delay) {
#ifdef __APPLE__
      // Preserve current COZ macOS overshoot handling here; do not regress it
      // while adding BCOZ Linux behavior.
      g_delays_skipped.fetch_add(1, std::memory_order_relaxed);
#else
      _global_delay.fetch_add(local_delay - global_delay);
#endif
    } else if (local_delay < global_delay &&
               (global_delay - local_delay > 100000)) {
      state->sampler.stop();
      size_t needed = global_delay - local_delay - 100000;
      size_t waited = wait(needed);
#ifdef __APPLE__
      // Keep COZ's existing macOS accounting / overshoot correction.
      state->local_delay.fetch_add(needed);
      if (waited > needed)
        g_experiment_overshoot.fetch_add(waited - needed, std::memory_order_relaxed);
      g_delays_applied.fetch_add(1, std::memory_order_relaxed);
#else
      state->local_delay.fetch_add(waited);
#endif
      state->sampler.start();
    }

  } else {
    // Outside experiments: snap both delay channels to global state.
    state->local_delay.store(_global_delay.load());
    state->based_local_delay = _based_global_delay.load();
  }
}
```

Key points:
- `in_wait` cannot replace `is_blocked`; both must be preserved and maintained. Checking both in `add_delays()` is the safest approach and stays closest to the "preserve COZ + introduce BCOZ" merge goal
- The `100000` ns threshold and `wait(... - 100000)` are existing BCOZ behavior — do not remove from the Linux path
- The `local > global` branch is the mechanism that turns "a virtually accelerated thread" into global delay — do not remove
- `based_local_delay` also has both ahead/behind cases, not just a simple catch-up path
- macOS continues to use COZ's existing overshoot correction logic; blocked-samples related capabilities still degrade on macOS per the Step 9 strategy

### 5.6 Update `profiler_thread()` Experiment Loop — Embed BCOZ Experiment Semantics into COZ Skeleton

> **Do not replace** the new COZ `profiler_thread()` with BCOZ's. New COZ carries warmup, JSON output, callchain aggregation, macOS `process_all_samples()` / overshoot correction, and `min_delta` filtering. The correct approach is: **preserve COZ's main skeleton and embed BCOZ's experiment state and output semantics**.

Incremental changes:

**a) Keep COZ's existing skeleton intact:**
- startup JSON / legacy text output
- `hit_callchains_enabled` startup field
- `_warmup_delay_ns` and `_warmup_complete`
- macOS `process_all_samples()` / `apply_pending_delays()` / overshoot correction
- `min_delta >= ExperimentTargetDelta` filtering before emitting experiment data
- Linux experiment-end `hit_callchains` aggregation

**b) Add `based_delay_size` after `delay_size`:**

This should faithfully follow bcoz's selection method — **do not incorrectly restrict it to "only generate when `_based_line` is set"**. `_based_blocked` also uses this channel.

```cpp
size_t based_delay_size;
if (_based_fixed_delay_size >= 0) {
  based_delay_size = _based_fixed_delay_size;
} else {
  size_t r = delay_dist(generator);
  if (r <= ZeroSpeedupWeight) {
    based_delay_size = 0;
  } else {
    based_delay_size = (r - ZeroSpeedupWeight) * SamplePeriod / SpeedupDivisions;
  }
}
_based_delay_size.store(based_delay_size);
```

**c) Record starting value of the active `_blocked_scope` counter:**

Do not unconditionally snapshot all `_blocked_*` counters as an earlier version of this document suggested; bcoz's real implementation only records the starting value of the one counter corresponding to the current experiment scope.

```cpp
size_t starting_blocked_scope = 0;
if (_blocked_scope != 0) {
  switch (_blocked_scope) {
    case 'o': starting_blocked_scope = _blocked_oncpu.load();   break;
    case 'i': starting_blocked_scope = _blocked_io.load();      break;
    case 'l': starting_blocked_scope = _blocked_lock.load();    break;
    case 's': starting_blocked_scope = _blocked_sched.load();   break;
    case 'b': starting_blocked_scope = _blocked_blocked.load(); break;
    case 'a': starting_blocked_scope = _blocked_all.load();     break;
  }
}
```

**d) Preserve COZ timing order, add BCOZ's `ex_count` at experiment start:**

`ex_count.fetch_add(1)` is bcoz's experiment counter and should be added to the experiment startup path; at the same time, preserve COZ's existing time snapshot positions — do not move `start_time` back to before setup.

```cpp
size_t start_time           = get_time();
size_t starting_samples     = selected->get_samples();
size_t starting_delay_time  = _global_delay.load();

_experiment_active.store(true);
ex_count.fetch_add(1);
```

**e) Add full `omit_experiment` handling:**

Do not just `continue` before output. bcoz's real logic also clears `_next_line`, deactivates the experiment, and enters cooloff.

```cpp
omit_experiment = false;

// Preserve COZ's existing Linux/macOS wait skeleton
// ... wait(experiment_length) or macOS chunked wait ...

if (omit_experiment) {
  omit_experiment = false;
  _next_line.store(nullptr);
  _experiment_active.store(false);
  if (_running)
    wait(ExperimentCoolOffTime);
  continue;
}
```

**f) Compute `blocked_scope` selected_samples and `based_speedup`:**

```cpp
float speedup       = (float)delay_size      / (float)SamplePeriod;
float based_speedup = (float)based_delay_size / (float)SamplePeriod;

size_t selected_samples = selected->get_samples() - starting_samples;
const char* scope_name  = nullptr;

if (_blocked_scope != 0) {
  switch (_blocked_scope) {
    case 'o': scope_name = "ON_CPU";     selected_samples = _blocked_oncpu.load()   - starting_blocked_scope; break;
    case 'i': scope_name = "IO";         selected_samples = _blocked_io.load()      - starting_blocked_scope; break;
    case 'l': scope_name = "LOCK";       selected_samples = _blocked_lock.load()    - starting_blocked_scope; break;
    case 's': scope_name = "SCHEDULING"; selected_samples = _blocked_sched.load()   - starting_blocked_scope; break;
    case 'b': scope_name = "BLOCKED";    selected_samples = _blocked_blocked.load() - starting_blocked_scope; break;
    case 'a': scope_name = "OFF_CPU";    selected_samples = _blocked_all.load()     - starting_blocked_scope; break;
  }
}
```

**g) Output layer — preserve COZ format while embedding BCOZ semantics:**

- Legacy text output:
  - When `_blocked_scope != 0`, `selected=` should output `ON_CPU/IO/LOCK/SCHEDULING/BLOCKED/OFF_CPU` rather than a source line
  - When `_based_line` or `_based_blocked` is active, preserve BCOZ's appended tag semantics, e.g. `(IO,12.00%|)` / `(OFF_CPU,5.00%|)`
  - `delay=` field is also preserved
- JSON output:
  - Preserve COZ's existing `type/selected/speedup/duration/selected_samples/hit_callchains`
  - When `_blocked_scope != 0`, `selected` should be `scope_name` rather than a line string
  - Add a `blocked_scope` field so consumers can explicitly identify this as a blocked-scope experiment
  - When based profiling is active, add `based_target` and `based_speedup` fields — do not bury this information solely in a legacy text suffix

> **Conclusion**: The migration baseline for this section should be "preserve COZ's outer structure and output capabilities, embed BCOZ's `blocked_scope` / `based_*` / `omit_experiment` / `ex_count` semantics" — not merely appending a few counter fields.

### 5.7 Update `add_thread()` and `start_thread()`

> **Important correction**: An earlier version of this document oversimplified these two functions. The real difference between `bcoz` and `coz` here is not just "add `num_tid++` and `process_samples.store(0)`" — it is about explicitly resetting new fields when threads are registered.

The real implementation differences between the two versions:

- `coz::add_thread()`:
  - Registers current `tid` with `_thread_states`
  - Increments `_num_threads_running` on success
  - Logs via `VERBOSE`
- `bcoz::add_thread()`:
  - Only does `_thread_states.insert(gettid())`
  - Additionally does `num_tid++`
  - `num_tid` has no confirmed consumer path in the current repository — appears to be a residual counter
- `coz::start_thread()`:
  - Registers thread
  - Writes `arg->_parent_delay_time` into `local_delay`
  - Starts sampling, executes real thread function
- `bcoz::start_thread()`:
  - Additionally does `state->process_samples.store(0)`
  - Has optional per-thread debug log open/close
  - Main flow otherwise identical to COZ

The correct migration focus is **explicitly resetting merged `thread_state` fields at thread registration**, so that `static_map` slot reuse does not cause new threads to inherit stale state.

**a) `add_thread()` — keep COZ body, add reset of BCOZ fields:**

> `num_tid++` can be kept as an optional compatibility counter if you want to preserve BCOZ source shape, but it is not the core of this step and should not replace `_num_threads_running`.

```cpp
thread_state* profiler::add_thread() {
  pid_t tid = gettid();
  thread_state* state = _thread_states.insert(tid);
  if (state != nullptr) {
    _num_threads_running += 1;
    VERBOSE << "Registered thread tid=" << tid;

    state->local_delay.store(0);
    state->based_local_delay    = 0;
    state->delayed_local_delay  = 0;
    state->pre_block_time       = 0;
    state->process_samples.store(0);
    state->last_sample_time     = 0;
    state->in_wait              = false;
    state->is_blocked.store(false);
    state->sync_local_with_global.store(false);
#ifndef __APPLE__
    state->_hit_callchains.clear();
#endif
  }
  return state;
}
```

**b) `start_thread()` — keep COZ flow, inherit parent delay:**

Since the state reset is already handled in `add_thread()`, `start_thread()` does not need to pile "initialize BCOZ state" into the child thread path. It only needs to preserve:

```cpp
void* profiler::start_thread(void* p) {
  thread_start_arg* arg = reinterpret_cast<thread_start_arg*>(p);

  thread_state* state = get_instance().add_thread();
  REQUIRE(state) << "Failed to add thread";

  state->local_delay.store(arg->_parent_delay_time);

  thread_fn_t real_fn = arg->_fn;
  void* real_arg      = arg->_arg;
  delete arg;

  profiler::get_instance().begin_sampling(state);
  void* result = real_fn(real_arg);
  pthread_exit(result);
}
```

### 5.8 Add `based_match_line()` Implementation

> **Important correction**: New COZ does not have `based_match_line()`; this is a BCOZ-only helper function.
> An earlier version of this document described it as "just call `match_line()` and compare `result.first`" — this is not equivalent:
> 1. `match_line()`'s matching target is `_selected_line`, not `_based_line`
> 2. `match_line()`'s returned `first` is "the first known source line / or the hit selected line" — it is not guaranteed to equal `_based_line`
> 3. BCOZ's real implementation independently scans the callchain looking for `_based_line`
>
> If the goal is a faithful BCOZ migration, add it per the BCOZ original implementation rather than reusing `match_line()`.

```cpp
bool profiler::based_match_line(perf_event::record& r) {
  if (!_based_line) return false;

  for (uint64_t pc : r.get_callchain()) {
    line* l = memory_map::get_instance().find_line(pc - 1).get();
    if (l && l == _based_line)
      return true;
  }

  return false;
}
```

This intentionally scans only the callchain, not `r.get_ip()` — matching BCOZ's actual behavior. If you later want to change `based_line` semantics to also check `get_ip()` (like `selected_line`), that is a separate behavioral change and should not be passed off as "migrated per BCOZ".

### 5.9 Update `samples_ready()` Signal Handler (Linux)

> **Correction**: `samples_ready()` should NOT conditionally dispatch to `process_blocked_samples()` based on `_blocked_scope` / `_based_blocked`. In BCOZ's Linux path, the signal handler always calls `process_samples()`; `process_blocked_samples()` is the immediate settlement path triggered by `post_block()` / `catch_up()`.
>
> The main difference from new COZ lies only in the macOS branch: new COZ's macOS signal handler only calls `add_delays()` without parsing samples; the Linux path is essentially identical to BCOZ here.

```cpp
void profiler::samples_ready(int, siginfo_t*, void*) {
  thread_state* state = get_instance().get_thread_state();
  if (!state) return;

  // If merged COZ retains in_use protection, keep the existing check_in_use() guard
  if (!state->check_in_use())
    get_instance().process_samples(state);
}
```

---

## Step 6: `libcoz/libcoz.cpp` — Environment Variables and Entry Point

**File path**: `coz/libcoz/libcoz.cpp`

### 6.1 Add New Environment Variable Parsing

In `init_coz()`, **preserve all existing COZ parsing logic** and only add BCOZ's new parameters incrementally.

> **Do not revert COZ's existing logic**. `bcoz/libcoz/libcoz.cpp` lacks `warmup`, `callchain`, `filter_system_sources`, and macOS initialization paths — all of these are capabilities that new COZ must retain and should not be removed because of the blocked-samples migration.

Existing COZ logic to preserve includes: `COZ_WARMUP`, `COZ_CALLCHAIN`, `COZ_FILTER_SYSTEM`, default `%` for empty `source_scope`, macOS executable parsing, etc.

```cpp
// BCOZ: off-CPU profiling configuration
char blocked_scope = 0;
std::string blocked_scope_str = getenv_safe("COZ_BLOCKED_SCOPE", "");
if (!blocked_scope_str.empty()) blocked_scope = blocked_scope_str[0];

char based_blocked = 0;
std::string based_blocked_str = getenv_safe("COZ_BASED_BLOCKED", "");
if (!based_blocked_str.empty()) based_blocked = based_blocked_str[0];

int based_speedup = -1;
std::stringstream(getenv_safe("COZ_BASED_SPEEDUP", "-1")) >> based_speedup;

std::string based_line_name = getenv_safe("COZ_BASED_LINE", "");
```

### 6.2 Resolve `based_line`

After the existing `fixed_line` resolution logic:

```cpp
// BCOZ: resolve based_line from memory map (same logic as fixed_line)
std::shared_ptr<line> based_line;
if (!based_line_name.empty()) {
  based_line = memory_map::get_instance().find_line(based_line_name);
  REQUIRE(based_line) << "Based line \"" << based_line_name << "\" was not found.";
}
```

> This should use `REQUIRE` like `fixed_line`, also consistent with BCOZ's original implementation; do not silently degrade to a warning.

### 6.3 Update `profiler::startup()` Call

Change the existing call:
```cpp
profiler::get_instance().startup(output_file, fixed_line.get(), fixed_speedup,
                                 end_to_end, use_callchain, warmup_delay_ns);
```

To:
```cpp
profiler::get_instance().startup(output_file, fixed_line.get(), fixed_speedup,
                                 end_to_end, use_callchain, warmup_delay_ns,
                                 blocked_scope,    // BCOZ: new
                                 based_line.get(), // BCOZ: new
                                 based_blocked,    // BCOZ: new
                                 based_speedup);   // BCOZ: new
```

### 6.4 Export Symbol Compatibility — Do Not Duplicate Existing COZ Exports

This step cannot follow BCOZ's original file and do a "wholesale addition", because new COZ already has most of these exported interfaces.

Existing COZ exports already present: `_coz_add_delays()`, `_coz_pre_block()`, `_coz_post_block(int)`.

Add only the BCOZ compatibility aliases:

```cpp
extern "C" void _coz_post_block_0() {
  if (initialized) profiler::get_instance().post_block(false);
}

extern "C" void _coz_post_block_1() {
  if (initialized) profiler::get_instance().post_block(true);
}

extern "C" void _coz_catch_up() {
  if (initialized) profiler::get_instance().catch_up();
}
```

> Do **not** re-add `_coz_pre_block()` (already exported). Do **not** delete `_coz_add_delays()` or `_coz_post_block(int)` — `coz/include/coz.h` depends on them.

### 6.5 Preserve COZ/macOS Paths

The following must **not** be reverted:
- macOS constructor/destructor initialization paths
- macOS helper exports: `coz_initialized`, `coz_handle_pthread_create`, `coz_pre_block`
- `COZ_FILTER_SYSTEM` → `memory_map::build(..., !filter_system_sources)`
- `coz/include/coz.h` blocking API: `COZ_PRE_BLOCK` / `COZ_POST_BLOCK(skip_delays)` / `COZ_CATCH_UP`

BCOZ's `COZ_LOG` / per-thread debug log and commented-out syscall wrappers (`fsync`, `pread`, etc.) are **optional compatibility items**, not default required migration. The commented-out wrappers are not active runtime behavior in the current source and should not be treated as required migration.

---

## Step 7: `include/coz.h` — Compatibility Layer (Usually No New `coz_block.h` Needed)

> **Important note**: New COZ's `coz/include/coz.h` (around line 165) already provides custom blocking macros:
> `COZ_PRE_BLOCK`, `COZ_POST_BLOCK(skip_delays)`, `COZ_CATCH_UP`.
> Therefore, **usually no new `coz_block.h` is needed**.

New COZ's `coz/include/coz.h` already provides: `COZ_PRE_BLOCK`, `COZ_POST_BLOCK(skip_delays)`, `COZ_CATCH_UP`.

**Default recommendation**: Do not add `coz/include/coz_block.h`. Only add the `_coz_post_block_0/_1` and `_coz_catch_up` aliases in Step 6.4 if backward compatibility with old BCOZ code is required.

If a compatibility shim is needed, prefer a thin alias header rather than copying the BCOZ file:

```c
#include "coz.h"

#define COZ_POST_BLOCK_0 COZ_POST_BLOCK(0)
#define COZ_POST_BLOCK_1 COZ_POST_BLOCK(1)
```

---

## Step 8: `coz` (Python CLI Script) — Add New Arguments

**File path**: `coz/coz`

> **Migration principle**: Use new `coz/coz` as the base — do not revert its runtime library location logic, macOS `DYLD_INSERT_LIBRARIES` support, `COZ_PRELOAD` override, `--warmup` / `--use-callchain` / `--verbose` / `--legacy-format`, or the more complete `plot` subcommand. The Linux-only paths and debug output from `bcoz/bcoz` (such as printing library paths directly to stderr) should not be migrated back.

### 8.1 Add Arguments to the `run` Subcommand

```python
_run_parser.add_argument(
    '--blocked-scope',
    metavar='<subclass>',
    choices=['o', 'b', 'i', 'l', 's', 'a'],
    default=None,
    help='Profile specific off-CPU subclass: '
         'o=all on-CPU samples, '
         'i=blocking I/O, l=lock-waiting, '
         's=CPU scheduling, b=other blocking (sleep), '
         'a=all off-CPU events. '
         'Requires bperf/blocked-samples kernel.'
)

_run_parser.add_argument(
    '--based-line',
    metavar='<file>:<line>',
    default=None,
    help='Target line for relational profiling (measures impact of another line on this one).'
)

_run_parser.add_argument(
    '--based-blocked',
    metavar='<subclass>',
    choices=['b', 'i', 'l', 's', 'a'],
    default=None,
    help='Off-CPU subclass for relational profiling base event.'
)

_run_parser.add_argument(
    '--based-speedup',
    metavar='<0-100>',
    type=int,
    choices=list(range(0, 101)),
    default=None,
    help='Fixed speedup percentage for relational profiling.'
)
```

> `--based-blocked` intentionally excludes `'o'` — BCOZ's CLI did not expose it, and output branches lack full support for it.
>
> `--print-log` is not a default required migration item for Step 8. It corresponds to BCOZ's optional debug capability, and there is also an inconsistency between `COZ_PRINT_LOG` (in `bcoz/bcoz`) and `COZ_LOG` (in `bcoz/libcoz/libcoz.cpp`). Do not mix it into the core parameter set unless you intend to fix the entire debug logging pipeline at the same time.

### 8.2 Pass Arguments as Environment Variables

```python
if args.blocked_scope is not None:
    env['COZ_BLOCKED_SCOPE'] = args.blocked_scope

if args.based_line is not None:
    env['COZ_BASED_LINE'] = args.based_line

if args.based_blocked is not None:
    env['COZ_BASED_BLOCKED'] = args.based_blocked

if args.based_speedup is not None:
    env['COZ_BASED_SPEEDUP'] = str(args.based_speedup)
```

> Additional notes:
>
> - `--blocked-scope o` is not "default behavior" — it simply makes all on-CPU samples the primary experiment target. The actual default is **not setting `--blocked-scope` at all**, in which case the primary target remains the selected line, while the merged BCOZ runtime continues to sample in an off-CPU-aware manner.
> - `--based-speedup` should follow the style of the existing `--fixed-speedup`: `choices=0..100`, default `None`, written to the environment only when explicitly provided. This is consistent with the CLI conventions in both `coz/coz` and `bcoz/bcoz`, and avoids introducing the `-1` sentinel value.

---

## Step 9: Platform Isolation

Blocked samples requires the modified Linux 5.3.7 kernel. Guard kernel-specific code:

### 9.1 Conditional Compilation in `perf.h`

```cpp
#ifdef __linux__
// These flags are only present in the blocked-samples patched kernel.
// On standard kernels, these bits are unused and is_*() always returns false.
#define PERF_RECORD_MISC_LOCKWAIT   (64 << 0)
#define PERF_RECORD_MISC_SCHED      (32 << 0)
#define PERF_RECORD_MISC_IOWAIT     (16 << 0)
#define PERF_RECORD_MISC_BLOCKED    (8  << 0)
#endif
```

### 9.2 macOS Fallback in `profiler.cpp`

```cpp
#ifdef __linux__
  // process_blocked_samples() and PERF_SAMPLE_WEIGHT-related code
#else
  // macOS fallback: disable blocked_scope silently
  if (_blocked_scope != 0) {
    WARNING << "--blocked-scope requires Linux with blocked-samples kernel patch. Disabling.";
    _blocked_scope = 0;
  }
#endif
```

---

## Step 10: Build

After completing all migration steps, build and install using the following commands:

```bash
cd /home/bcoz/repos/blocked_samples/coz
cmake -S . -B build
cmake --build build -j
sudo cmake --install build
sudo ldconfig
```

---

## Implementation Order

```
Step 1 → Step 2    (perf layer: independent, no dependencies)
    ↓
Step 3             (thread_state: depends on perf.h new types)
    ↓
Step 4             (profiler.h: depends on thread_state fields)
    ↓
Step 5             (profiler.cpp: depends on all of the above)
    ↓
Step 6             (libcoz.cpp: depends on profiler.cpp signature)
    ↓
Step 7             (coz_block.h: independent, can be done in parallel)
Step 8             (coz script: independent, can be done in parallel)
    ↓
Step 9             (platform guards: global check)
    ↓
Step 10            (Build)
```

---

## Key Notes

### COZ Features to Preserve (Must Not Remove)

| Feature | Relevant Code | Notes |
|---------|--------------|-------|
| `use_callchain` | `startup()` parameter, `thread_state._hit_callchains` | Callchain tracking is a core new COZ feature |
| `warmup_delay_ns` | `startup()` parameter | Warmup delay |
| DWARF 5 support | `libcoz/inspect.cpp` | Do not modify this file |
| Viewer LLM integration | `coz` (Python script `plot` section) | Do not modify |
| macOS kperf sampling | `perf_macos.cpp/h` | Do not modify; `blocked_scope` degrades gracefully on macOS |

### SampleBatchSize Decision

BCOZ uses `SampleBatchSize = 1` globally, consistent with always-on off-CPU-aware sampling in `begin_sampling()`.

- **Faithful BCOZ migration**: keep `SampleBatchSize = 1`
- **Keeping new COZ's 10-sample batching**: treat this as a deliberate performance trade-off, evaluated independently — do not gate it on `_blocked_scope != 0`

```cpp
enum {
  SampleBatchSize = 1,  // faithful BCOZ behavior
};

// In begin_sampling():
pe.wakeup_events = SampleBatchSize;
```

### `local_delay` Type Decision

BCOZ changed `local_delay` to plain `size_t`. **Keep `std::atomic<size_t>`** in the merged COZ and use `.fetch_add()` / `.load()` / `.store()` consistently throughout migration code. Do not mix atomic and non-atomic write styles.
