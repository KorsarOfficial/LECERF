---
phase: 14-jit-depth
plan: 01
subsystem: jit
tags: [abi, codegen, win64, jit_flush, snap_restore]
dependency_graph:
  requires: [13-06]
  provides: [14-02, 14-03, 14-04, 14-05]
  affects: [src/core/codegen.c, include/core/codegen.h, src/core/jit.c, include/core/jit.h, src/core/tt.c]
tech_stack:
  added: [emit_prologue, emit_epilogue_ok, jit_flush, CG_R_OFF, CG_PC_OFF, CG_APSR_OFF, CG_HALT_OFF]
  patterns: [WIN64 non-volatile register save, REX.B r15-relative addressing, TB cache wipe on snap_restore]
key_files:
  created: [tests/test_jit_abi.c]
  modified: [include/core/codegen.h, src/core/codegen.c, include/core/jit.h, src/core/jit.c, src/core/tt.c, tests/CMakeLists.txt]
decisions:
  - Option B from research: prologue saves rcx/rdx into non-volatile r15/r14 (not just rcx->r15); consistent [r15+...] throughout all thunks
  - 4 pushes (r15/r14/rbx/rsi, 32B) + sub rsp,32 = 64B total; entry rsp 8 mod 16 -> inner calls see rsp 8 mod 16 (16B aligned for WIN64)
  - jit_flush subsumes jit_reset_counters in snap_restore; jit_reset_counters kept exported for callers not needing full flush
  - test_jit_abi uses actual test_harness.h macros (ASSERT_TRUE/ASSERT_EQ_U32/RUN/TEST_REPORT) not the plan's pseudocode TEST/PASS
metrics:
  duration: ~35 min
  completed: 2026-04-27
  tasks: 3
  files_created: 1
  files_modified: 6
  tests_before: 11
  tests_after: 12
---

# Phase 14 Plan 01: WIN64 ABI Fix Summary

ABI fix: every native thunk now has a WIN64-correct prologue (push r15/r14/rbx/rsi + sub rsp,32 + mov r15,rcx + mov r14,rdx) and matching epilogue; all cpu_t access uses [r15+disp32] REX.B=1 addressing; jit_flush exported and wired into snap_restore.

## What Was Done

### Task 1 — codegen.h/codegen.c: ABI macros + prologue/epilogue + r15 base

**include/core/codegen.h:** Replaced the stale SYSV comment with WIN64 ABI comment. Added four offset macros:
- `CG_R_OFF   = offsetof(cpu_t, r)` — r[0]..r[15] base (= 0)
- `CG_PC_OFF  = CG_R_OFF + 15*4 = 60` — r[15]/PC
- `CG_APSR_OFF = offsetof(cpu_t, apsr) = 72` — for 14-02 flag setters
- `CG_HALT_OFF = offsetof(cpu_t, halted)` — for 14-03 bus-fault path

**src/core/codegen.c:** Dropped local `R_OFF`/`PC_OFF` macros; all helpers use `CG_R_OFF`/`CG_PC_OFF`. Rewrote ld/st helpers to REX.B=1 r15-relative encoding:
- `ld_eax`: `41 8B 87 disp32` (was `8B 87 disp32`)
- `ld_ecx`: `41 8B 8F disp32` (was `8B 8F disp32`)
- `st_eax`: `41 89 87 disp32` (was `89 87 disp32`)
- `st_pc`:  `41 C7 87 disp32 imm32` (was `C7 87 disp32 imm32`)

Added `emit_prologue`:
```
41 57  push r15
41 56  push r14
53     push rbx
56     push rsi
48 83 EC 20  sub rsp, 32
49 89 CF     mov r15, rcx
49 89 D6     mov r14, rdx
```

Added `emit_epilogue_ok`:
```
48 83 C4 20  add rsp, 32
5E           pop rsi
5B           pop rbx
41 5E        pop r14
41 5F        pop r15
B0 01        mov al, 1
C3           ret
```

`codegen_emit` now: check supports -> check capacity (`n*96+64`) -> emit prologue -> emit ops -> st_pc -> emit epilogue. Replaced `ret_true` (deleted).

Stack alignment: entry rsp = 8 mod 16. 4 pushes (32B, 0 mod 16) -> still 8 mod 16. sub rsp,32 (0 mod 16) -> still 8 mod 16. Any call inside thunk sees rsp 8 mod 16, which satisfies WIN64 requirement (caller rsp aligned before call = 0 mod 16 -> after push ret addr = 8 mod 16 at callee entry). Correct.

