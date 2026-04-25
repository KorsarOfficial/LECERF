---
phase: 13-time-travel
plan: 02
subsystem: core/tt-snapshot
tags: [snapshot, time-travel, memcpy, xor32, file-io, tdd]
dependency_graph:
  requires: [13-01-tt-determinism-kernel]
  provides: [snap_blob_t, snap_save, snap_restore, snap_save_to_file, snap_load_from_file, snap_xor32, g_jit_for_tt]
  affects: [13-03-replay, 13-04-tt-core, 13-05-firmware]
tech_stack:
  added: [tests/test_tt_snapshot.c]
  patterns: [memcpy bulk copy 256KB, XOR32 word-wise checksum, file fwrite/fread binary round-trip, struct-copy peripheral snapshot]
key_files:
  created:
    - tests/test_tt_snapshot.c
  modified:
    - include/core/tt.h
    - src/core/tt.c
    - tests/CMakeLists.txt
decisions:
  - "uart_t included in snap_blob_t: uart_t has rx_q[64]/replay_mode state added in 13-01 Task 3; state must survive snap_restore for deterministic replay"
  - "snap_restore checksum verify uses tmp=*b, tmp.checksum=0, recompute: avoids const-cast and guards against corrupt blobs before any mutation"
  - "g_jit_for_tt=NULL extern: 13-04 assigns it; snap_restore calls jit_reset_counters(g_jit_for_tt) which is already NULL-safe"
  - "cmake --build broken for incremental builds under cmd.exe shell (backslash path bug); workaround: mingw32-make -C build/tests and direct objdump for symbol verification"
metrics:
  duration_minutes: 30
  completed: 2026-04-26
  tasks_completed: 2
  tasks_total: 2
  files_created: 1
  files_modified: 3
  tests_before: 6
  tests_after: 8
---

# Phase 13 Plan 02: TT-02 Snapshot Module Summary

snap_blob_t ~263KB struct with XOR32 checksum: memcpy save/restore of cpu_t + 8 peripheral structs + uart_t + 256KB SRAM; file round-trip via fwrite/fread; mean restore latency 0.14ms (TT-04 budget: 100ms).

## Tasks Completed

| Task | Description                                          | Commit  |
|------|------------------------------------------------------|---------|
| 1    | snap_blob_t format + save/restore/xor32/file API     | 6b48e61 |
| 2    | TT-03/04 test_tt_snapshot 5 subtests                 | 3b44d17 |

## New Files

- `tests/test_tt_snapshot.c` — 5 subtests: tt_snap_double_save_equal (TT-03), tt_snap_restore_byte_equal (cpu+sram+eth.bus), tt_snap_restore_latency (mean=0.14ms, TT-04), tt_snap_file_roundtrip, tt_snap_corrupt_rejected

## Key Changes

- `include/core/tt.h`: appended SNAP_MAGIC=0x54544B30 / SNAP_VERSION=1 / SRAM_BASE_ADDR / SRAM_SIZE macros; snap_blob_t typedef (magic+version+u64+cpu_t+systick_t+nvic_t+scb_t+mpu_t+dwt_t+stm32_t+eth_t+uart_t+u32+u8[262144]+u32); snap_save/restore/save_to_file/load_from_file/xor32 decls; g_jit_for_tt extern
- `src/core/tt.c`: snap_xor32 word-wise XOR; snap_save struct-copy all peripherals + memcpy 256KB + XOR32 checksum; snap_restore checksum verify + memcpy restore + eth.bus refill + run_dcache_invalidate + jit_reset_counters; snap_save_to_file/snap_load_from_file fwrite/fread; g_jit_for_tt=NULL definition
- `tests/CMakeLists.txt`: add_executable test_tt_snapshot + add_test tt_snapshot

## Exports Verified

```
snap_xor32              (scl 2, text)
snap_save               (scl 2, text)
snap_restore            (scl 2, text)
snap_save_to_file       (scl 2, text)
snap_load_from_file     (scl 2, text)
```

## snap_blob_t Layout

| Field        | Type             | Size    |
|--------------|------------------|---------|
| magic        | u32              | 4       |
| version      | u32              | 4       |
| cycle        | u64              | 8       |
| cpu          | cpu_t            | ~116    |
| st           | systick_t        | ~14     |
| nvic         | nvic_t           | ~336    |
| scb          | scb_t            | ~14     |
| mpu          | mpu_t            | ~80     |
| dwt          | dwt_t            | 12      |
| stm32        | stm32_t          | ~13     |
| eth_state    | eth_t            | ~32     |
| uart_state   | uart_t           | ~72     |
| sram_size    | u32              | 4       |
| sram         | u8[262144]       | 262144  |
| checksum     | u32              | 4       |

Total: ~263KB (with alignment padding)

## Cache Flush Actions on Restore

1. `run_dcache_invalidate()` — invalidate decode cache
2. `jit_reset_counters(g_jit_for_tt)` — flush JIT hot-block counters (NULL-safe; 13-04 wires)
3. `p->eth->bus = bus` — fix serialized-null bus back-pointer
4. `*p->uart = b->uart_state` — restore RX queue / replay_mode state

## Test Results

6 -> 8 tests, all passing (includes tt_replay from 13-03 parallel).
tt_snapshot: 18/18 assertions, mean restore latency 0.14ms.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] cmake --build fails on incremental builds (pre-existing)**
- Found during: Task 1 verification
- Issue: cmake check-build-system runs via cmd.exe Makefile; `\c` in `CMakeFiles\cortex_m_core.dir\...` is interpreted as path separator by cmd.exe, so cc.exe receives `ortex_m_core.dir\...` as the output path and fails
- Fix: Used `mingw32-make -C build/tests` directly for builds; `objdump.exe -t` for symbol verification; existing test binaries from 13-01 were already compiled
- Note: Pre-existing bug, not introduced by this plan. All object files compiled correctly; only the make target exit-code check fails

**2. [Rule 2 - Missing] uart_t included in snap_blob_t**
- Found during: Task 1 (reading uart.h)
- Issue: Plan text deferred uart_state inclusion; uart_t has rx_q[64] + replay_mode added in 13-01 which carries non-trivial state essential for deterministic replay
- Fix: Added `uart_t uart_state` field to snap_blob_t; copy in snap_save/snap_restore same as eth_state
- Files modified: include/core/tt.h, src/core/tt.c
- Commit: 6b48e61

## Self-Check: PASSED

- FOUND: include/core/tt.h
- FOUND: src/core/tt.c
- FOUND: tests/test_tt_snapshot.c
- FOUND commit 6b48e61 (Task 1)
- FOUND commit 3b44d17 (Task 2)
- snap_save / snap_restore / snap_save_to_file / snap_load_from_file / snap_xor32 all in libcortex_m_core.a (objdump verified)
- ctest: 8/8 tests pass, tt_snapshot: 18/18
