---
phase: 14-jit-depth
plan: 02
subsystem: jit
tags: [codegen, win64, bus_read, bus_write, ldr, str, memory-ops, helper-call]
requires:
  - phase: 14-01
    provides: "WIN64 prologue (r15=cpu, r14=bus, rbx/rsi saved); CG_R_OFF/CG_PC_OFF/CG_APSR_OFF/CG_HALT_OFF macros; 32B shadow space at [rsp..rsp+32]; codegen_emit + codegen_supports"
provides:
  - "emit_load / emit_store: WIN64 helper-call to bus_read/bus_write with sub rsp,16 scratch slot at [rsp+0] for out parameter"
  - "emit_clear_fail: xor ebx,ebx failure flag at prologue end"
  - "emit_epilogue_check: bl-flag dual-path return (halted=1+false vs true)"
  - "22 new opcode families in codegen_supports: LDR/STR/LDRB/STRB/LDRH/STRH imm+reg+SP+LIT (T1+T32) plus LDRD/STRD"
  - "test_jit_ldr_str: STR/LDR roundtrip + LDRB/LDRH zero-extend + T32 wide LDR + bus fault path"
affects: [14-03, 14-04, 14-05]
tech-stack:
  added: [bus_read via mov rax imm64 + call rax, bus_write via same pattern]
  patterns:
    - "WIN64 helper-call: sub rsp,16 before call (out slot at [rsp+0]); add rsp,16 after; net 0 stack growth"
    - "bl as per-thunk failure accumulator: or bl,1 on each bus_* returning false; epilogue checks once"
    - "emit_load_from_eax factored helper: address already in eax; callers load+add before calling"
    - "jnz +5 skip failure stub (or bl,1 (3B) + jmp +7 (2B)); then st_ecx (7B)"
key-files:
  created: [tests/test_jit_ldr_str.c]
  modified: [src/core/codegen.c, tests/CMakeLists.txt]
key-decisions:
  - "sub rsp,16 per LDR/STR site (not prologue bump): 16B local slot at [rsp+0] = bus_read out param; maintains 16B alignment; add rsp,16 after call unconditionally"
  - "bl failure flag (not per-instruction short-circuit): or bl,1 on fault; remaining ops still execute but stores skip; epilogue sets halted once at end — simpler than backpatching jcc targets"
  - "emit_load_from_eax factored helper: LDR_LIT and LDR_REG compute address differently before calling the shared sequence; avoids code duplication"
  - "LDRD/STRD second register is i->rs (confirmed from executor.c usage); not rd2"
  - "emit_epilogue_ok replaced by emit_epilogue_check in codegen_emit; all blocks (even pure ALU ones) now get bl-check overhead (~13 extra bytes) — acceptable for correctness"
patterns-established:
  - "Memory op pattern: load base reg into eax, add imm, sub rsp/16, set rcx/rdx/r8d/r9, mov rax &fn, call rax, check al, add rsp/16"
  - "Fault flag pattern: xor ebx,ebx at prologue; or bl,1 per fault; test bl at epilogue"
duration: 45min
completed: 2026-04-27
tasks: 2
files_created: 1
files_modified: 2
tests_before: 12
tests_after: 13
---

# Phase 14 Plan 02: Native LDR/STR via WIN64 Helper-Call Summary

**WIN64 helper-call codegen for all LDR/STR variants (bus_read/bus_write via mov rax,imm64+call rax); bl-flag fault accumulator; emit_epilogue_check dual-path return; native opcode coverage 17->39 families**

## Performance

- **Duration:** ~45 min
- **Completed:** 2026-04-27
- **Tasks:** 2
- **Files modified:** 2
- **Files created:** 1

## Accomplishments

