---
phase: 14-jit-depth
plan: 04
subsystem: jit
tags: [x86-64, codegen, arm-thumb, eflags, branch, jcc, apsr]

# Dependency graph
requires:
  - phase: 14-jit-depth
    plan: 03
    provides: "emit_flags_nzcv/nz; CMP/CMN/TST native; APSR write via lahf+seto; 48 opcode families"

provides:
  - "emit_apsr_to_eflags: APSR NZCV -> x86 EFLAGS via pushfq+and+bt+setc/setnc+or+popfq"
  - "emit_b_cond: jcc rel32 conditional branch with EFLAGS reconstruction; 14 ARM cond codes"
  - "emit_b_uncond: unconditional branch PC store"
  - "emit_t32_bl: BL with LR=(pc+size)|1 and PC=(pc+4+imm)&~1"
  - "codegen_emit trailing st_pc suppression for branch terminators"
  - "test_jit_branch: 106 assertions covering all 14 ARM conditions + B_UNCOND + T32_BL"

affects: [14-05-bench, future-block-chaining, isa-coverage-tracker]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "APSR.C -> x86.CF inversion: ARM C=1=no-borrow stored as CF=0 (x86 no-borrow convention); jcc table uses jae/jb/ja/jbe accordingly"
    - "bt+setc/setnc+movzx sequence preserves CF through zero-extension (no xor which would clear CF)"
    - "pushfq/pop-build-push/popfq sandwich to inject NZCV into live EFLAGS without affecting reserved bits"
    - "branch terminators suppress codegen_emit trailing st_pc via wrote_pc flag"

key-files:
  created:
    - tests/test_jit_branch.c
  modified:
    - src/core/codegen.c
    - tests/CMakeLists.txt

key-decisions:
  - "ARM.C -> x86.CF inverted: ARM no-borrow=C=1; x86 no-borrow=CF=0; store NOT(ARM.C) into EFLAGS.CF so jcc table (CS=jae, CC=jb, HI=ja, LS=jbe) is consistent"
  - "setc+movzx instead of xor+setc: xor clears CF before setc can capture it; movzx zero-extends without touching flags"
  - "Branch terminators suppress trailing st_pc in codegen_emit (already wrote PC)"
  - "T32_BL target masked &~1u to strip Thumb LSB per executor.c convention"

patterns-established:
  - "setc_r10d: setc r10b + movzx r10d,r10b (flag-safe zero-extension)"
  - "setnc_r10d: setnc r10b + movzx r10d,r10b (inverted capture for carry)"

# Metrics
duration: 75min
completed: 2026-04-27
---

# Phase 14 Plan 04: Branch Native Emission Summary

**Native B.cond via pushfq+NZCV-rebuild+popfq+jcc-rel32 with ARM.C->CF inversion; B uncond and T32_BL PC/LR stores; 14 ARM condition codes verified; ctest 14->15**

## Performance

- **Duration:** ~75 min
- **Started:** 2026-04-27T11:00:00Z
- **Completed:** 2026-04-27T12:09:54Z
- **Tasks:** 2 (+ 2 bug-fix deviations)
- **Files modified:** 3

## Accomplishments

- `emit_apsr_to_eflags`: reconstructs x86 EFLAGS from APSR NZCV via pushfq/pop rax / clear mask 0x08C1 / 4x(bt+setc+movzx+shl+or) / push rax/popfq; preserves all non-NZCV EFLAGS bits
- ARM.C -> x86.CF inversion identified and fixed: ARM no-borrow = C=1; x86 no-borrow = CF=0; `setnc_r10d` stores NOT(C) so `jae`/`jb`/`ja`/`jbe` opcodes work correctly
- `emit_b_cond` layout: apsr_to_eflags + jcc rel32(disp=13) + st_pc(fall,11B) + jmp_short(11) + st_pc(taken,11B)
- `emit_b_uncond` / `emit_t32_bl`: simple PC stores; BL also sets LR=(pc+size)|1
- `codegen_emit` suppresses trailing `st_pc` for branch terminators
- ctest count: 14 -> 15; test_jit_branch 106 assertions all pass

## Task Commits

1. **Task 1: emit_apsr_to_eflags + branch emitters + codegen wiring** - `6da40c8` (jit)
2. **Task 2: test_jit_branch + bug fixes** - `0fa401b` (fix+test)

## Files Created/Modified

- `src/core/codegen.c` - emit_apsr_to_eflags, emit_b_cond, emit_b_uncond, emit_t32_bl, cond_to_jcc, setnc_r10d, setc_r10d fix, codegen_emit wrote_pc guard, codegen_supports branch ops
- `tests/test_jit_branch.c` - 106-assertion test: 14 cond codes x 2-3 inputs + B_UNCOND + T32_BL
- `tests/CMakeLists.txt` - add test_jit_branch executable + ctest entry

