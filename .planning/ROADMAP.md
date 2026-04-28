# Roadmap: LECERF v2.0

## Overview

5 phases (13-17). Phases 13-14 are the engine (time-travel + JIT depth).
Phases 15-17 are the three product faces (web learn / CI / distribution).

## Phases

- [x] **Phase 13: Time-Travel Kernel** — determinism + snapshot + replay + rewind (shipped 2026-04-27, 8/8 TT verified, 11 ctest + 14 fw green)
- [x] **Phase 14: JIT Depth** — direct chaining + LDR/STR native + flags via LEA (shipped 2026-04-27, 6/6 JIT verified, 19 ctest + 14 fw green, 5M insns in 38-46ms = 100M+ IPS)
- [ ] **Phase 15: WASM + Web IDE** — emscripten port + Monaco editor + lessons
- [ ] **Phase 16: Python API + CI** — pytest plugin + GitHub Action + Docker
- [ ] **Phase 17: Landing & Distribution** — lecerf.dev + release pipeline

## Phase Details

### Phase 13: Time-Travel Kernel

**Goal:** turn f(state, time, events) into a fully deterministic, snapshotable, reversible function.
This is the unique USP that beats QEMU/Renode.

**Depends on:** v1.0 (existing emulator).
**Requirements:** TT-01..TT-08 (8/8 verified 2026-04-27)
**Plans:** 6 plans (5 initial + 1 gap-closure)

**Success criteria (observable):**
1. running same firmware twice with same event log produces byte-equal final state
2. snapshot(state) -> blob; restore(blob) reproduces register/memory exactly
3. rewind(N cycles back) seeks correctly across 1M+ cycle history under 100ms
4. step_back(1) returns to previous instruction in interactive debug
   (step_back uses whole-instruction granularity; cycle counter may overshoot target by up to one ARM cycle)
5. all 14 v1.0 firmware tests still pass

**Plans:**
- [x] 13-01-PLAN.md — determinism kernel: ev_t/ev_log_t, jit_t.counters[], bus_find_flat, uart.replay_mode (Wave 1)
- [x] 13-02-PLAN.md — snapshot module: snap_blob_t (~263KB) memcpy save/restore + xor32 + file rt (Wave 2)
- [x] 13-03-PLAN.md — replay engine: ev_log_seek + tt_inject_event + run_until_cycle + tt_replay (Wave 2)
- [x] 13-04-PLAN.md — rewind primitives: tt_t lifecycle + bsearch rewind + step_back + diff 0.3ms@1M (Wave 3)
- [x] 13-05-PLAN.md — time-travel firmware test: fw_tt 50K cycles, 3-run byte-eq REF=rewind=stepback (Wave 4)
- [x] 13-06-PLAN.md — TT-02 closure: tt_t.frames[256] side-blob + eth_inject_rx + EVENT_ETH_RX replay byte-eq (Wave 5, gap closure)

---

### Phase 14: JIT Depth

**Goal:** lift native JIT from 30M IPS to 100M+ IPS. Cover hot loops in real
firmware (FreeRTOS, DSP) without falling back to interpreter.

**Depends on:** Phase 13 (snapshot needed for safe TB invalidation).
**Requirements:** JIT-01..JIT-06
**Plans:** 6 plans

**Success criteria:**
1. native LDR/STR works for FreeRTOS context switch (no fallback)
2. CMP/ADDS/SUBS set NZCV flags correctly via x86 lahf+seto
3. B.cond emits x86 jcc with rel32, falls through cleanly
4. direct block chaining: terminator hands off to next TB without C-frame return
5. FreeRTOS 5M instructions in under 50ms (100M+ IPS)
6. all v1.0 firmware tests pass through native JIT path

