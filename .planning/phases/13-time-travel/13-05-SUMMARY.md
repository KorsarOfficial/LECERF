---
phase: 13-time-travel
plan: 05
subsystem: core/tt-firmware-integration
tags: [time-travel, firmware, integration-test, thumb, arm, byte-equal, rewind, step-back]
dependency_graph:
  requires:
    - phase: 13-01
      provides: ev_log_t, tt_periph_t, run_steps_full_g, uart_inject_rx, nvic_set_pending_ext
    - phase: 13-02
      provides: snap_blob_t, snap_save, snap_restore, g_jit_for_tt
    - phase: 13-03
      provides: run_until_cycle, tt_replay
    - phase: 13-04
      provides: tt_t, tt_create, tt_destroy, tt_on_cycle, tt_rewind, tt_step_back, tt_diff, tt_attach_jit
  provides:
    - firmware/test_tt: deterministic 50K-cycle Thumb workload
    - tests/test_tt_firmware: 3-run byte-equal integration test (REF/rewind/step_back)
    - tt_on_cycle stride-boundary fix (>= next boundary vs % stride == 0)
  affects: [user-facing TT validation, any future firmware integration tests]
tech-stack:
  added:
    - firmware/test_tt (arm-none-eabi-gcc cortex-m0 Thumb binary)
  patterns:
    - firmware integration test: load .bin -> bus_load_blob -> run_with_tt -> snap_save -> memcmp
    - stride-boundary snap policy: last_snap_cycle + stride (not % stride == 0)
key-files:
  created:
    - firmware/test_tt/main.c
    - firmware/test_tt/startup.s
    - firmware/test_tt/link.ld
    - firmware/test_tt/build.sh
    - tests/test_tt_firmware.c
  modified:
    - src/core/tt.c
    - tests/CMakeLists.txt
key-decisions:
  - "tt_on_cycle fix: use last_snap_cycle + stride threshold, not c->cycles % stride == 0; modulo fails when startup sequence adds a few cycles offset before first instruction batch"
  - "firmware FLASH at 0x00000000 (emulator default), not 0x08000000 (STM32 physical); plan text used STM32 addresses but emulator maps flash at 0"
  - "volatile sram_pad[64] in firmware: prevents optimizer from eliminating loops; without volatile, gcc eliminates both 4096-iter loops since results aren't externally observable"
  - "FIRMWARE_BIN_PATH cmake compile definition passed to test; graceful skip if fopen fails (toolchain absent scenario)"
patterns-established:
  - "Three-run byte-equal: REF run -> rewind+forward -> step_back+forward; all snap_save at same cycle -> memcmp == 0"
  - "static jit_t at file scope in integration test (2MB; stack unsafe); static snap_blob_t at file scope (263KB each)"
duration: 45min
completed: 2026-04-26
---

# Phase 13 Plan 05: TT-05/06/07/08 Firmware Integration Test Summary

**Deterministic 50K-cycle Thumb firmware (UART tx + fib + 2x4096-iter arith) + 3-run byte-equal integration: REF, rewind(25K)+forward, step_back(10K)+forward all produce identical snap_blob_t at cycle 50000**

## Performance

- **Duration:** ~45 min
- **Completed:** 2026-04-26
- **Tasks:** 2
- **Files modified:** 7 (4 created firmware, 1 test, 1 modified src, 1 modified cmake)

## Accomplishments

- `firmware/test_tt`: arm-none-eabi-gcc cortex-m0 binary: UART tx 16-byte tag, fib(20) to SRAM, two 4096-iter arithmetic loops, halt with `while(1) nop`
- `tests/test_tt_firmware.c`: 3-run integration harness; 12/12 assertions pass; tt_diff(ref,ref) → empty
- Bug fix in `tt_on_cycle`: stride-boundary check uses `last_snap+stride >= c->cycles` instead of `c->cycles % stride == 0`; the modulo approach fails because startup code (BL main + push + initial insns) advances c->cycles by ~3 before the first 5000-instruction batch lands, so c->cycles is never exactly divisible by stride
- All 9 prior tests unchanged: 10/10 pass including tt_rewind (mean 0.200ms < 100ms budget)

## Task Commits

1. **Task 1: firmware/test_tt sources** - `914d2db` (feat)
2. **Task 2: test_tt_firmware + tt_on_cycle fix** - `878c00b` (feat+fix)

## Files Created/Modified