## Decisions Made

- ARM.C -> x86.CF inverted (NOT direct copy) so standard jcc opcodes apply: CS=jae(0x83), CC=jb(0x82), HI=ja(0x87), LS=jbe(0x86). Direct copy would require non-standard jcc pairs.
- `setc_r10d` uses `setc r10b + movzx r10d, r10b` instead of `xor r10d,r10d + setc r10b`: xor clears CF, making setc capture 0 always. movzx is flag-neutral.
- T32_BL target = (pc+4+imm)&~1u matching executor.c Thumb LSB stripping.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] setc_r10d used xor+setc which cleared CF before capture**
- **Found during:** Task 2 (test_jit_branch showed all conditions wrong)
- **Issue:** `setc_r10d` emitted `xor r10d,r10d; setc r10b`. The `xor` instruction clears CF to 0. Then `setc r10b` always captured 0 regardless of the prior `bt` result.
- **Fix:** Changed to `setc r10b; movzx r10d, r10b`. The `movzx` zero-extends the byte to dword without touching flags.
- **Files modified:** src/core/codegen.c
- **Verification:** test_jit_branch went from 88/106 to 98/106 passing
- **Committed in:** 0fa401b (Task 2 commit)

**2. [Rule 1 - Bug] ARM.C -> x86.CF needs inversion for jcc table to work**
- **Found during:** Task 2 (CS/CC/HI/LS conditions still failing after bug 1 fix)
- **Issue:** ARM C=1 means no-borrow (unsigned a >= b); x86 CF=0 means no-borrow. Copying C=1 directly to CF=1 inverts the carry semantics. x86 `jae` (CF=0) checks "above or equal" = no-borrow, which is ARM CS. But with direct copy, ARM CS (C=1) gives CF=1, and `jae` would NOT branch. The plan's jcc table (CS=jae) is correct only with inverted carry.
- **Fix:** Added `setnc_r10d` helper (`setnc r10b + movzx r10d,r10b`). Used it instead of `setc_r10d` for the APSR.C -> CF step in `emit_apsr_to_eflags`. Also added comment explaining the inversion.
- **Files modified:** src/core/codegen.c
- **Verification:** test_jit_branch 106/106 passing; all 14 cond codes correct
- **Committed in:** 0fa401b (Task 2 commit)

**3. [Rule 2 - Adaptation] Test uses actual harness API (ASSERT_TRUE/TEST_REPORT) not plan's TEST(cond,tag)/PASS()**
- **Found during:** Task 2 (test file writing)
- **Issue:** Plan used `TEST(cond, tag)` and `PASS()` macros that don't exist in test_harness.h
- **Fix:** Used `ASSERT_TRUE`, `ASSERT_EQ_U32`, `TEST(name){}`, `RUN(name)`, `TEST_REPORT()` as in other test files
- **Files modified:** tests/test_jit_branch.c
- **Verification:** Compiles and runs correctly
- **Committed in:** 0fa401b

---

**Total deviations:** 3 auto-fixed (2 Rule 1 bugs, 1 Rule 2 adaptation)
**Impact on plan:** Bugs 1 and 2 are directly related — both stem from the plan's `setc_r10d` being wrong in two ways. All fixes necessary for correct branch emission. No scope creep.

## Issues Encountered

- Build environment: `cc.exe` (MinGW GCC 14.2) requires `/c/msys64/mingw64/bin` in PATH to find DLLs for spawned subprocesses (cc1.exe, as.exe). Without it, every compilation silently fails (empty stderr, exit=1). Fixed by `export PATH="/c/msys64/mingw64/bin:$PATH"` — this was a session environment issue, not a code issue.

## Next Phase Readiness

- Native B.cond fully functional; most firmware loop TBs now stay native end-to-end
- 52 opcode families covered natively (was 48)
- Ready for 14-05 (direct block chaining / jmp rel32 inter-TB, per pending todos)
- Benchmark (14-06) can now measure real improvement from branch emission

## Self-Check: PASSED

All claimed artifacts verified:
- `src/core/codegen.c` — contains `emit_apsr_to_eflags`, `emit_b_cond`, `setnc_r10d`, `cond_to_jcc`
- `tests/test_jit_branch.c` — exists, 211 lines
- Commits `6da40c8` and `0fa401b` exist in git log
- 15/15 ctest pass

---
*Phase: 14-jit-depth*
*Completed: 2026-04-27*