**Plans:**
- [ ] 14-01-PLAN.md — WIN64 ABI fix: rcx/rdx -> r15/r14 prologue, [r15+R_OFF] addressing; jit_flush + snap_restore hook (Wave 1)
- [ ] 14-02-PLAN.md — native LDR/STR via WIN64 bus_read/bus_write helper-call; LDRD/STRD pair; bl-flag fault path (Wave 2)
- [ ] 14-03-PLAN.md — native NZCV via lahf+seto+bit shifts; CMP/CMN/TST family + ADD/SUB always-set retrofit (Wave 3)
- [ ] 14-04-PLAN.md — native B.cond/B.uncond/T32_BL; APSR -> EFLAGS via pushfq+bt/setc; jcc rel32 (Wave 4)
- [ ] 14-05-PLAN.md — pseudo-chain dispatch jit_run_chained + n_blocks overflow generation reset (Wave 5)
- [ ] 14-06-PLAN.md — bench harness QPC + test_jit_bench 5M test7 <50ms regression (Wave 6)
- [ ] 14-07-PLAN.md — gap closure JIT-06: native PUSH/POP/LDM/STM + B.cond fast path (no pushfq/popfq); 100M+ IPS / <50ms hard-gated (Wave 7, gap closure)

---

### Phase 15: WASM + Web IDE

**Goal:** emulator runs in browser via WASM. Web IDE is the educational
front-end (lecerf-learn product face).

**Depends on:** Phase 13 (clean determinism makes WASM port simpler).
**Requirements:** WEB-01..WEB-06

**Success criteria:**
1. emscripten produces .wasm + .js loader, page loads in <2s
2. firmware runs in browser, UART output visible in terminal pane
3. GPIO toggles drive on-page LED graphic
4. Monaco editor compiles C in browser via emcc.js (or pre-compiled lessons)
5. 5 lessons available, auto-grader pass/fail works
6. step / breakpoint / register inspect via web debug bridge

**Plans:**
- 15-01: emscripten build target + WASM-friendly socket abstraction
- 15-02: web debug bridge (postMessage replaces TCP RSP)
- 15-03: web IDE shell (React/Svelte) with editor + run + regs
- 15-04: 5 lessons + auto-grader
- 15-05: hosted demo on lecerf.dev (or vercel preview)

---

### Phase 16: Python API + CI Runner

**Goal:** Build the lecerf-ci product face: pip-installable Python wrapper
around the LECERF C engine, pytest plugin with fixtures for assertions on
UART/registers/GPIO, Docker image <50MB, GitHub Action with <30s 3-test
sample-repo run.

**Depends on:** Phase 13 (snapshot/restore exposed in API), Phase 14 (engine
performance budget the &lt;30s CI gate relies on).
**Requirements:** CI-01..CI-06
**Plans:** 5 plans

**Success criteria (observable):**
1. pip install lecerf (or local wheel) works
2. import lecerf; Board("stm32f407").flash(...).run() returns exit cause
3. pytest can assert on UART output, register state, GPIO toggles
4. Docker image under 50MB, runs firmware test on stdin/stdout
5. GitHub Action sample repo passes a 3-test suite under 30s

**Plans:**
- [ ] 16-01-PLAN.md — board_t opaque struct + lecerf.h ABI + globals refactor (Wave 1)
- [ ] 16-02-PLAN.md — Python ctypes wrapper via scikit-build-core + Board/RunResult/Cpu/Uart/Gpio + 6 smoke tests (Wave 2)
- [ ] 16-03-PLAN.md — pytest plugin via pytest11 entry point + 3 example tests covering GPIO/UART/registers (Wave 3)
- [ ] 16-04-PLAN.md — two-stage alpine Dockerfile + .dockerignore + runner.py + 50MB size gate (Wave 4)
- [ ] 16-05-PLAN.md — GitHub Action manifest + release workflow + lecerf-ci-example sample repo (Wave 5)

---

### Phase 17: Landing & Distribution

**Goal:** lecerf.dev live, release pipeline, docs.

**Depends on:** Phases 13-16 (need product to land).
**Requirements:** DIST-01..DIST-04