### Task 2 — jit.h/jit.c/tt.c: jit_flush + snap_restore hook

**include/core/jit.h:** Added `void jit_flush(jit_t* g)` declaration with comment.

**src/core/jit.c:** Implemented `jit_flush`:
- `n_blocks = 0` — pool empty
- `lookup_n = 0` — CRITICAL: without this, post-flush install() appends at stale position
- `jit_steps = native_steps = interp_steps = 0` — telemetry is run-scoped
- `cg.used = 0` — reclaim entire codegen buffer
- Loop: `lookup_idx[i] = -1; lookup_pc[i] = 0; counters[i] = 0`

**src/core/tt.c:** `snap_restore` now calls `jit_flush(g_jit_for_tt)` instead of `jit_reset_counters(g_jit_for_tt)`. jit_flush subsumes reset_counters. TT safety: after restore, old TBs reference pre-restore PC layout and must be discarded; they recompile lazily.

### Task 3 — test_jit_abi.c + CMakeLists.txt: ABI smoke test

Three sub-tests in one executable (using actual test_harness.h macros):

1. **abi_single_insn:** synthesize `insn_t{op=OP_T32_MOVW, rd=0, imm=0xDEADBEEF, pc=0, size=4}`, call `codegen_emit`, invoke thunk as `fn(&cpu, &bus)`. Assert: returns true, `cpu.r[0] == 0xDEADBEEF`, `cpu.r[PC] == 4`. Proves: prologue saves rcx->r15, thunk writes via [r15+0] = r[0], epilogue stores PC=4, returns 1.

2. **abi_jit_flush:** set fake state (n_blocks=7, lookup_n=13, cg.used=12345, counters[42]=99, lookup_idx[10]=5, lookup_pc[10]=0xCAFEBABE). Call `jit_flush`. Assert all zeroed/reset to -1.

3. **abi_two_insn_block:** after flush (cg.used=0), emit 2-insn block (MOVW r0=0x11112222 at pc=0, MOVW r1=0x33334444 at pc=4). Call thunk. Assert r[0]=0x11112222, r[1]=0x33334444, PC=8. Proves: multiple emit_op iterations work, PC set to pc_end=8.

Result: 15/15 assertions pass.

## Deviations from Plan

**1. [Rule 3 - Adaptation] Test harness macros differ from plan's pseudocode**
- Found during: Task 3
- Issue: plan used `TEST(cond, msg)` and `PASS()` macros that do not exist in tests/test_harness.h; actual harness has `ASSERT_TRUE(x)`, `ASSERT_EQ_U32(a, b)`, `RUN(fn)`, `TEST_REPORT()`
- Fix: adapted test to use actual macros; split into three named sub-tests with `TEST(name)` block pattern
- Files modified: tests/test_jit_abi.c
- No commit needed (deviation handled inline)

**2. [Rule 3 - Adaptation] Plan's extern execute() reference removed**
- Issue: plan included `extern bool execute(cpu_t*, bus_t*, const insn_t*)` for a jit_run-path test; test was simplified to direct codegen_emit + thunk invocation (more direct ABI proof)
- Fix: removed unused extern; test exercises codegen_emit -> thunk call directly

None - all other plan steps executed exactly as specified.

## Verification

- cmake --build build: clean (warnings are pre-existing: Wpedantic obj-ptr cast, nvic sign conversion, unused emit_w16)
- ctest: 12/12 pass (11 prior + jit_abi)
- firmware/run_all.sh: 14/14 pass
- objdump -t libcortex_m_core.a | grep jit_flush: `jit_flush` at scl=2 (external) confirmed
- grep jit_flush src/core/tt.c: 1 match (snap_restore)
- test_jit_abi.exe: 15/15 passed (direct output)

## Commits

- `20c7f47` — codegen WIN64 ABI: CG_R_OFF/PC_OFF/APSR_OFF macros; emit_prologue; ld/st helpers REX.B=1; emit_epilogue_ok
- `de276cc` — jit_flush: zero n_blocks+lookup_n+cg.used+counters[]+lookup_idx[]/pc[]; snap_restore calls jit_flush
- `fc9d29e` — test_jit_abi: smoke test WIN64 ABI round-trip; ctest 11->12

## Self-Check: PASSED
