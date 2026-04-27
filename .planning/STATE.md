# State

## Project Reference

See: .planning/PROJECT.md (updated 2026-04-26)

**Core value:** f(state, time, events) -> state' deterministic, fast, snapshotable, reversible
**Current focus:** v2.0 — time-travel + product platform

## Current Position

Phase: 14 (in progress)
Plan: 04 (complete — native B.cond via APSR->EFLAGS+jcc rel32; B uncond; T32_BL; test_jit_branch)
Status: 14-04 complete; 15/15 ctest; ready for 14-05
Last activity: 2026-04-27 — 14-04 executed; emit_apsr_to_eflags; emit_b_cond/uncond/bl; ctest 14->15

## Performance Metrics

v1.0 shipped: 12 phases, 62 tests, ~30M IPS hybrid native JIT.
p13.01: 3 tasks, 4 files created, 12 modified, 5->6 tests, 65 min.
p13.02: 2 tasks, 1 file created, 3 modified, 6->8 tests, 30 min.
p13.03: 2 tasks, 1 file created, 6 modified, 8->8 tests (snapshot tests already counted), 45 min.
p13.04: 2 tasks, 1 file created, 3 modified, 8->9 tests, 35 min.
p13.05: 2 tasks, 5 files created, 2 modified, 9->10 tests, 45 min.
p13.06: 2 tasks, 1 file created, 5 modified, 10->11 tests, 25 min.
p14.01: 3 tasks, 1 file created, 6 modified, 11->12 tests, 35 min.
p14.02: 2 tasks, 1 file created, 2 modified, 12->13 tests, 45 min.
p14.03: 2 tasks, 1 file created, 2 modified, 13->14 tests, 12 min.
p14.04: 2 tasks, 1 file created, 2 modified, 14->15 tests, 75 min.

## Accumulated Context

### Decisions

- p1..p12 all decisions logged in MILESTONES.md
- p12 native JIT pattern: rdi=cpu, [rdi+R_OFF+i*4] = r[i]; supports MOV/ADD/SUB/AND/OR/EOR + imm; fallback to interp on miss
- determinism kernel needed before snapshot (must remove implicit time/rand)
- WASM port deferred to p15 — needs WSA-free network stack first
- p13.01: tt_record_irq/uart_rx use strong no-ops (MinGW static-lib weak symbol gap); 13-04 replaces bodies
- p13.01: run_steps_full_g(jit_t*) is the TT determinism path; run_steps_full_gdb(gdb_t*) keeps gdb integration
- p13.01: jit_t is ~2MB; must not be stack-allocated (Windows 1MB default stack)
- p13.02: snap_blob_t includes uart_state (rx_q/replay_mode); eth.bus zeroed on save, refilled on restore
- p13.03: run_until_cycle in run.c (co-located with run_steps_full_g), declared in tt.h; run.h uses struct forward decls
- p13.03: snap_blob_t ~263KB; must be static in tests, same as jit_t ~2MB
- p13.04: tt_record_irq/uart_rx bodies replaced in-place (no weak override); targets in TT-06 test must be >= first snap cycle (stride boundary)
- p13.04: tt_diff uses FILE* -> stdio.h added to tt.h; snap_entry_t defined in tt.h alongside full tt_t struct
- p13.04: rewind mean latency 0.3ms at 1M history; bsearch 7 cmps + 0.14ms memcpy + worst-case 10K cycles forward replay
- p13.05: tt_on_cycle stride-boundary fix: use last_snap_cycle+stride threshold (not %stride==0); modulo fails when startup adds non-zero cycle offset before first instruction batch
- p13.05: firmware/test_tt flash at 0x00000000 (emulator default); volatile sram_pad needed to prevent optimizer from eliminating loops
- p13.05: phase 13 complete; TT-01..TT-08 all satisfied; 10/10 tests pass; shippable
- p13.06: TT-02 ETH gap closure; frames[] side-blob in tt_t (NOT snap_blob_t); ev_t.payload reused as u32 frame_id; eth_inject_rx dual-use (record-time + replay-time); EVENT_ETH_RX no longer a no-op; 11/11 tests pass
- p14.01: WIN64 ABI fix: thunk prologue saves rcx/rdx->r15/r14 (non-volatile); [r15+disp32] REX.B=1 addressing throughout; 4 pushes+sub rsp,32 = 64B stack aligned; jit_flush exported (zeros n_blocks+lookup_n+cg.used+all tables); snap_restore calls jit_flush (TB cache invalidation on rewind); test_jit_abi 15/15 assertions; 12/12 ctest + 14/14 firmware
- p14.02: native LDR/STR: sub rsp,16 scratch slot per call site; bus_read (rcx=bus,rdx=addr,r8d=sz,r9=&out@[rsp+0]) + bus_write (r9d=val) via mov rax,imm64+call rax; bl failure accumulator (xor ebx,ebx at prologue; or bl,1 on fault; emit_epilogue_check dual-path); 22 new opcode families (LDR/STR/LDRB/STRB/LDRH/STRH imm+reg+SP+LIT T1+T32 + LDRD/STRD); LDRD/STRD uses i->rs for second reg; native coverage 17->39 families; 13/13 ctest + 14/14 firmware
- p14.03: NZCV native: lahf clobbers AH (rax bits[15:8]); fix = save eax->r11d before lahf, movzx edx,ah before restoring eax; ARM_C=NOT CF for sub (xor r10d,1); emit_flags_nzcv(is_sub) + emit_flags_nz; CMP/CMN/TST discard result (no st_eax); T1 always-flag; T32 gate on set_flags; T32_ADDW/SUBW never flag; native coverage 39->48; 14/14 ctest + 14/14 firmware
- p14.04: B.cond native: emit_apsr_to_eflags via pushfq+and(~0x08C1)+4xbt+setc/setnc+movzx+or+popfq; ARM.C stored as NOT(C) in CF so jcc table works (CS=jae, CC=jb, HI=ja, LS=jbe); emit_b_cond layout disp32=13 skips 11B+2B to taken label; emit_b_uncond+emit_t32_bl simple PC/LR stores; codegen_emit suppresses trailing st_pc for branch terminators; native coverage 48->52; 15/15 ctest

### Pending Todos

- direct block chaining (jmp rel32 inter-TB)
- WASM-compatible socket layer (postMessage)

### Blockers

none.

## Session Continuity

Last session: 2026-04-27
Stopped at: Completed 14-04-PLAN.md (native B.cond APSR->EFLAGS+jcc; B_UNCOND; T32_BL; 15/15 ctest)
Resume file: none