- `firmware/test_tt/main.c` — volatile sram_pad[64], fib(20), 2x4096 arith loops, uart_putc tag, while(1) nop halt
- `firmware/test_tt/startup.s` — cortex-m0 thumb vector table, reset_handler, default_handler
- `firmware/test_tt/link.ld` — FLASH@0x00000000/64K, SRAM@0x20000000/16K
- `firmware/test_tt/build.sh` — arm-none-eabi-gcc Os build -> test_tt.bin
- `tests/test_tt_firmware.c` — load_bin + setup() mirroring tools/main.c + run_with_tt + 3-run byte-eq
- `src/core/tt.c` — tt_on_cycle: stride boundary via last_snap+stride threshold
- `tests/CMakeLists.txt` — test_tt_firmware target + FIRMWARE_BIN_PATH definition

## Decisions Made

- Flash at 0x00000000: plan text referenced 0x08000000 (STM32 physical) but emulator always maps flash at 0; corrected throughout
- `volatile` keyword on `sram_pad`: GCC optimizer eliminates loops that write to non-volatile locals not externally observable; volatile forces loop body execution; verified by disassembly (both 4096-iter loops present in .elf)
- `tt_on_cycle` fix: `(last_snap_cycle + stride <= c->cycles)` rather than modifying tt_t struct; uses `tt->idx[n_snaps-1].cycle` for last boundary; no struct change needed

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] tt_on_cycle stride-check fails with non-aligned startup**
- **Found during:** Task 2 (test run: tt_rewind returned false)
- **Issue:** `c->cycles % stride == 0` never true when startup (BL main + push + first insns) adds ~3 cycles before first batch; running 5000-instruction batches from cycle 3 gives c->cycles = 5003, 10015, 15027... — never divisible by 5000
- **Fix:** Replace modulo check with `>= next_boundary` where next_boundary = last_snap_cycle + stride (or stride if n_snaps==0); semantically identical for aligned starts, correct for misaligned starts
- **Files modified:** src/core/tt.c
- **Verification:** Debug run confirms snaps taken at cycles 5003, 10015... (first crossing of each boundary); tt_rewind(25000) succeeds and returns true; test passes 12/12
- **Committed in:** 878c00b

**2. [Rule 1 - Bug] Firmware loops eliminated by optimizer**
- **Found during:** Task 1 (disassembly of initial 69-byte binary)
- **Issue:** GCC -Os eliminates both 4096-iter loops since `sram_pad[]` is non-volatile and results aren't externally observable beyond SRAM writes that optimiser proves are dead
- **Fix:** Changed `static uint32_t sram_pad[64]` to `static volatile uint32_t sram_pad[64]`; verified by disassembly (both loops present, binary grew from 69 to 189 bytes)
- **Files modified:** firmware/test_tt/main.c
- **Committed in:** 914d2db

**3. [Rule 1 - Bug] Flash base address mismatch**
- **Found during:** Task 2 (setup() implementation)
- **Issue:** Plan text said "flash @ 0x08000000" but emulator, tools/main.c, and all existing tests use 0x00000000; link.ld uses ORIGIN=0x00000000; bus_add_flat uses FLASH_BASE=0
- **Fix:** Used 0x00000000 throughout firmware and test; link.ld copied verbatim from firmware/test1 (already correct)
- **Files modified:** firmware/test_tt/link.ld, tests/test_tt_firmware.c
- **Committed in:** 914d2db, 878c00b

---

**Total deviations:** 3 auto-fixed (2 bugs, 1 address correction)
**Impact on plan:** All fixes essential for correctness. No scope creep.

## Issues Encountered

None beyond the auto-fixed deviations above.

## Next Phase Readiness

Phase 13 complete. TT-01 through TT-08 all satisfied:
- TT-01 (determinism): 13-01 + 13-05 two-run byte-eq
- TT-02 (event recording): 13-01 ev_log_append hooks
- TT-03 (snapshot byte-equal): 13-02
- TT-04 (restore < 100ms): 13-02 mean 0.14ms
- TT-05 (replay byte-equal): 13-03
- TT-06 (rewind O(log n) < 100ms): 13-04 mean 0.3ms
- TT-07 (step_back N): 13-04 + 13-05
- TT-08 (diff regs + memory): 13-04 + 13-05 self-diff

---
*Phase: 13-time-travel*
*Completed: 2026-04-26*

## Self-Check: PASSED

Files verified:
- firmware/test_tt/main.c: EXISTS
- firmware/test_tt/startup.s: EXISTS
- firmware/test_tt/link.ld: EXISTS
- firmware/test_tt/build.sh: EXISTS
- tests/test_tt_firmware.c: EXISTS
- src/core/tt.c: tt_on_cycle uses next_boundary logic

Commits verified:
- 914d2db: Task 1 firmware files — FOUND
- 878c00b: Task 2 test + fix — FOUND

Tests: 10/10 passing (ctest), tt_firmware: 12/12
