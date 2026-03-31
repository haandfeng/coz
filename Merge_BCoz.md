# Merging BCOZ into COZ: Design Document

## Overview

This document describes how the **Blocked-COZ (BCOZ)** off-CPU causal profiling capabilities were merged into the **new COZ** codebase, producing a unified profiler that supports both on-CPU and off-CPU causal analysis while retaining all of COZ's modern features.

BCOZ was a research fork of the original COZ profiler that added kernel-level blocked-sample support — the ability to capture and classify off-CPU events (I/O blocking, lock contention, CPU scheduling, etc.) via a modified Linux 5.3.7 kernel. However, BCOZ diverged from COZ's mainline and lacked several features that COZ had since developed independently. The merge brings both capabilities together.

---

## What Was Merged from BCOZ

### 1. Off-CPU Event Classification via Kernel Perf Flags

BCOZ introduced four new `misc` flag bits in `perf_event_header` to classify why a thread went off-CPU:

```cpp
// libcoz/perf.h
#define PERF_RECORD_MISC_LOCKWAIT   (64 << 0)   // Lock contention
#define PERF_RECORD_MISC_SCHED      (32 << 0)   // Involuntary CPU scheduling
#define PERF_RECORD_MISC_IOWAIT     (16 << 0)   // Blocking I/O
#define PERF_RECORD_MISC_BLOCKED    (8  << 0)   // Generic blocking (sleep, epoll, etc.)
```

These flags are set by the modified kernel at sample time. The merged COZ exposes them as inline query methods on `perf_event::record`:

```cpp
inline bool is_lock() const;
inline bool is_sched() const;
inline bool is_io() const;
inline bool is_blocked() const;
inline bool is_blocked_any() const;
```

On a standard (unpatched) kernel, these bits are always zero, so the classification methods return false transparently — no runtime failure.

### 2. Weighted Samples (`PERF_SAMPLE_WEIGHT`)

BCOZ's kernel patch records how many consecutive sampling periods a thread spent off-CPU in the `weight` field of the perf sample record. This lets a single sample represent multiple periods of blocking.

The merged COZ adds:
- `sample::weight` to the `perf_event::sample` enum (`libcoz/perf.h`)
- `get_weight()` accessor on `perf_event::record` (`libcoz/perf.cpp`)
- Weight-aware field walking in `locate_field<>()` — positioned after `PERF_SAMPLE_STACK_USER` to match the kernel's sample record layout

Sample counting throughout the profiler uses `weight + 1` to account for the actual number of sampling periods represented:

```cpp
uint32_t sample_count = r.get_weight() + 1;
```

### 3. `process_blocked_samples()` — Off-CPU Sample Processing

This is the core function migrated from BCOZ (`profiler.cpp`). It processes perf ring buffer records with off-CPU awareness:

- **Warmup gating**: Samples collected before `_warmup_complete` are consumed but ignored, preventing startup transients from leaking into experiments.
- **Based-profiling path**: If `_based_blocked` or `_based_line` is configured, samples are first checked against the relational profiling target.
- **Blocked-scope filtering**: When `_blocked_scope` is set, only samples matching the specified off-CPU subclass (I/O, lock, scheduling, generic, or all) are treated as experiment hits.
- **Dual delay staging**:
  - Off-CPU hits are materialized immediately into `local_delay_inc`, then flushed to `state->local_delay` at the end of the function.
  - On-CPU hits are staged into `state->delayed_local_delay`, deferred until the thread resumes CPU execution.
- **Callchain recording**: On Linux, matching samples also feed into the `hit_callchains::Table` system (a COZ-original feature), so off-CPU hits appear in callchain output.

Call sites:
- `post_block()` — invoked when a thread exits a blocking region
- `catch_up()` — invoked to synchronize delays before unlocking dependents

### 4. Blocked-Scope Experiment Mode (`--blocked-scope`)

Instead of selecting a source line as the experiment target, BCOZ can target an entire off-CPU subclass:

| Flag | Scope | Description |
|------|-------|-------------|
| `o` | On-CPU | Only on-CPU samples count as hits |
| `i` | I/O | Blocking I/O events |
| `l` | Lock | Lock contention events |
| `s` | Scheduling | Involuntary context switches |
| `b` | Blocked | Generic blocking (sleep, epoll, futex) |
| `a` | All off-CPU | Union of all off-CPU subclasses |

When `_blocked_scope` is active, the experiment output uses a named scope (e.g., `"IO"`, `"LOCK"`) instead of a source file:line reference:

