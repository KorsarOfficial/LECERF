---
phase: 13-time-travel
plan: "06"
subsystem: time-travel
tags: [eth, recording, replay, side-blob, TT-02]
dependency_graph:
  requires: [13-04, 13-05]
  provides: [TT-02-complete, eth_inject_rx, tt_record_eth_rx, eth-side-blob-store]
  affects: [src/core/tt.c, include/core/tt.h, src/periph/eth.c, include/periph/eth.h]
tech_stack:
  added: []
  patterns: [side-blob store (runtime-only, not in snap), ev_t payload reuse as u32 frame_id]
key_files:
  created: [tests/test_tt_eth_replay.c]
  modified: [include/core/tt.h, src/core/tt.c, include/periph/eth.h, src/periph/eth.c, tests/CMakeLists.txt]
decisions:
  - "Side-blob frames[] live in tt_t runtime only (NOT snap_blob_t); ev_t.payload reused as u32 frame_id; sizeof(ev_t)==16 static_assert preserved"
  - "eth_inject_rx is dual-use: record-time (tt_record_eth_rx wraps it implicitly via replay path) + replay-time (tt_inject_event EVENT_ETH_RX)"
  - "tt_inject_event reads g_tt->frames[id] directly; consistent with tt_record_irq/uart_rx pattern of using g_tt without threading tt through call sites"
metrics:
  duration: 25min
  completed: 2026-04-26
  tasks_completed: 2
  files_created: 1
  files_modified: 5
---

# Phase 13 Plan 06: TT-02 ETH Gap Closure Summary

One-liner: ETH RX recording via tt_record_eth_rx + fixed-cap side-blob store (256 x 1600B in tt_t) + EVENT_ETH_RX dispatch to eth_inject_rx, closing TT-02 with two-replay byte-eq test.

## What Was Built

### Task 1: side-blob store + eth_inject_rx + tt_record_eth_rx

**include/core/tt.h:**
- Added `TT_ETH_MAX 256u`, `TT_ETH_MTU 1600u` constants
- Added `eth_frame_t` struct: `u32 len` + `u8 buf[TT_ETH_MTU]`
- Extended `tt_t` with `eth_frame_t* frames` and `u32 n_frames` (after `ev_log_t log`)
- Added `tt_record_eth_rx(u64 cycle, const u8* frame, u32 len)` prototype
- `_Static_assert(sizeof(ev_t) == 16)` still holds; `snap_blob_t` shape unchanged

**src/core/tt.c:**
- `tt_record_eth_rx`: guards `g_tt && !g_replay_mode && fr && ln`; clamps to TT_ETH_MTU; `id = n_frames++`; memcpy into `frames[id]`; `ev_log_append(EVENT_ETH_RX, id)`; rolls back `n_frames` on log failure
- `tt_create`: `calloc(TT_ETH_MAX, sizeof(eth_frame_t))` after ev_log_init; fails back through `tt_destroy` on OOM
- `tt_destroy`: `free(tt->frames)` before existing `free(tt->snaps)`
- `tt_inject_event EVENT_ETH_RX`: replaced no-op `break` with lookup `g_tt->frames[id]` -> `eth_inject_rx(p->eth, fr->buf, fr->len)`

**include/periph/eth.h:**
- Added `eth_inject_rx(eth_t* e, const u8* frame, u32 len)` prototype

**src/periph/eth.c:**
- Implemented `eth_inject_rx`: guards `e && e->bus && e->rx_addr && fr && ln`; `bus_write` loop byte-by-byte into `rx_addr + i`; sets `rx_len = ln`; `status |= 0x3u`

### Task 2: TT-02 ETH replay byte-eq integration test

**tests/test_tt_eth_replay.c:**
- `tt_eth_replay_byte_equal`: `tt_create(10000, 4)` -> `snap_save` at cycle 0 -> 64B deterministic frame (`i ^ 0xA5`) -> `tt_record_eth_rx(5000, fr, 64)` -> asserts `fid==0`, `n_frames==1`, `log.buf[0].type==EVENT_ETH_RX`, `log.buf[0].payload==0`, `log.buf[0].cycle==5000` -> two independent `tt_replay` calls -> verifies frame in SRAM at `rx_addr`, `rx_len==64`, `status&0x3==0x3`, `memcmp(cpu1,cpu2)==0`, `memcmp(sram1,sram2)==0` -> replay-mode guard test
- `tt_eth_capacity_guard`: fills all `TT_ETH_MAX` slots; asserts `TT_ETH_MAX+1`-th returns `0xFFFFFFFFu`

**tests/CMakeLists.txt:** Added `test_tt_eth_replay` target and `tt_eth_replay` ctest.

## Verification Results

- `cmake --build build`: clean, no new warnings (pre-existing nvic.c sign-conversion warning unchanged)
- `ctest --test-dir build`: 11/11 pass (10 pre-existing + 1 new `tt_eth_replay`)
- `firmware/run_all.sh`: 14/14 pass
- `objdump` confirms `tt_record_eth_rx` and `eth_inject_rx` exported from `libcortex_m_core.a`
- `_Static_assert(sizeof(ev_t) == 16)` compiles (ev_t shape unchanged)
- `snap_blob_t` size unchanged (frames live only in tt_t at runtime)

## Deviations from Plan

None - plan executed exactly as written.

## TT-02 Closure Confirmation

| Criteria | Status |
|----------|--------|
| src/periph/eth.c contains eth_inject_rx | WIRED |
| EVENT_ETH_RX case dispatches to eth_inject_rx | WIRED |
| tt_record_eth_rx emits EVENT_ETH_RX with payload=frame_id | WIRED |
| tt_t carries frames[TT_ETH_MAX] side-blob store | DONE |
| ev_t 16B static_assert holds | PASS |
| snap_blob_t shape unchanged | PASS |
| Two tt_replay calls produce byte-equal cpu_t + SRAM | PASS |
| ctest count 10 -> 11, all green | PASS |

TT-02 is fully satisfied. All I/O events (UART RX, IRQ, ETH frame) are now recorded with cycle stamps and replayable byte-equal.