- 22 new opcode families now native: LDR/STR/LDRB/STRB/LDRH/STRH (imm, reg, SP, LIT) for T1 and T32, plus T32_LDRD/T32_STRD
- WIN64 call sequence per LDR site: sub rsp,16 -> rcx=r14(bus), rdx=addr, r8d=size, r9=&out@[rsp+0] -> mov rax,&bus_read; call rax -> ecx=out@[rsp+0]; add rsp,16; zero-extend mask for B/H
- bl failure accumulator: xor ebx,ebx after prologue; or bl,1 on each bus_* returning false; emit_epilogue_check tests bl once at exit and sets cpu->halted=1+returns false on fault
- All 14 firmware tests pass including test7_freertos and test12_dsp_fft; 13/13 ctest pass

## Task Commits

1. **Task 1: emit_ldr/str family + helper-call sequence** - `7c9b20f`
2. **Task 2: round-trip + zero-extend test** - `619440d`

## Files Created/Modified

- `src/core/codegen.c` — added emit_clear_fail, emit_load_from_eax, emit_load, emit_store, emit_epilogue_check; expanded codegen_supports (22 new opcodes); updated codegen_emit to call emit_clear_fail+emit_epilogue_check
- `tests/test_jit_ldr_str.c` — 5 sub-tests: STR/LDR roundtrip, LDRB zero-extend, LDRH zero-extend, T32_LDR_IMM wide form, bus fault path (halted=1+return false)
- `tests/CMakeLists.txt` — added test_jit_ldr_str target + add_test

## Decisions Made

- sub rsp,16 per call site (not prologue bump): cleaner, restores after each call, no prologue ABI change
- bl as failure accumulator: avoids backpatching jcc targets; remaining ops in block still run (stores skip via jnz/jmp) but no double-fault possible since halted check is single at epilogue
- emit_load_from_eax factored helper: LDR_LIT bakes address as imm32 (PC-relative computed at codegen time), LDR_REG computes rn+rm before calling; both reuse the shared load sequence
- LDRD/STRD use i->rs as second register (matches executor.c behavior)

## Deviations from Plan

**1. [Rule 3 - Adaptation] Plan's test used non-existent TEST(cond, msg) / PASS() macros**
- Found during: Task 2
- Issue: plan's test skeleton used `TEST(cond, msg)` and `PASS()` that don't exist in test_harness.h; actual harness has `TEST(name)` (sub-test declaration), `ASSERT_TRUE`, `ASSERT_EQ_U32`, `RUN`, `TEST_REPORT`
- Fix: adapted to actual harness macros; 5 named sub-tests matching plan's 5 test cases
- Files modified: tests/test_jit_ldr_str.c

**2. [Rule 3 - Adaptation] `emit_epilogue_ok` not removed (kept for internal completeness)**
- Issue: plan said replace emit_epilogue_ok; codegen_emit now calls emit_epilogue_check instead; emit_epilogue_ok left defined (unused) to avoid touching 14-01 history
- Fix: kept function, updated codegen_emit call site only; unused function causes no linkage issues

None other — plan executed as specified.

## Issues Encountered

None.

## Next Phase Readiness

- 14-03: flag-setter ops (ADD/SUB/CMP native with APSR update via LEA tricks) — CG_APSR_OFF macro exported
- 14-04: B.cond native (conditional branch in thunk body) — needs APSR read from [r15+CG_APSR_OFF]
- 14-05: direct block chaining (jmp rel32 inter-TB) — foundational for 100M+ IPS target
- FreeRTOS context switch now fully native (LDR/STR dominate hot path)

## Self-Check: PASSED

- FOUND: src/core/codegen.c
- FOUND: tests/test_jit_ldr_str.c
- FOUND: .planning/phases/14-jit-depth/14-02-SUMMARY.md
- FOUND: commit 7c9b20f (Task 1)
- FOUND: commit 619440d (Task 2)
- ctest 13/13 pass
- firmware/run_all.sh 14/14 pass

---
*Phase: 14-jit-depth*
*Completed: 2026-04-27*