```json
{"type":"experiment","selected":"IO","speedup":0.3,"duration":50000000,...}
```

Six atomic counters track per-subclass hit rates: `_blocked_all`, `_blocked_io`, `_blocked_lock`, `_blocked_sched`, `_blocked_blocked`, `_blocked_oncpu`.

### 5. Relational Profiling (`--based-line`, `--based-blocked`)

BCOZ introduced a second, independent experiment channel that runs in parallel with the primary experiment. This measures the causal impact of optimizing one code region on the performance of a designated "based" (baseline) target.

Merged fields:
- `_based_line` — target source line for relational profiling
- `_based_blocked` — off-CPU subclass as relational target instead of a line
- `_based_fixed_delay_size` / `_based_delay_size` — virtual speedup for the based channel
- `_based_global_delay` — global delay accumulator for the based channel
- `state->based_local_delay` — per-thread delay for the based channel

`based_match_line()` scans the callchain for the based target line (separate from `match_line()`, which targets the selected experiment line).

Output includes both speedup values:
```json
{"type":"experiment","selected":"src/foo.cpp:42","speedup":0.25,
 "based_target":"IO","based_speedup":0.15,...}
```

### 6. `pre_block()` / `post_block()` Race Condition Fix

BCOZ fixed a race condition in the blocking annotation API where experiment boundaries could be crossed between `pre_block()` and `post_block()` calls:

- `pre_block()` now captures `state->ex_count = ex_count.load()` to record which experiment the blocking event belongs to
- `post_block()` calls `process_blocked_samples()` immediately, ensuring off-CPU samples are processed before the thread re-enters on-CPU accounting
- `state->in_wait` coexists with COZ's original `state->is_blocked` atomic — both are maintained for their respective consumers

### 7. Thread State Extensions

New per-thread fields added to `thread_state`:

| Field | Type | Purpose |
|-------|------|---------|
| `delayed_local_delay` | `size_t` | Staged on-CPU delay, flushed when thread resumes |
| `based_local_delay` | `size_t` | Per-thread delay for relational profiling |
| `process_samples` | `atomic<int>` | Concurrent-processing guard (0 = idle, 1 = active) |
| `ex_count` | `uint64_t` | Experiment counter for pre/post_block correlation |
| `last_sample_time` | `uint64_t` | Deduplication of stale out-of-order samples |
| `in_wait` | `bool` | Thread is inside a blocking region |
| `sync_local_with_global` | `atomic<bool>` | Synchronization flag for based-profiling path |

All original COZ fields (`is_blocked`, `_hit_callchain_table`, `local_delay` as `atomic<size_t>`) are preserved.

### 8. Perf Event Configuration Change

The merged COZ unconditionally enables off-CPU-aware sampling, matching BCOZ's default behavior:

```cpp
pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_CALLCHAIN
               | PERF_SAMPLE_WEIGHT | PERF_SAMPLE_TIME;
pe.exclude_kernel = 0;            // Include kernel samples (syscall-level blocking)
pe.exclude_callchain_kernel = 0;  // Include kernel frames in callchains
pe.wakeup_events = SampleBatchSize;  // = 1 (immediate processing)
```

This is not gated behind `_blocked_scope`. Even in default mode (no `--blocked-scope`), the profiler captures off-CPU samples and classifies them. The `_blocked_scope` and `_based_blocked` flags only affect how the experiment *interprets* these samples, not whether they are *collected*.

### 9. `add_delays()` Dual-Channel Support

The delay synchronization function now maintains two parallel delay channels:
1. **Primary channel**: `state->local_delay` ↔ `_global_delay` (existing COZ behavior)
2. **Based channel**: `state->based_local_delay` ↔ `_based_global_delay` (new)

Both channels follow the same ahead/behind logic:
- Thread ahead of global → push global forward
- Thread behind global by >100μs → pause thread via `wait()` to catch up

The `in_wait` flag prevents recursive delay injection on already-blocked threads.

### 10. CLI and Environment Variables

New arguments added to the `coz run` subcommand:

```
--blocked-scope {o,a,b,i,l,s}   → COZ_BLOCKED_SCOPE
--based-line FILE:LINE           → COZ_BASED_LINE
--based-blocked {a,b,i,l,s}     → COZ_BASED_BLOCKED
--based-speedup 0-100            → COZ_BASED_SPEEDUP
```

### 11. Export Symbol Compatibility

New C export symbols for BCOZ's custom blocking API:

```cpp
extern "C" void _coz_post_block_0();   // post_block(false)
extern "C" void _coz_post_block_1();   // post_block(true)
extern "C" void _coz_catch_up();       // catch_up()
```

These coexist with COZ's existing exports (`_coz_pre_block`, `_coz_post_block(int)`, `_coz_add_delays`).

---

## What COZ Already Had (Not from BCOZ)

These features existed in COZ before the merge and were preserved throughout:

### 1. Warmup Delay System

COZ supports a configurable warmup period (`--warmup <seconds>` / `COZ_WARMUP`) that delays experiment selection until the target program reaches steady state. The profiler thread sleeps for `_warmup_delay_ns` before starting the experiment loop, and all sample processing paths (`process_samples()`, `process_blocked_samples()`) skip records collected before `_warmup_complete` becomes true.

BCOZ had no warmup support — experiments could begin during program initialization, sampling startup transients.

### 2. Lock-Free Per-Thread Callchain Accumulation

COZ implemented a dedicated `hit_callchains` subsystem (`hit_callchains.h` / `hit_callchains.cpp`) that collects and aggregates call stacks at sample points:

- **Per-thread `hit_callchains::Table`**: A 128-bucket open-addressing hash table stored in each thread's state. Raw instruction pointers are hashed and stored without symbol resolution during sampling (hot path).
- **Deferred symbol resolution**: `build_callchain_string_from_bucket()` resolves addresses to `file:line` strings only at experiment collection time (cold path), avoiding `memory_map` lookups on the sampling fast path.
- **Lock-free concurrent writes**: An atomic `writers` counter allows multiple threads to record hits simultaneously; the reader (`collect_table()`) spin-waits for `writers == 0` before collecting.
- **Callchain output in JSON**: Experiment records include a `hit_callchains` array with chain strings and hit counts.

During the merge, a bug fix was applied: `build_callchain_string_from_bucket()` previously discarded the entire callchain when the leaf IP could not be resolved (common for JIT code, vDSO, stripped libraries). This was changed to use an `addr_fallback()` that generates `[unknown@0x<addr>]` placeholders, preserving chain structure and sample counts.

BCOZ had no per-thread callchain accumulation. It recorded callchains inline during sample processing but did not aggregate them per-experiment.

### 3. JSON Output Format

COZ defaults to JSON Lines output (`{"type":"startup",...}\n{"type":"experiment",...}\n...`), which is machine-parseable and supports structured fields like callchain arrays. A `--legacy-format` flag falls back to the tab-separated text format that BCOZ used exclusively.

### 4. DWARF 5 Support

COZ's `inspect.cpp` (via libelfin) transparently handles DWARF 2 through DWARF 5 line tables. DWARF 5 changed file indexing to start at 0 instead of 1, which COZ accommodates. BCOZ was only tested against DWARF 4 and earlier.

### 5. macOS Support (kperf-based Sampling)

COZ added macOS support using the `kperf` framework as an alternative to Linux `perf_event_open`. This includes:
- Timer-based sampling via `kdebug`
- `DYLD_INSERT_LIBRARIES` preloading
- Platform-specific delay application with overshoot correction
- `process_all_samples()` / `apply_pending_delays()` macOS-specific paths

BCOZ was Linux-only. The merged codebase gates blocked-samples features behind `#ifndef __APPLE__` so macOS support is not regressed.

### 6. LLM-Powered Optimization Advisor

COZ's viewer includes an AI-powered code optimization feature:
- HTTP endpoints for streaming suggestions (`/optimize`, `/llm-config`)
- Support for multiple LLM providers: Anthropic Claude, OpenAI GPT, Amazon Bedrock, Ollama (local)
- A system prompt that teaches the LLM causal profiling principles
- Linear regression analysis of speedup curves to identify optimization priority
- Streaming NDJSON responses rendered in the viewer UI

### 7. Enhanced Viewer

The web viewer (`viewer/`) includes:
- D3.js-based speedup curve visualization with linear regression slopes
- LLM provider selection UI with API key management
- Color-coded causal impact interpretation (red = contention, green = high priority)
- Cookie and localStorage persistence for settings and model caches

### 8. Source Scope Filtering

`COZ_FILTER_SYSTEM` / `memory_map::build(..., include_system)` controls whether system library source lines are included in experiment selection. This prevents the profiler from selecting lines in libc or libstdc++ as experiment targets.

### 9. Experiment Quality Filtering