**Success criteria:**
1. lecerf.dev landing page resolves with demo embed + install instructions
2. README.md has architecture diagram, ISA table, build/run, link to lessons
3. GitHub releases with single-binary tarballs for linux/macos/windows
4. CI workflow builds + tests + attaches release artifacts on tag push

**Plans:**
- 17-01: README rewrite v2 + diagrams
- 17-02: landing page (static, vercel/cloudflare)
- 17-03: release CI matrix (linux/macos/windows binaries)
- 17-04: tag v2.0.0 + first public release

## Progress

| Phase | Plans | Status |
|-------|-------|--------|
| 13. Time-Travel Kernel | 6 | Shipped 2026-04-27 (8/8 TT, 11 ctest + 14 fw) |
| 14. JIT Depth          | 7 | Shipped 2026-04-27 (6/6 JIT, 19 ctest + 14 fw; 5M insns 38-46ms = 100M+ IPS; gap closure 14-07) |
| 15. WASM + Web IDE     | 5 | Not started |
| 16. Python API + CI    | 5 | Planned 2026-04-28 (5 plans / 5 waves) |
| 17. Landing & Dist     | 4 | Not started |

**Total:** 27 plans across 5 phases.

## Wave Plan: Phase 13

| Wave | Plans                | Depends on        | Notes                                                 |
|------|----------------------|-------------------|-------------------------------------------------------|
| 1    | 13-01                | (none)            | Determinism kernel: tt.h types, jit_t.cnt[], hooks    |
| 2    | 13-02, 13-03         | 13-01             | Parallel: snapshot + replay engine (no file overlap)  |
| 3    | 13-04                | 13-02, 13-03      | tt_t lifecycle + rewind/step_back/diff                |
| 4    | 13-05                | 13-04             | Integration firmware + 3-run byte-eq                  |
| 5    | 13-06                | 13-04, 13-05      | Gap closure TT-02: ETH RX side-blob + replay byte-eq  |

## Wave Plan: Phase 14

| Wave | Plans  | Depends on | Notes                                                                 |
|------|--------|------------|-----------------------------------------------------------------------|
| 1    | 14-01  | (none)     | WIN64 ABI fix: r15=cpu, r14=bus prologue; jit_flush + snap_restore   |
| 2    | 14-02  | 14-01      | Native LDR/STR via bus_read/bus_write helper-call (sequential due to shared codegen.c) |
| 3    | 14-03  | 14-02      | NZCV flag setters + CMP/CMN/TST family (sequential due to shared codegen.c) |
| 4    | 14-04  | 14-03      | B.cond / B.uncond / T32_BL with APSR -> EFLAGS reconstruction        |
| 5    | 14-05  | 14-04      | jit_run_chained pseudo-chain + generation-reset eviction              |
| 6    | 14-06  | 14-05      | Bench harness QPC + test_jit_bench 5M < 50ms regression              |
| 7    | 14-07  | 14-06      | Gap closure JIT-06: native PUSH/POP/LDM/STM + B.cond fast path; 100M+ IPS hard-gated  |

## Wave Plan: Phase 16

| Wave | Plans  | Depends on    | Notes                                                                                 |
|------|--------|---------------|---------------------------------------------------------------------------------------|
| 1    | 16-01  | (none)        | board_t opaque struct + lecerf.h ABI + globals refactor; foundational multi-instance safety |
| 2    | 16-02  | 16-01         | Python ctypes wrapper via scikit-build-core; Board/RunResult/Cpu/Uart/Gpio + 6 smoke  |
| 3    | 16-03  | 16-02         | pytest plugin via pytest11 entry point; 3 example tests for GPIO/UART/registers       |
| 4    | 16-04  | 16-02         | Two-stage alpine Dockerfile; <50MB size gate; runner.py entry script                  |
| 5    | 16-05  | 16-03, 16-04  | GitHub Action manifest + release.yml + lecerf-ci-example sample repo                  |
