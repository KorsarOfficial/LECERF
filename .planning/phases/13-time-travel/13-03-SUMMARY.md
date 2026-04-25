---
phase: 13-time-travel
plan: 03
subsystem: core/tt
tags: [replay, event-log, bsearch, determinism, tdd]
dependency_graph:
  requires:
    - phase: 13-01
      provides: ev_log_t, tt_periph_t, run_steps_full_g, uart_inject_rx, nvic_set_pending
    - phase: 13-02
      provides: snap_blob_t, snap_save, snap_restore
  provides:
    - ev_log_seek O(log n) lower-bound by cycle
    - tt_inject_event dispatcher (UART_RX, IRQ_INJECT, ETH_RX-deferred)
    - run_until_cycle monotone forward runner with event drain
    - tt_replay snap_restore + run_until_cycle under g_replay_mode
  affects: [13-04-tt-core, 13-05-firmware-integration]
tech-stack:
  added: []
  patterns:
    - bsearch lower-bound for event log seek O(log n)
    - replay dispatcher pattern (inject event by type at cycle boundary)
    - monotone loop with watchdog on stuck CPU
    - g_replay_mode gate wraps snap_restore + run_until_cycle
key-files:
  created:
    - tests/test_tt_replay.c
  modified:
    - include/core/tt.h
    - include/core/run.h
    - src/core/tt.c
    - src/core/run.c
    - tests/test_tt_determinism.c
    - tests/CMakeLists.txt
key-decisions:
  - "run_until_cycle placed in src/core/run.c (owns run_steps_full_g), declared in tt.h (has ev_t/tt_periph_t types); run.h uses struct forward decls to avoid circular include"
  - "tt_inject_event (void)c (void)bus: cpu and bus not needed for current event types; kept for future extensibility"
  - "ev_log_seek null guard added (null lg returns 0, safe for empty log case)"
  - "test_tt_determinism SRAM_SIZE renamed to TEST_SRAM_SIZE to avoid redefinition with tt.h SRAM_SIZE"
patterns-established:
  - "Replay loop: while cycles < target { drain events; compute gap; run_steps_full_g(gap); watchdog }"
  - "tt_replay = g_replay_mode=true + snap_restore + ev_log_seek(start_cycle) + run_until_cycle + restore prev mode"
duration: 45min
completed: 2026-04-26
---

# Phase 13 Plan 03: Replay Engine Summary

**ev_log_seek O(log n) bsearch + tt_inject_event 3-case dispatcher + run_until_cycle monotone loop + tt_replay deterministic wrapper — TT-05 byte-equal across replays verified**

## Performance

- **Duration:** 45 min
- **Completed:** 2026-04-26
- **Tasks:** 2
- **Files modified:** 6 (+ 1 created)

## Accomplishments

- ev_log_seek: full O(log n) lower-bound bsearch (null-safe, was already in 13-01 but cleaned up)
- tt_inject_event: EVENT_UART_RX routes to uart_inject_rx, EVENT_IRQ_INJECT to nvic_set_pending, EVENT_ETH_RX deferred
- run_until_cycle: monotone forward runner — drains event log at cycle boundaries, watchdog on stuck CPU, overshoot <= one ARM cycle
- tt_replay: snap_restore + run_until_cycle wrapped in g_replay_mode=true/prev, returns bool
- TT-05 verified: two independent tt_replay(snap, log, 10000) produce byte-equal cpu_t and SRAM

## Task Commits

1. **Task 1: ev_log_seek + tt_inject_event + run_until_cycle + tt_replay** - `667ade1` (feat)
2. **Task 2: test_tt_replay TT-05 byte-equal test** - `8006ecf` (test)

## Files Created/Modified

- `include/core/tt.h` - Added tt_inject_event, run_until_cycle, tt_replay declarations (replay engine section)
- `include/core/run.h` - Added run_until_cycle declaration with struct forward decls for ev_t/tt_periph_t
- `src/core/tt.c` - Added tt_inject_event dispatcher, tt_replay wrapper; also contains snap (13-02) and ev_log (13-01)
- `src/core/run.c` - Added run_until_cycle implementation; added #include "core/tt.h"
- `tests/test_tt_replay.c` - TT-05 byte-equal test + monotone run_until_cycle check (117 lines)
- `tests/test_tt_determinism.c` - Fixed SRAM_SIZE redefinition (renamed local to TEST_SRAM_SIZE)
- `tests/CMakeLists.txt` - Added test_tt_replay target

## Decisions Made

- `run_until_cycle` in `run.c` (not `tt.c`) to keep it co-located with `run_steps_full_g` which it calls in a tight loop
- `run.h` uses `struct ev_s*` / `struct tt_periph_s*` forward decls to avoid including tt.h from run.h (would create a chain where tt.h -> run.h -> tt.h)
- Test uses static `snap_blob_t s_snap` (~263KB) and `static jit_t s_g1, s_g2` (~2MB each) to avoid Windows stack overflow

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] SRAM_SIZE macro redefinition in test_tt_determinism.c**
- **Found during:** Task 1 (build)
- **Issue:** 13-02 added `#define SRAM_SIZE (256u * 1024u)` to tt.h; test_tt_determinism.c had `#define SRAM_SIZE (64u << 10)` locally — compiler warning
- **Fix:** Renamed local define to `TEST_SRAM_SIZE` and updated usage
- **Files modified:** tests/test_tt_determinism.c
- **Committed in:** 667ade1 (Task 1 commit)

**2. [Rule 1 - Bug] cmake build directory missing Makefile (generator not configured)**
- **Found during:** Task 1 (build verification)
- **Issue:** cmake --build failed silently; CMakeCache.txt had CMAKE_MAKE_PROGRAM=NOTFOUND
- **Fix:** Re-ran cmake configure with explicit -G "MinGW Makefiles" and PATH including /c/msys64/mingw64/bin
- **Files modified:** None (cmake cache only)
- **Committed in:** N/A (environment issue)

---

**Total deviations:** 2 auto-fixed (1 macro conflict, 1 build env)
**Impact on plan:** Both minor. No scope creep. All plan objectives met.

## Issues Encountered

- Linter repeatedly stripped additions from run.c and tt.c during editing. Resolved by using Write tool for complete file rewrites and verifying file state before commits.
- snap_blob_t/snap_save/snap_restore (13-02) were in uncommitted working-tree state when 13-03 began. Committed them as prerequisite in the same Task 1 commit since they were required for tt_replay.

## Next Phase Readiness

- tt_replay operational: snap + log + target -> byte-equal cpu_t
- Ready for 13-04: tt_t struct, tt_create/free, tt_step_back, periodic snapshots
- run_until_cycle available for 13-04's tt_rewind (bsearch + snap_restore + run_until_cycle)

---
*Phase: 13-time-travel*
*Completed: 2026-04-26*

## Self-Check: PASSED

Files verified:
- tests/test_tt_replay.c: EXISTS
- include/core/tt.h: contains tt_inject_event, run_until_cycle, tt_replay
- src/core/run.c: contains run_until_cycle
- src/core/tt.c: contains tt_inject_event, tt_replay

Commits verified:
- 667ade1: p13.03 T1 — FOUND
- 8006ecf: p13.03 T2 — FOUND

Tests: 8/8 passing (decoder, executor, bus, memory, t32, tt_determinism, tt_snapshot, tt_replay)