COZ applies a `min_delta >= ExperimentTargetDelta` threshold before emitting experiment data, filtering out experiments where the selected line received too few samples to produce meaningful results.

---

## Advantages of Merged COZ over Standalone BCOZ

| Dimension | BCOZ | Merged COZ |
|-----------|------|------------|
| **Off-CPU profiling** | ✅ Full support | ✅ Full support (identical) |
| **Warmup period** | ❌ None | ✅ Configurable startup delay avoids sampling transients |
| **Callchain collection** | ❌ Inline only, no aggregation | ✅ Lock-free per-thread Table with deferred resolution and unknown-address fallback |
| **Output format** | Text only (tab-separated) | JSON Lines (default) + legacy text fallback |
| **DWARF support** | DWARF 2–4 | DWARF 2–5 |
| **macOS support** | ❌ Linux only | ✅ kperf-based sampling (off-CPU features gracefully disabled) |
| **LLM advisor** | ❌ None | ✅ Multi-provider streaming optimization suggestions |
| **Viewer** | Basic D3.js | Enhanced with regression analysis, LLM UI, persistence |
| **Source filtering** | ❌ None | ✅ System library exclusion via `COZ_FILTER_SYSTEM` |
| **Experiment quality** | All experiments emitted | Filtered by `ExperimentTargetDelta` threshold |
| **Leaf resolution failure** | Entire callchain discarded | Preserved with `[unknown@0x<addr>]` fallback |
| **local_delay type** | `size_t` (non-atomic) | `atomic<size_t>` (safer for signal handler contexts) |
| **pre/post_block race fix** | ✅ Fixed with `in_wait` + `ex_count` | ✅ Preserved, coexists with COZ's `is_blocked` |
| **Relational profiling** | ✅ `--based-line/blocked/speedup` | ✅ Preserved with JSON output support |
| **Blocked-scope experiments** | ✅ `--blocked-scope` | ✅ Preserved with JSON output support |

---

## Merge Strategy Summary

The merge followed an **incremental, additive** approach:

1. **COZ as the base**: All existing COZ code, features, and test infrastructure were preserved as-is. No COZ-original capability was removed or regressed.

2. **BCOZ features layered in**: Off-CPU classification, weighted samples, `process_blocked_samples()`, blocked-scope experiments, relational profiling, and the pre/post_block race fix were added to the existing COZ codebase.

3. **Conflict resolution favored COZ conventions**:
   - `local_delay` remains `atomic<size_t>` (COZ) rather than plain `size_t` (BCOZ)
   - `is_blocked` (COZ) coexists with `in_wait` (BCOZ), both maintained
   - JSON output is default; BCOZ's text-only format available via `--legacy-format`
   - macOS paths are untouched; BCOZ features are `#ifndef __APPLE__` guarded

4. **Bug fixes applied during merge**:
   - `build_callchain_string_from_bucket()` no longer discards chains on leaf resolution failure
   - `pre_block()` / `post_block()` race condition fix from BCOZ preserved

5. **No dead code imported**: BCOZ residual declarations without implementations (e.g., `blocked_samples_ready()`, `samples_ready_empty()`, `start_process()`) were not carried over.

---

## File Change Summary

| File | Changes |
|------|---------|
| `libcoz/perf.h` | Added off-CPU misc flags, `sample::weight`, classification methods, `get_weight()` |
| `libcoz/perf.cpp` | Added `get_weight()` implementation, weight support in `locate_field<>()` |
| `libcoz/thread_state.h` | Added BCOZ fields (`delayed_local_delay`, `based_local_delay`, `process_samples`, `ex_count`, `last_sample_time`, `in_wait`, `sync_local_with_global`) |
| `libcoz/profiler.h` | Extended `startup()` signature, added blocked counters, based-profiling fields, `ex_count`, new method declarations |
| `libcoz/profiler.cpp` | Added `process_blocked_samples()`, `based_match_line()`, updated `pre_block()` / `post_block()` / `catch_up()` / `add_delays()` / `process_samples()` / experiment loop output |
| `libcoz/libcoz.cpp` | Added env var parsing (`COZ_BLOCKED_SCOPE`, `COZ_BASED_*`), export symbols (`_coz_post_block_0/1`, `_coz_catch_up`) |
| `libcoz/hit_callchains.cpp` | Bug fix: `addr_fallback()` for unresolved leaf/frame addresses |
| `coz` (Python CLI) | Added `--blocked-scope`, `--based-line`, `--based-blocked`, `--based-speedup` arguments |
