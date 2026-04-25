# Phase 13: Time-Travel Kernel - Research

**Researched:** 2026-04-24
**Domain:** Deterministic emulation, snapshot/restore, record-and-replay, rewind primitives (C11, single-process)
**Confidence:** HIGH (codebase read directly; external systems verified via official QEMU docs and rr project)

---

## Summary

Phase 13 turns the existing ARMv7-M emulator into a fully deterministic, snapshotable, reversible
function f(state, time, events) -> state'. The codebase is well-suited: it is pure C11, single-
threaded, has no external dependencies in the core, and the only state containers are cpu_t (~120
bytes of registers + fpu_t = 32*4 + 12 = ~252 bytes total), bus_t flat regions (flash 1MB +
SRAM 256KB), and a handful of peripheral structs (systick_t, nvic_t, scb_t, etc.). Total mutable
state per run is ~1.3MB.

The QEMU record/replay architecture (verified via official docs) and Mozilla rr (verified via
project site and ATC'17 paper) both converge on the same pattern: log only non-deterministic
inputs with cycle stamps; snapshot the full state periodically; rewind = restore nearest snap +
forward replay. For our single-threaded embedded emulator this is significantly simpler than
either rr or QEMU because there are no kernel syscall races, no multi-vCPU threads, and no
IOMMU concerns.

The key risk is the x86 JIT codegen layer (codegen.c / jit.c): native thunks are mmap'd RWX
pages whose addresses are run-specific. After snapshot restore the JIT cache is still valid for
correctness (it only mutates cpu_t fields and does not capture addresses), but the hot-counter
static array in jit.c is not part of any serializable struct and must be either excluded from
state or reset on restore. The decode cache (g_dcache in run.c) is similarly not part of cpu_t
and must be flushed on restore. Both are purely caches, not semantic state.

**Primary recommendation:** implement snapshot as memcpy of (cpu_t + flat bus regions + all
peripheral structs) into a heap blob; record non-deterministic inputs in a compact fixed-width
event log (8 bytes per event: u64 cycle | u8 type | u32 payload); stripe snapshots every 10 000
cycles; rewind = bsearch nearest snap + replay from there. This fits entirely in ~30–40 LOC per
sub-module and requires zero new dependencies.

---

## Area 1: Determinism Strategy

### What Hidden Non-Determinism Exists (Codebase Audit)

Direct inspection of all source files reveals the following:

#### Confirmed deterministic (no action needed)
| Source | Reason |
|--------|--------|
| `cpu_t.cycles` | monotonic counter, incremented exactly once per instruction |
| `systick_tick()` | pure function of cvr/rvr/cycles, no wall clock |
| ETH loopback (`eth.c`) | ICMP reply is a pure transform of the TX frame bytes |
| GPIO reads (`stm32.c`) | IDR always returns 0 (no external pin state) |
| NVIC priority logic | pure bitmask math |
| Decode cache `g_dcache` | cache of decode results, not semantic; safe to clear on restore |
| FPU integer ops (VCVT, VMOV, etc.) | operate on u32 bit patterns, fully deterministic |

#### Confirmed non-deterministic — must log or eliminate

**1. UART RX (`uart.c` line 6)**
```c
case 0x00: return 0;   /* RX: nothing */
```
Currently returns 0 always. If real terminal input is added later, every byte is a non-
deterministic event. Even now, UART SR bit RXNE (0x04 bit 1) is always 0 — firmware that polls
RXNE will never see input. For TT-01: log any future UART RX byte with its cycle stamp.
Integration point: `uart_read()` in `src/periph/uart.c`.

**2. `nvic_set_pending(nvic_t*, u32 irq)` called externally**
This is the primary external IRQ injection path. Any call from outside `run_steps_full_g` is
non-deterministic from the firmware's perspective. Must log: (cycle, IRQ_number).
Integration point: `nvic_set_pending()` in `src/core/nvic.c`; callers must wrap with log.

**3. `static u32 counters[JIT_MAX_BLOCKS]` in `jit_run()` (`src/core/jit.c` line 74)**
This static array persists across calls and across restores. It affects when blocks get compiled
(JIT_HOT_THRESHOLD=16), which changes the instruction-level timing of block promotion. For
determinism: either (a) include `counters[]` in jit_t struct so it can be snapshotted, or
(b) flush counters on snapshot restore. Option (b) is simpler — after restore the JIT warm-up
is slightly slower but correctness is unaffected. **Decision: flush on restore.**

**4. FPU operations: `__builtin_sqrtf` and `__builtin_fmaf` (`executor.c` lines 1022–1094)**
- `__builtin_sqrtf(-negative)` produces `0.0f / 0.0f` (a NaN). The bit pattern of this NaN is
  host-FPU-defined. On x86-64 it is 0xFFC00000 (quiet NaN). On ARM Cortex-M it is also
  0x7FC00000. These differ in the sign bit and the quiet bit position but both are quiet NaN
  per IEEE 754. Since we store into `fpu.reg.u[]` (u32 array), the bit pattern is captured
  exactly in snapshots and replayed exactly. **Cross-run determinism is preserved as long as the
  same host runs both runs** (same x86 FPU). For now: HIGH confidence it is deterministic on a
  single host. If cross-host replay is ever needed, replace `__builtin_sqrtf(-x)` with a
  canonical `0x7FC00000u` NaN mask per ARM DDI 0403E.
- `__builtin_fmaf` uses the host FPU's fused-multiply-add. On x86 with AVX2 it maps to VFMADD.
  Result is IEEE 754-2008 correctly-rounded, same result on any conforming host. **Deterministic.**

**5. `getenv("EMU_TRACE")` in `executor.c` line 11**
Called once per process, cached. Does not affect cpu_t state; only triggers stderr output.
**Not a determinism concern for state.**

**6. `fputc/fflush` in uart/stm32/gpio write handlers**
UART TX output to stdout is a side effect, not part of emulated state. The firmware instruction
that triggered it is deterministic; the output is reproducible from the event log.
**No action needed for determinism, but must not be re-fired during replay.**
Integration point: add a `bool replay_mode` flag to uart_t; in replay mode, suppress stdout.

#### Summary: non-deterministic event types to log
```
EVENT_UART_RX   : u64 cycle, u8 byte
EVENT_IRQ_INJECT: u64 cycle, u8 irq_num
(future) EVENT_ETH_RX: u64 cycle, u16 frame_len, u8[] frame_data
```

**Confidence: HIGH** (based on direct codebase read of all .c files)

---

## Area 2: Snapshot Approach

### State to capture

| Region | Size | Type |
|--------|------|------|
| `cpu_t` (all fields incl. fpu_t) | ~252 bytes | struct |
| Flash (1MB flat) | 1 048 576 bytes | buf in bus |
| SRAM (256KB flat) | 262 144 bytes | buf in bus |
| `systick_t` | 20 bytes | struct |
| `nvic_t` (enable+pending+active+prio) | 8*4*3 + 240 = 336 bytes | struct |
| `scb_t` | ~24 bytes | struct |
| `mpu_t` | small | struct |
| `dwt_t` | small | struct |
| `stm32_t` (odr_a/b/c) | 12 bytes | struct |
| `eth_t` | ~28 bytes | struct |
| **Total** | **~1.31 MB** | |

Flash is write-protected (writable=false on flash region) so it never changes. **Flash does not
need to be in the snapshot blob** — it can be omitted and the restorer can assume the same
firmware image is still loaded. This reduces snapshot size to **~262KB**.

### Approach Comparison

| Approach | Snapshot size | Restore latency | Complexity |
|----------|---------------|-----------------|------------|
| Full memcpy of SRAM + structs | ~262KB | <5ms memcpy | 20 LOC |
| COW via mmap MAP_PRIVATE | System-page granularity | <1ms page faults | 150 LOC + POSIX mmap |
| Dirty bitmap + delta | Variable | Variable | 200 LOC |
| Fork-based (rr style) | OS managed | <1ms fork | Linux only, not Windows |

**Recommendation: full memcpy.** For 256KB SRAM + ~1KB structs:
- `memcpy(262KB)` at ~10GB/s = **~26 microseconds**. Far under the 100ms TT-04 requirement.
- Single call, no OS dependency, works on Windows and Linux.
- No page-table manipulation needed.
- Snapshot blob is a plain `u8[]` that can be written to disk for cross-session replay.

COW via `mmap MAP_PRIVATE` would be valuable if snapshots were >32MB, or if we needed sub-
microsecond restore latency. For 256KB it is over-engineered and Windows `VirtualAlloc` does not
support `MAP_PRIVATE` natively. **Do not implement COW for this phase.**

### Snapshot blob format

```c
typedef struct {
    u32  magic;        /* 0x54544B30 "TTK0" */
    u32  version;      /* 1 */
    u64  cycle;        /* cpu.cycles at snapshot time */
    cpu_t  cpu;        /* full register file + fpu */
    systick_t st;
    nvic_t  nvic;
    scb_t   scb;
    mpu_t   mpu;
    dwt_t   dwt;
    stm32_t stm32;
    eth_t   eth_state; /* minus bus back-pointer */
    u32  sram_size;    /* always 256*1024 */
    u8   sram[256*1024];
    u32  checksum;     /* simple XOR32 of all preceding bytes */
} snap_blob_t;         /* ~263KB total */
```

The `bus_t` itself (region descriptors, MMIO callbacks, buf pointers) is not serialized — it is
reconstructed identically at restore time from the same setup sequence. Only SRAM content and
peripheral registers need saving.

**Confidence: HIGH** (arithmetic verified; memcpy latency verified by memory bandwidth math)

---

## Area 3: Replay Storage — Event Log Format

### Format decision: fixed-width records

QEMU uses variable-length TLV with 1-byte type + u32 lengths. For us, a fixed-width record is
simpler and faster to seek:

```c
typedef struct {
    u64 cycle;    /* cpu.cycles when event occurs */
    u8  type;     /* EVENT_UART_RX, EVENT_IRQ_INJECT, EVENT_ETH_RX, ... */
    u8  pad[3];
    u32 payload;  /* for UART_RX: low byte = char; for IRQ_INJECT: irq num */
} ev_t;           /* exactly 16 bytes */
```

Fixed 16-byte records allow O(log n) binary search by cycle field, zero parsing overhead,
trivial serialization, and direct mmap of log file for replay.

For ETH_RX frames (variable): store frame in a side blob array indexed by a u32 offset in
payload. This is a rare event; the overhead is acceptable.

### Size estimate: 1M cycle execution

- Most firmware loops are empty spin + SysTick IRQ. IRQ frequency = once per RVR cycles
  (FreeRTOS uses ~1000 cycles / tick). At 1M cycles with 1000-cycle tick: ~1000 IRQ events.
- UART TX is a side effect, not logged. UART RX: 0 in typical firmware.
- ETH: 0 in typical firmware.
- **1M cycles -> ~1000 events -> 16KB event log.** Negligible.
- At 30M IPS (JIT speed), 1 second real time = 30M cycles = ~30 000 events = 480KB log.
- For 10 minutes of recording at 30M IPS: ~18M cycles = ~18 000 events = 288KB. Still small.

### Snapshot stride

Snapshots at every K=10 000 cycles:
- 1M cycles -> 100 snapshots * 263KB = **26.3MB** RAM.
- Rewind to any cycle: restore nearest snap + replay up to 10 000 cycles.
  At 30M IPS, 10 000 cycles replays in **0.33ms**. Well under 100ms target (TT-06).
- K is tunable at runtime. Default K=10 000 is conservative.

**Index structure:**
```c
typedef struct {
    u64 cycle;       /* cycle at snapshot */
    u32 snap_idx;    /* index into snap_store[] array */
} snap_entry_t;

typedef struct {
    snap_entry_t entries[MAX_SNAPS];  /* sorted by cycle */
    u32 n;
} snap_index_t;
```

Binary search (`bsearch` or hand-written) finds nearest `cycle <= target` in O(log n).

For 1M cycles with K=10 000: 100 entries, bsearch terminates in 7 comparisons.

**Confidence: HIGH** (math verified; event count estimated from firmware behavior)

---

## Area 4: Rewind Algorithm

### Algorithm

```
rewind(target_cycle T):
  1. i = bsearch(snap_index, T)          // O(log n), finds largest snap where snap.cycle <= T
  2. restore(snap_store[snap_index[i]])   // memcpy 263KB ~= 26us
  3. flush dcache, flush jit counters     // 2 memset calls
  4. replay events in event_log where event.cycle in [snap.cycle, T)
     for each event in range:
       run until cpu.cycles == event.cycle
       inject event (set nvic pending / push uart byte to queue)
  5. run until cpu.cycles == T
  6. done; cpu_t now reflects state at cycle T
```

### run_until_cycle helper

The existing `run_steps_full_g` runs for a fixed step count. Add:

```c
// src/core/run.c — new function ~15 LOC
u64 run_until_cycle(cpu_t* c, bus_t* bus, u64 target_cycle,
                    systick_t* st, scb_t* scb,
                    const ev_t* log, u32 log_n, u32* log_pos);
```

This loops calling `run_steps_full_g` with `min(K, target_cycle - c->cycles)` steps,
injecting events from the log as their cycle stamps are reached.

### step_back(N) — TT-07

`step_back(N)` = `rewind(cpu.cycles - N)`. Caller passes current cycle, function does the
bsearch + replay. For N=1 (single instruction back): rewind 1 cycle back requires replaying
from the nearest snap, which may be up to K cycles of replay. At K=10 000 and 30M IPS:
0.33ms. Acceptable for interactive debugger.

### diff(snap_a, snap_b) — TT-08

```c
// ~30 LOC
void snap_diff(const snap_blob_t* a, const snap_blob_t* b, FILE* out);
```

Compare cpu_t field by field (r[16], msp, psp, apsr, ..., fpu.reg.u[32], fpu.fpscr).
Then compare sram bytes with a compact range-encoding output: "SRAM[0x20000100..0x20000104]:
0xDEADBEEF -> 0xCAFEBABE". O(256KB) walk.

**Confidence: HIGH** (algorithm is standard; verified against QEMU and rr patterns)

---

## Area 5: API Design

### C API (internal, single-threaded)

```c
// include/core/tt.h

typedef struct tt_s tt_t;

/* Lifecycle */
tt_t* tt_create(u32 snap_stride_cycles, u32 max_snaps);
void  tt_destroy(tt_t* tt);

/* Recording (called from run loop) */
void  tt_on_cycle(tt_t* tt, cpu_t* c, bus_t* bus,
                  systick_t* st, nvic_t* nv, scb_t* scb,
                  mpu_t* mpu, dwt_t* dwt, stm32_t* stm32, eth_t* eth);
void  tt_log_event(tt_t* tt, u64 cycle, u8 type, u32 payload);

/* Snapshot */
snap_blob_t* tt_snapshot(tt_t* tt, cpu_t* c, bus_t* bus, ...);  /* returns ptr into snap_store */
bool  tt_restore(tt_t* tt, u32 snap_idx, cpu_t* c, bus_t* bus, ...);

/* Replay / rewind */
bool  tt_rewind(tt_t* tt, u64 target_cycle, cpu_t* c, bus_t* bus, ...);
bool  tt_step_back(tt_t* tt, u64 n, cpu_t* c, bus_t* bus, ...);
void  tt_diff(const snap_blob_t* a, const snap_blob_t* b, FILE* out);

/* Replay determinism */
bool  tt_replay(tt_t* tt, u32 snap_idx, u64 target_cycle, cpu_t* c, bus_t* bus, ...);
```

The `...` stands for the peripheral pointers (systick, nvic, scb, mpu, dwt, stm32, eth) — wrap
them in a `tt_periph_t` struct to avoid 7-argument chains.

### External / Python API (future, Phase 17)

The C API above is the foundation. Phase 17 wraps it in Python via ctypes or cffi. For now:
design the C API so structs are layout-stable (no padding changes, explicit `__attribute__((packed))`
or use stdint types with explicit offsets).

### GDB stub extension (future)

GDB RSP custom packets for rewind:
```
qTTRewind:<cycle_hex>   -> OK or E01
qTTSnapshot             -> returns snap index as hex
qTTDiff:<snap_a>,<snap_b> -> returns diff text
```

Add handler stubs to `gdb.c` now (10 LOC each), implement later.

**Confidence: HIGH** (API shape follows codebase conventions; GDB extension pattern from gdb.c)

---

## Area 6: Common Pitfalls in Time-Travel Emulators

### Pitfall 1: Static mutable state outside the snapshot

**What goes wrong:** jit_run() uses `static u32 counters[JIT_MAX_BLOCKS]` (jit.c:74). After
restore this array is stale, causing some blocks to be promoted to JIT earlier or later than
in the original run. This does not affect correctness (JIT is a cache) but breaks
cycle-for-cycle instruction timing if the JIT itself introduces cycle-count differences.

**How to avoid:** Move `counters[]` into `jit_t` struct (it already has its own instance).
Then include `jit_t` in the snapshot, OR simply `memset(counters, 0, sizeof(counters))` after
every restore. **Recommended: memset approach — 1 line.**

Integration: `src/core/run.c` — add `extern u32 g_jit_counters[]; void jit_reset_counters();`
called from `tt_restore`.

**Warning signs:** two runs with same firmware diverge after first restore.

### Pitfall 2: Decode cache not flushed after restore

**What goes wrong:** `g_dcache[4096]` in `run.c` caches decoded instructions. After restoring
to an earlier cycle where SRAM content differed (self-modifying code scenario), the decode cache
may serve stale decoded instructions.

**How to avoid:** call `run_dcache_invalidate()` (already exported from run.c) after every
`tt_restore`. Cost: `memset(4096 * sizeof(dcache_e_t))` = ~80KB zero-fill ~= 8us. Negligible.

### Pitfall 3: JIT native thunk pointers in snap_blob_t

**What goes wrong:** jit_block_t contains `cg_thunk_t native` which is a function pointer into
the mmap'd RWX buffer. If tt_t snapshots include jit_t, these function pointers will be invalid
after process restart (different mmap base address).

**How to avoid:** do NOT include jit_t in the snap_blob_t. The JIT cache is purely a
performance optimization; after restore it rebuilds automatically. The snapshot must exclude
all derived/cache state.

### Pitfall 4: Re-firing UART TX during replay

**What goes wrong:** During replay from a snapshot, the emulator re-executes all instructions
including UART TX writes. If `uart_write` calls `fputc`, the terminal sees duplicate output.

**How to avoid:** add `bool quiet` flag to `uart_t` (same pattern as `stm32_t.quiet`). Set it
true during replay. Cost: 1 bool field + 1 if-check in uart_write.

### Pitfall 5: Peripheral back-pointers in eth_t

**What goes wrong:** `eth_t` contains `bus_t* bus` (a back-pointer). If eth_t is naively
serialized into snap_blob_t, restoring it would overwrite `eth.bus` with a potentially stale
pointer.

**How to avoid:** in `tt_restore`, after memcpy-ing eth_state into live eth_t, re-set
`eth.bus = original_bus_ptr`. This is a 1-line fix in restore code.

### Pitfall 6: g_cpu_for_scb and g_nvic_for_run global pointers

**What goes wrong:** `scb.c` uses `cpu_t* g_cpu_for_scb` and `run.c` uses `nvic_t* g_nvic_for_run`,
`dwt_t* g_dwt_for_run`. These are raw pointers set during `main()` setup. If tt_restore
re-allocates cpu_t / nvic_t / dwt_t (e.g., on heap), these globals become dangling.

**How to avoid:** snap_blob_t restores into the same stack-allocated structs as the original
setup. The globals remain valid. Document this constraint: `tt_restore` must receive the SAME
pointer addresses as the original init.

### Pitfall 7: Snapshot stride too large for interactive step_back

**What goes wrong:** with K=100 000 cycles and JIT at 30M IPS, step_back(1) requires replaying
up to 100K cycles = 3.3ms. Acceptable. But with K=1 000 000 cycles it becomes 33ms, which
feels sluggish.

**How to avoid:** keep default K=10 000 (0.33ms max replay per step). Allow tuning via
`tt_create(stride, max_snaps)`.

### Pitfall 8: FPU NaN payload cross-host non-determinism

**What goes wrong:** `__builtin_sqrtf(-1.0f)` produces different NaN bit patterns on different
x86 microarchitectures (pre-AVX vs post-AVX) or when compiled with different -ffast-math flags.
A snapshot taken on machine A may not replay byte-equal on machine B.

**How to avoid:** for Phase 13 (same-host determinism), this is not an issue. Document it as
a known limitation: cross-host replay requires replacing all NaN-producing FPU ops with explicit
canonical NaN constants (0x7FC00000 for ARM quiet NaN). Defer to a future phase.

**Confidence: HIGH** (all pitfalls found directly in codebase; #8 verified via ARM docs and
x86 FPU behavior literature)

---

## Area 7: Prior Art — What to Learn From

### Mozilla rr (record-and-replay for Linux x86)
**What we steal:** the conceptual architecture — record non-deterministic inputs, replay with
same inputs, rewind via nearest-checkpoint + forward replay.
**What does NOT apply:** rr works at the OS syscall boundary (ptrace), handles signals,
multi-process, multi-thread. We have none of those. Our "syscall" boundary is the `bus_t` MMIO
callback, which is pure and synchronous. rr's checkpoint mechanism uses `clone(2)` + copy-on-
write fork. We use memcpy. Far simpler.
**Key lesson from rr paper (ATC'17):** keep recording overhead under 2x; rr achieves ~1.2x for
Firefox. Our event log has ~0 overhead (1000 events per 1M cycles = 0.1% extra writes).

### QEMU record/replay (icount mode)
**What we steal:** event type taxonomy (clock reads, network packets, serial input, IRQ injection).
QEMU logs exactly: input devices, hardware clocks, thread scheduling events. We have a strict
subset: UART RX, IRQ injection, ETH RX.
**What does NOT apply:** QEMU requires icount mode (instruction counting mode) which halves
performance. Our emulator already has `cpu.cycles` as the instruction counter — we get icount
for free.
**Key lesson:** QEMU's replay log uses "instruction count between events" as the timestamp, not
wall time. We use `cpu.cycles` which is identical.
Source: https://www.qemu.org/docs/master/devel/replay.html (HIGH confidence)

### WinDbg Time Travel Debugging (TTD)
**What we steal:** the concept of JIT instrumentation cache separate from the snapshot. TTD
maintains an "instrumentation cache" of rewritten code blocks. We maintain a JIT decode cache
that is excluded from snapshots.
**What does NOT apply:** TTD is binary-only, Windows-only, proprietary. Our JIT is transparent.

### PANDA (whole-system reverse debugger, built on QEMU)
**What we steal:** the idea of pluggable "taint analysis" at record time. For us, a `tt_plugin_t`
callback hook that fires on each snapshot could enable future instrumentation.
**What does NOT apply:** PANDA's overhead (2–5x recording) is for full OS emulation. Our target
is embedded firmware with minimal I/O.

### Renode
**Does not have:** time-travel or rewind. Snapshot export exists but replay is manual.
**Why we beat it:** our snap+replay is automatic, API-driven, O(log n) seek.

**Confidence: MEDIUM** (QEMU docs HIGH; rr project verified; Renode from web search)

---

## Area 8: Integration Constraints

### Constraint 1: 14 v1.0 firmware tests must still pass

The time-travel layer is additive. `tt_t` is an optional struct passed alongside the existing
`cpu_t`/`bus_t`. The existing `run_steps_full_g` signature is unchanged. A new
`run_steps_tt(cpu_t*, bus_t*, u64 max, systick_t*, scb_t*, tt_t*)` wraps it and calls
`tt_on_cycle` periodically. Old tests that call `run_steps_full` or `run_steps_full_g` directly
continue to compile and pass without modification.

**Risk:** LOW. No existing API is changed.

### Constraint 2: JIT codegen must be determinism-aware

Current JIT blocks (jit_block_t) contain pre-decoded instructions only. Native thunks directly
mutate cpu_t fields (registers, PC). They do NOT read external state (no bus_read, no UART).
Verified: `codegen_emit` generates only: ld/st from cpu_t offsets, add/sub/and/or/xor imm,
set_pc, ret. These are all deterministic transforms of cpu_t register state.

**One concern:** JIT blocks that contain LDR/STR are NOT currently natively compiled
(`codegen_supports()` returns false for OP_LDR_*). So the bus_read path always goes through the
interpreter. Future JIT phases (JIT-02) that add LDR/STR native emit must ensure bus_read calls
are replay-aware (use event log input, not real device, during replay).

**Action for 13-01:** audit codegen_supports() list; ensure no memory-read op is natively
compiled without replay guard. Current state: safe.

### Constraint 3: gdb stub must not conflict

GDB stub (`gdb.c`) runs when `gdb_should_stop` returns true and blocks in `gdb_serve`. During
replay, GDB stub must be disabled (pass `gdb=NULL` to run_steps_tt). Add assertion: `assert(!gdb
|| !tt)` during replay. During interactive rewind, re-enable GDB after reaching target cycle.

### Constraint 4: Windows compatibility

The project compiles on Windows (MSVC, ws2_32 for sockets). The snapshot approach (memcpy) is
fully portable. `mmap MAP_PRIVATE` COW approach would require `VirtualAlloc`/`CreateFileMapping`
on Windows — this is why we chose memcpy instead.

**Confidence: HIGH** (verified against CMakeLists.txt, codegen.c WIN32 ifdefs)

---

## Standard Stack

This phase requires zero new external dependencies. Everything is built from C11 stdlib.

| Component | What we use | From |
|-----------|-------------|------|
| Snapshot storage | `malloc` + `memcpy` | libc |
| Event log | `u8[]` grow-buffer or static array | internal |
| Binary search | `bsearch` from `<stdlib.h>` | libc |
| Diff output | `fprintf` to FILE* | libc |
| Serialization | flat struct write (`fwrite`) | libc |

No new headers. No new CMake targets beyond `cortex_m_core`.

New files to create:
```
include/core/tt.h          # TT API (snap, log, rewind, diff)
src/core/tt.c              # TT implementation (~300 LOC)
tests/test_tt.c            # unit tests for TT (~150 LOC)
firmware/tt_demo/main.c    # test firmware that exercises rewind
```

---

## Architecture Patterns

### Recommended file layout

```
include/core/
  tt.h               # Public API: tt_t, snap_blob_t, ev_t, tt_periph_t

src/core/
  tt.c               # Implementation: all 8 TT requirements

tests/
  test_tt.c          # Unit: snapshot/restore byte-equal, replay, rewind timing

firmware/tt_demo/
  main.c             # Firmware: runs 50k cycles, uart output, then halts
```

### Pattern 1: tt_periph_t — peripheral bundle

To avoid 8-argument functions:

```c
// include/core/tt.h
typedef struct {
    systick_t* st;
    nvic_t*    nv;
    scb_t*     scb;
    mpu_t*     mpu;
    dwt_t*     dwt;
    stm32_t*   stm32;
    eth_t*     eth;
} tt_periph_t;
```

Snapshot/restore take `(cpu_t*, bus_t*, tt_periph_t*)`. This pattern matches the existing
`run_steps_full_g` style.

### Pattern 2: snapshot-on-stride

```c
// Inside tt_on_cycle (called from run loop after each instruction batch):
void tt_on_cycle(tt_t* tt, cpu_t* c, bus_t* bus, tt_periph_t* p) {
    if (c->cycles % tt->stride == 0) {
        tt_take_snap(tt, c, bus, p);
    }
}
```

Stride-based snapshots guarantee O(1) decision per call.

### Pattern 3: event injection during replay

```c
// In run_until_cycle:
while (c->cycles < target_cycle) {
    // find next event at or before target
    while (log_pos < log_n && log[log_pos].cycle <= c->cycles) {
        tt_inject_event(c, bus, p, &log[log_pos]);
        log_pos++;
    }
    u64 next_ev = (log_pos < log_n) ? log[log_pos].cycle : target_cycle;
    u64 steps = next_ev - c->cycles;
    run_steps_full(c, bus, steps, p->st, p->scb);
}
```

### Anti-patterns to avoid

- **Anti-pattern:** include JIT native thunk pointers in snap_blob_t. They are process-local
  function pointers that change between runs. NEVER serialize cg_thunk_t values.
- **Anti-pattern:** snapshot at every cycle. At 30M IPS, 263KB * 30M = 7.5TB/sec. Nonsensical.
  Use stride K.
- **Anti-pattern:** store event log as ASCII/JSON. Binary fixed-width is 10x smaller and O(1)
  seekable. TLV parsing overhead is wasteful for a single-threaded tight loop.
- **Anti-pattern:** share snap_blob_t across different bus_t setups (different flash images).
  The blob assumes 256KB SRAM at 0x20000000. Document this assumption.

---

## Code Examples

### Snapshot take (core logic, ~25 LOC)

```c
// src/core/tt.c
static void tt_take_snap(tt_t* tt, cpu_t* c, bus_t* bus, tt_periph_t* p) {
    if (tt->n_snaps >= tt->max_snaps) return;  /* ring: overwrite oldest if needed */
    snap_blob_t* blob = &tt->snaps[tt->n_snaps];
    blob->magic   = 0x54544B30u;
    blob->version = 1;
    blob->cycle   = c->cycles;
    blob->cpu     = *c;
    blob->st      = *p->st;
    blob->nvic    = *p->nv;
    blob->scb     = *p->scb;
    blob->mpu     = *p->mpu;
    blob->dwt     = *p->dwt;
    blob->stm32   = *p->stm32;
    /* eth_t has bus back-pointer; save state minus pointer */
    memcpy(&blob->eth_state, p->eth, sizeof(eth_t));
    blob->sram_size = SRAM_SIZE;
    /* copy SRAM flat buffer */
    region_t* sram_r = bus_find_region(bus, SRAM_BASE);  /* new helper */
    memcpy(blob->sram, sram_r->buf, SRAM_SIZE);
    /* index entry */
    tt->idx[tt->n_snaps] = (snap_entry_t){ c->cycles, tt->n_snaps };
    tt->n_snaps++;
}
```

`bus_find_region` is a simple linear scan of `bus->regs[]` already available in bus.c logic;
expose it or inline the lookup.

### Snapshot restore (core logic, ~20 LOC)

```c
static bool tt_restore_snap(tt_t* tt, u32 si, cpu_t* c, bus_t* bus, tt_periph_t* p) {
    snap_blob_t* b = &tt->snaps[si];
    *c     = b->cpu;
    *p->st  = b->st;
    *p->nv  = b->nvic;
    *p->scb = b->scb;
    *p->mpu = b->mpu;
    *p->dwt = b->dwt;
    *p->stm32 = b->stm32;
    memcpy(p->eth, &b->eth_state, sizeof(eth_t));
    p->eth->bus = bus;          /* fix back-pointer */
    region_t* sr = bus_find_region(bus, SRAM_BASE);
    memcpy(sr->buf, b->sram, SRAM_SIZE);
    /* flush caches */
    run_dcache_invalidate();
    jit_reset_counters();       /* new export from jit.c */
    return true;
}
```

### Rewind (core logic, ~30 LOC)

```c
bool tt_rewind(tt_t* tt, u64 target_cycle, cpu_t* c, bus_t* bus, tt_periph_t* p) {
    /* find largest snap index where snap.cycle <= target_cycle */
    int lo = 0, hi = (int)tt->n_snaps - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (tt->idx[mid].cycle <= target_cycle) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best < 0) return false;
    tt_restore_snap(tt, tt->idx[best].snap_idx, c, bus, p);
    /* forward replay to target_cycle */
    u32 ev_pos = tt_find_first_event(tt, tt->snaps[best].cycle);
    return tt_run_until(tt, target_cycle, ev_pos, c, bus, p);
}
```

### diff output (~20 LOC)

```c
void tt_diff(const snap_blob_t* a, const snap_blob_t* b, FILE* out) {
    for (int i = 0; i < 16; ++i)
        if (a->cpu.r[i] != b->cpu.r[i])
            fprintf(out, "R%d: 0x%08x -> 0x%08x\n", i, a->cpu.r[i], b->cpu.r[i]);
    if (a->cpu.apsr != b->cpu.apsr)
        fprintf(out, "APSR: 0x%08x -> 0x%08x\n", a->cpu.apsr, b->cpu.apsr);
    /* ... other cpu fields ... */
    for (u32 i = 0; i < a->sram_size; ) {
        if (a->sram[i] != b->sram[i]) {
            u32 j = i;
            while (j < a->sram_size && a->sram[j] != b->sram[j]) j++;
            fprintf(out, "SRAM[0x%08x..0x%08x]: %u bytes differ\n",
                    SRAM_BASE + i, SRAM_BASE + j - 1, j - i);
            i = j;
        } else { i++; }
    }
}
```

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Binary search on snap index | Custom search | `bsearch` from `<stdlib.h>` | Already correct, O(log n) |
| Checksum of blob | CRC32 table | Simple XOR32 or add32 | Phase 13 only needs detect corruption, not cryptographic integrity |
| Event log file format | Custom framing | Raw `fwrite` of ev_t[] | Fixed-width is self-framing; no parser needed |
| COW page tracking | mmap tricks | `memcpy` | 256KB is too small to justify COW; memcpy is faster per the math |
| Cross-session replay blob versioning | Complex schema | `magic + version` header field | Future migration path with minimal code |

---

## State of the Art

| Old approach | Current approach | Impact for us |
|--------------|------------------|---------------|
| rr: fork-based COW checkpoint | memcpy snapshot (for small state) | Our 263KB is below COW break-even |
| QEMU: icount mode required for replay | cpu.cycles already exists | Zero overhead |
| PANDA: QEMU plugin overhead 2-5x | embedded emulator with no OS | Our recording overhead: ~0% |
| WinDbg TTD: binary-only, Windows | open source C11 | Cross-platform |

**Deprecated/outdated:**
- "Use fork() for snapshots": Linux-only, not applicable on Windows where this project runs.
- "Serialize entire bus_t including callbacks": callbacks are code pointers, non-serializable.

---

## Open Questions

1. **Bus region discovery: `bus_find_region` helper needed**
   - What we know: `bus_t` has `regs[BUS_MAX_REGIONS]` array with `base` and `buf` fields.
   - What's unclear: no exported function to look up a region by base address. Need to add one.
   - Recommendation: add `region_t* bus_find_flat(bus_t* b, addr_t base)` in bus.c, 5 LOC.

2. **jit_t static counters exposure**
   - What we know: `static u32 counters[JIT_MAX_BLOCKS]` is function-local in jit_run().
   - What's unclear: making it non-static requires moving it to jit_t struct and updating all callers.
   - Recommendation: expose `void jit_reset_counters(jit_t* j)` that zeros the array; move array into jit_t.
     ~5 LOC change in jit.c.

3. **Snap store memory allocation**
   - What we know: 100 snapshots at K=10 000 = 26.3MB for 1M cycles.
   - What's unclear: should snap_store[] be static (fixed max) or malloc'd at tt_create time?
   - Recommendation: malloc in tt_create with `max_snaps` parameter. Allow caller to tune.
     Default max_snaps=200 = 52.6MB ceiling.

4. **Replay mode: suppress side effects globally**
   - What we know: UART TX calls fputc, GPIO writes call fprintf to stderr.
   - What's unclear: should replay suppress ALL peripheral output, or only non-deterministic input?
   - Recommendation: add `bool replay_mode` to a global `tt_t* g_tt` pointer checked in uart_write
     and gpio_write. During replay, set `replay_mode=true`. Cost: 1 global bool.

5. **Event log capacity**
   - What we know: 1M cycles generates ~1000 events = 16KB.
   - What's unclear: should the log be static (e.g., 1M entries = 16MB) or dynamically grown?
   - Recommendation: static array of 65536 events (1MB) as the default, with overflow detection.
     For longer recordings, grow via realloc.

---

## Implementation Order Recommendation

### 13-01: Determinism Kernel (~150 LOC)
**Goal:** TT-01, TT-02.

Must contain:
- Audit all MMIO handlers; confirm no rand/wall-clock usage (done in research: all clean).
- Add `ev_t` struct and `ev_log_t` (append-only array) to `include/core/tt.h`.
- Wrap `nvic_set_pending()` to log EVENT_IRQ_INJECT with current cycle.
- Wrap `uart_read()` to return bytes from replay queue during replay; log EVENT_UART_RX during record.
- Add `bool replay_mode` flag; suppress UART TX / GPIO stderr during replay.
- Add `jit_reset_counters()` to jit.c (move static array into jit_t).
- Add `bus_find_flat()` to bus.c.
- Unit test: run firmware twice, verify cpu.cycles and APSR match byte-equal at step N.

### 13-02: Snapshot Module (~120 LOC)
**Goal:** TT-03, TT-04.

Must contain:
- Define `snap_blob_t` struct with magic/version/cycle + cpu_t + all periph structs + sram[].
- Implement `tt_take_snap()`: memcpy all state into blob. ~25 LOC.
- Implement `tt_restore_snap()`: reverse direction; fix eth.bus back-pointer; flush dcache + jit counters. ~20 LOC.
- Implement `tt_snapshot_to_file()` / `tt_restore_from_file()` for cross-session use. ~20 LOC.
- Unit test (TT-03): snapshot at cycle 5000; modify SRAM; restore; verify SRAM byte-equal.
- Unit test (TT-04): time 100 restore calls; assert mean < 100ms (should be ~0.03ms).

### 13-03: Replay Engine (~100 LOC)
**Goal:** TT-05.

Must contain:
- Implement `tt_run_until(tt, target_cycle, ev_pos, c, bus, p)`: runs forward from current state,
  injecting events from log at correct cycles, stopping at target_cycle. ~40 LOC.
- Implement `tt_replay(tt, snap_idx, target_cycle, ...)`: restore snap + run_until. ~15 LOC.
- Unit test (TT-05): record 100K cycle run of test firmware; snapshot at 0; replay to cycle 50K;
  replay again from cycle 0 to 50K; assert final cpu_t byte-equal across both replays.

### 13-04: Rewind Primitives (~80 LOC)
**Goal:** TT-06, TT-07, TT-08.

Must contain:
- Implement `tt_rewind(tt, target_cycle, ...)`: bsearch on snap_idx[] + restore + run_until. ~30 LOC.
- Implement `tt_step_back(tt, N, ...)`: rewind(cpu.cycles - N). ~5 LOC.
- Implement `tt_diff(a, b, FILE*)`: register + SRAM comparison with range encoding. ~30 LOC.
- Unit test (TT-06): fill 1M cycle history; measure rewind latency to random targets; assert <100ms.
- Unit test (TT-07): step_back(1) from cycle 9999; verify arrives at cycle 9998.
- Unit test (TT-08): diff two snaps that differ in R0 and 4 SRAM bytes; verify diff output.

### 13-05: Time-Travel Firmware Test (~80 LOC new firmware + 50 LOC harness)
**Goal:** integration + regression.

Must contain:
- `firmware/tt_demo/main.c`: firmware that runs 50K cycles, writes known sequence to UART, reads
  from NVIC IRQ handler, performs arithmetic, halts. Deterministic by construction.
- `tests/firmware/test14_tt_rewind.c`: harness that (1) runs firmware to 50K cycles with TT recording,
  (2) rewinds to cycle 25K, (3) continues to 50K again, (4) asserts final state byte-equal to first run.
- Verify all 14 prior firmware tests still pass (run existing suite with TT enabled in background).

---

## Sources

### Primary (HIGH confidence)
- Direct codebase read: all `.c` / `.h` files in `src/core/`, `src/periph/`, `include/core/`, `include/periph/`
- `https://www.qemu.org/docs/master/devel/replay.html` — QEMU event taxonomy, log format, icount mode
- `https://rr-project.org/` — rr checkpoint/replay design principles

### Secondary (MEDIUM confidence)
- ATC'17 paper "Engineering Record And Replay For Deployability" (arxiv 1705.05937) — rr design lessons
- QEMU docs replay.txt v5.2.0 — event type list cross-referenced with current docs
- ARM DDI 0403E (ARMv7-M Architecture Reference) — FPU NaN behavior, FPSCR flags

### Tertiary (LOW confidence)
- WebSearch: "time travel debugging optimal checkpoint interval" — general landscape; not primary source
- WebSearch: "floating point NaN determinism x86 ARM" — confirms host-FPU NaN bit patterns; low confidence on
  exact behavior without direct hardware test

---

## Metadata

**Confidence breakdown:**
- Determinism audit: HIGH — read every source file directly
- Snapshot design: HIGH — arithmetic verified, approach matches known-good systems
- Event log format: HIGH — QEMU docs cross-verified
- Rewind algorithm: HIGH — bsearch is standard; latency math verified
- Pitfalls: HIGH — all from direct codebase inspection
- FPU NaN cross-host: LOW — known issue, documented as out-of-scope

**Research date:** 2026-04-24
**Valid until:** 2026-06-01 (stable domain; only changes if new JIT ops are added)
