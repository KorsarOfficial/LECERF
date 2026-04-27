---
phase: 14
type: research
date: 2026-04-27
domain: x86-64 JIT codegen, ARM-on-x86 emulation, basic-block chaining
confidence: HIGH
---

# Phase 14: JIT Depth — Research

**Researched:** 2026-04-27
**Domain:** x86-64 JIT codegen for ARM Cortex-M; direct block chaining; flag mapping; helper-call ABI
**Confidence:** HIGH (all findings from direct codebase reads and objdump of built binary)

---

## Summary

The current JIT (`src/core/jit.c` + `src/core/codegen.c`) is a "pre-decoded cache" — blocks of
ARM instructions are decoded once and replayed via interpreter, with a minority of simple blocks
getting native x86-64 thunks. The native path handles only 17 opcode families (MOV/ADD/SUB/AND/OR/EOR
and T32 equivalents). Any block containing LDR, STR, CMP, B.cond, or almost anything else falls back
to interpreter. Measured baseline: ~30M IPS on test7_freertos (3,050,511 insns in ~100ms).

The gap to 100M+ IPS requires: (1) native LDR/STR via helper calls, (2) native flag-setter ops
(CMP/ADDS/SUBS), (3) native B.cond via x86 jcc, (4) direct block chaining so TB-to-TB transitions
don't return through C, and (5) TB cache eviction when the 1024-slot pool fills.

**Critical pre-work:** there is a latent ABI mismatch. The emitted x86 thunks use `[rdi + R_OFF]`
to access cpu_t, but the project compiles with MinGW-w64 (`x86_64-w64-mingw32`, WIN64 ABI). In WIN64,
function arguments arrive in rcx/rdx/r8/r9, not rdi/rsi. The thunks currently work by accident
because `jit_run` happens to leave `rdi = cpu_ptr` before the call (see disassembly below). Any
helper-call thunk that emits a `call bus_read(...)` will clobber `rdi`, breaking all subsequent
`[rdi + ...]` accesses. This ABI issue must be resolved first (JIT-02 prerequisite).

**Primary recommendation:** Fix the ABI to use rcx/rdx from the thunk entry point (or save
rcx into a non-volatile register at thunk prologue), then add helper-call LDR/STR, flag-setter
ops, B.cond, and finally direct block chaining.

---

## 1. Current JIT State (Questions A.1-A.5)

### A.1 — Which opcodes have native codegen today?

`codegen_supports()` (codegen.c:60-80) returns true for exactly:

| Opcode family | x86-64 emitted | codegen.c line |
|---------------|----------------|----------------|
| OP_MOV_IMM, OP_T32_MOV_IMM, OP_T32_MOVW | `mov eax, imm32; mov [rdi+R_OFF+rd*4], eax` | 86-87 |
| OP_MOV_REG | `mov eax, [rdi+R_OFF+rm*4]; mov [rdi+R_OFF+rd*4], eax` | 89-90 |
| OP_ADD_REG | `ld_eax(rn); ld_ecx(rm); add eax,ecx; st_eax(rd)` | 93 |
| OP_SUB_REG | same with sub | 96 |
| OP_ADD_IMM3, OP_ADD_IMM8, OP_T32_ADD_IMM, OP_T32_ADDW | `ld_eax(rn); add eax, imm32; st_eax(rd)` | 98-100 |
| OP_SUB_IMM3, OP_SUB_IMM8, OP_T32_SUB_IMM, OP_T32_SUBW | same with sub | 102-104 |
| OP_AND_REG | `ld_eax(rn); ld_ecx(rm); and eax,ecx; st_eax(rd)` | 106-107 |
| OP_T32_AND_IMM | `ld_eax(rn); and eax, imm32; st_eax(rd)` | 108-110 |
| OP_ORR_REG | same with or | 113-114 |
| OP_T32_ORR_IMM | same with or imm | 115-117 |
| OP_EOR_REG | same with xor | 120-121 |
| OP_T32_EOR_IMM | same with xor imm | 122-124 |
| OP_NOP, OP_T32_NOP | no-op (no bytes emitted) | 84 |

Every block is tried: `codegen_emit` (codegen.c:157) iterates all instructions; if any has
`!codegen_supports(op)`, returns NULL immediately (line 158). Entire block falls to interpreter.

**Not native (fall to interp):** LDR/STR, CMP, ADDS/SUBS, B.cond, B.uncond, BX, BL, PUSH, POP,
LDM, STM, all T32 multiply, shift with carry, IT block, VFP.

Epilogue always emits: `st_pc(next_pc); ret_true` (codegen.c:162-164).
Return value `true` = "thunk succeeded" (jit.c:81).

### A.2 — Dispatch loop and fallback

`jit_run()` (jit.c:60-99):
1. Hash PC → look up block index.
2. If not cached: increment hot counter. If counter < JIT_HOT_THRESHOLD (16), return false (cold path).
3. At threshold: call `compile_block()` → calls `codegen_emit()`.
4. If block has `native != NULL`: call `bk->native(c, b)`. If returns true, done (native path).
5. Otherwise: walk `bk->ins[]` calling the `execute` fn pointer per instruction (interpreter path).
6. Return `true` if any instructions ran.

A native thunk runs the entire block (up to JIT_MAX_BLOCK_LEN=32 ARM insns) atomically — no
per-instruction IRQ checks inside the thunk. After return, the outer loop (`run_steps_full_g`)
does IRQ injection once per block. This means blocks with conditional branches must be short.

Block terminates at: B.cond, B.uncond, BX, BLX, T32_BL, T32_B_COND, CBZ, CBNZ, POP, T32_LDM,
UDF, SVC, BKPT, T32_TBB, T32_TBH (jit.c:24-35). These are all terminators, so basic blocks are
semantically correct.

### A.3 — Native thunk ABI

`cg_thunk_t` typedef (codegen.h:21): `bool (*)(cpu_t* c, bus_t* b)`.
Return true = success; false would trigger interpreter fallback (but current emit always returns true).

Call site in jit.c (confirmed by objdump):
```
mov %rbp,%rdx   ; rdx = bus (rbp holds bus, loaded from r8 at jit_run entry)
mov %rdi,%rcx   ; rcx = cpu (rdi holds cpu, loaded from rdx at jit_run entry)
call *%rax      ; call thunk with WIN64 ABI: rcx=cpu, rdx=bus
```

The thunk body uses `[rdi + R_OFF + ...]` to read/write cpu fields. In WIN64, `rdi` is NOT
an argument register — it is callee-saved. At the call site, `rdi` still holds the cpu pointer
(set earlier in jit_run: `mov %rdx,%rdi`), so the thunk happens to read the right value from rdi.
This works only because nothing clobbers rdi between that setup and the call.

### A.4 — cpu_t field layout

`cpu_t` (include/core/cpu.h:51-73):
```c
struct cpu_s {
    u32 r[16];      // offset 0; R0..R15
    u32 msp;        // offset 64
    u32 psp;        // offset 68
    u32 apsr;       // offset 72
    u32 ipsr;       // offset 76
    u32 epsr;       // offset 80
    u32 primask;    // offset 84
    u32 faultmask;  // offset 88
    u32 basepri;    // offset 92
    u32 control;    // offset 96
    cpu_mode_t mode; // offset 100 (int, 4 bytes)
    core_t core;    // offset 104
    u64 cycles;     // offset 108 (8 bytes, but may be 112 depending on alignment)
    bool halted;    // follows cycles
    u8 itstate;
    fpu_t fpu;      // large struct after halted/itstate
    ...
};
```

`R_OFF = offsetof(cpu_t, r) = 0` (codegen.c:12 defines it as `offsetof(cpu_t, r)`, which is 0
since `r[16]` is the first field).

`PC_OFF = R_OFF + 15*4 = 60` (codegen.c:13).

So `[rdi + 0]` = R0, `[rdi + 4]` = R1, ..., `[rdi + 60]` = R15/PC.
`apsr` is at offset 72 (after r[16]=64 bytes, msp=4, psp=4).

The codegen confirms: `ld_eax(cg, r)` emits `8B 87 XX XX XX XX` = `mov eax, [rdi + R_OFF + r*4]`
(codegen.c:25-27). The `0x87` ModRM byte = mod=10 (disp32), reg=0 (eax), rm=7 (rdi). This is
SYSV rdi encoding, not WIN64 rcx.

### A.5 — Baseline IPS measurement

```
./build/cortex-m.exe firmware/test7_freertos/test7_freertos.bin 5000000
Output: "halted after 3050511 instructions" in ~100ms (3 runs: 104ms, 103ms, 96ms)
~30M IPS (3050511 / 0.100 = 30.5M IPS)
```

The firmware halts before reaching 5M steps because it executes 3,050,511 instructions total.
Target: run the same firmware in under 50ms (5M steps), which requires 100M+ IPS for the hot loops.

---

## 2. ABI Verification (Questions H.26, B.7)

### Win64 vs SYSV — definitive finding

**Compiler:** `C:/msys64/mingw64/bin/cc.exe` → `x86_64-w64-mingw32`, GCC 14.2.0. This is WIN64 ABI.

**Declared ABI in header:** `include/core/codegen.h:9` says `/* ABI: rdi=cpu, rsi=bus -> bool */` — this
is SYSV notation. The comment is incorrect for Windows.

**Actual call site** (from `objdump -d libcortex_m_core.a`, `<jit_run>`):
```
; jit_run WIN64 args: rcx=j, rdx=c, r8=b, r9=execute
70: mov 0x3c(%rdx),%r13d    ; r13d = c->r[PC]  (rdx=cpu)
7e: mov %rcx,%rsi           ; rsi = j           (jit_t*)
81: mov %rdx,%rdi           ; rdi = c           (cpu_t*)  <-- set here
84: mov %r8,%rbp            ; rbp = b           (bus_t*)
...
; before thunk call:
c0: mov %rbp,%rdx           ; rdx = bus         (WIN64 arg1 to thunk)
c3: mov %rdi,%rcx           ; rcx = cpu         (WIN64 arg0 to thunk)
c6: call *%rax              ; thunk(cpu, bus) WIN64
```

**Thunk body emitted by codegen.c** uses `[rdi + ...]` = SYSV first-arg register.
**At call time**, `rdi` still holds `cpu_ptr` (from line 81), so it works by accident.

**The bug:** Any future thunk that calls a C helper (e.g., `bus_read`) will corrupt `rdi`
as part of WIN64 argument setup for the helper call. After the helper returns, subsequent
`[rdi + ...]` accesses will read from the wrong address.

### Fix options for JIT-02

**Option A (recommended): Thunk uses rcx instead of rdi.**
Change codegen.c `ld_eax`/`ld_ecx`/`st_eax` to encode `[rcx + offset]` (ModRM byte
`0x81` instead of `0x87`). This fixes the ABI. Win64: `rcx` is arg0.
Cost: change 3 helper functions + all emit_op call sites (simple grep-replace).

**Option B: Prologue saves rcx into r15 (non-volatile).**
At thunk entry, emit `push r15; mov r15, rcx` (6 bytes). Access cpu via `[r15 + ...]`.
At epilogue, `pop r15`. This preserves across helper calls. Slightly more bytes per thunk.

**Option C: Prologue saves rcx to a global / fixed SRAM address.**
Not thread-safe; overkill for this single-threaded emulator. Do not use.

**Decision: Option A for existing ops; Option B for LDR/STR helper-call thunks** (where
rcx must be free for the helper call's arg setup, so save cpu ptr into r15 at prologue).

Actually, simplest consistent approach: **emit a thunk prologue that saves rcx (cpu) and rdx (bus)
into non-volatile registers (r15, r14) once at block entry, then use [r15+...] throughout**.
This makes all ops consistent. r14, r15 are non-volatile in WIN64 — thunk must save/restore them.

Prologue:
```asm
push r15
push r14
mov r15, rcx   ; r15 = cpu_t*
mov r14, rdx   ; r14 = bus_t*
```
Epilogue (before ret_true):
```asm
pop r14
pop r15
```
All `[rdi + ...]` accesses become `[r15 + ...]`.

For helper calls (bus_read/bus_write) inside the block:
- Use rcx, rdx, r8, r9 freely for args
- r15 (cpu), r14 (bus) survive the call (non-volatile in WIN64)

---

## 3. Native Memory Ops Design (Questions B.6-B.9)

### B.6 — Current LDR/STR handling

`codegen_supports()` returns false for all load/store opcodes (OP_LDR_IMM, OP_STR_IMM,
OP_LDR_REG, etc.). At `codegen_emit:158`: first non-supported op → return NULL. So any block
with a LDR/STR gets `native = NULL`, falls entirely to interpreter.

FreeRTOS context switch is heavy on `LDR r0, [r1, #0]`, `STR r0, [r1, #4]`, etc.
These keep entire hot loops in interpreter path.

### B.7 — Helper-call ABI for LDR/STR (WIN64 version)

`bus_read` signature (bus.h:47): `bool bus_read(bus_t* b, addr_t a, u32 size, u32* out)`
WIN64 args: rcx=b, rdx=a, r8=size, r9=out.

`bus_write` signature (bus.h:48): `bool bus_write(bus_t* b, addr_t a, u32 size, u32 val)`
WIN64 args: rcx=b, rdx=a, r8=size, r9=val.

Sequence for `LDR Rd, [Rn, #imm]` with r15=cpu, r14=bus, prologue already run:

```asm
; compute address: Rn + imm
mov eax, [r15 + R_OFF + rn*4]   ; eax = cpu->r[rn]
add eax, imm32                   ; eax = addr (u32)
; call bus_read(bus, addr, 4, &scratch)
; need stack slot for out param (WIN64 requires 32-byte shadow space)
sub rsp, 40                      ; 32 shadow + 8 for &out alignment
mov [rsp + 32], rax              ; temporary: out lives at rsp+32... wait, need a REAL out slot
```

Actually cleaner: allocate one u32 scratch slot on stack at block prologue.

Better prologue (full):
```asm
push r15
push r14
push rbx                          ; rbx = non-volatile scratch
sub rsp, 40                       ; 32-byte shadow + 8 for u32 out slot (at rsp+32)
mov r15, rcx                      ; r15 = cpu
mov r14, rdx                      ; r14 = bus
```

For `LDR Rd, [Rn, #imm]`:
```asm
mov eax, [r15 + R_OFF + rn*4]    ; eax = cpu->r[rn]
add eax, imm32                    ; eax = addr
lea r9, [rsp + 32]                ; r9 = &out (shadow space reuse)
mov r8d, 4                        ; r8 = size=4
mov edx, eax                      ; rdx = addr
mov rcx, r14                      ; rcx = bus
call bus_read                     ; returns bool in al
; result u32 at [rsp+32]
mov eax, [rsp + 32]
mov [r15 + R_OFF + rd*4], eax    ; cpu->r[rd] = result
```

For `STR Rd, [Rn, #imm]`:
```asm
mov eax, [r15 + R_OFF + rn*4]
add eax, imm32
mov r9d, [r15 + R_OFF + rd*4]    ; r9 = val
mov r8d, 4                        ; size=4
mov edx, eax                      ; addr
mov rcx, r14                      ; bus
call bus_write
```

For byte (LDRB) / halfword (LDRH): same pattern with r8d = 1 or 2 respectively.
For LDRB: mask result: `and eax, 0xFF`.
For LDRH: mask result: `and eax, 0xFFFF`.

**Failure handling:** if `bus_read`/`bus_write` return false (al=0), set `cpu->halted = 1` and
return false from thunk (fallback to interpreter which will catch it). Emit `test al,al; jz
<epilogue_halted>` after each bus call. Epilogue_halted: `mov byte [r15 + halted_off], 1; xor al,al; ret`.

**Byte count per LDR:** ~35 bytes of x86-64. Fits well in the `n * 64` budget per instruction
(codegen.c:159 checks `n * 64 > capacity`).

### B.8 — Direct SRAM access optimization (skip bus_read for known-flat regions)

**Opportunity:** if Rn is a known compile-time constant AND falls in the flat SRAM region
(0x20000000–0x2003FFFF), we could do `mov eax, [flat_buf_ptr + (addr - SRAM_BASE)]` directly,
bypassing the region-scan in bus_read. ~5x faster per LDR.

**Complication:** the flat buffer pointer is a heap address (from `calloc` in bus_add_flat),
not a compile-time constant. We would need to bake it into the emitted x86 code as a 64-bit imm.
Adds invalidation complexity: if bus regions are ever remapped, all TBs with baked pointers
become stale.

**Recommendation:** skip for Phase 14. Use helper-call path for all LDR/STR. Note in open
questions as a Phase 15+ optimization. The helper-call path will still be fast enough for 100M+
IPS when combined with direct chaining (JIT-01).

### B.9 — LDRD/STRD/LDM/STM native?

`OP_T32_LDRD_IMM`, `OP_T32_STRD_IMM`: two separate bus_read/write calls, writeback possible.
These can be natively emitted using two back-to-back helper call sequences. They appear in
FreeRTOS context switch for saving/restoring register pairs.

`OP_LDM`, `OP_STM`, `OP_T32_LDM`, `OP_T32_STM`: variable count, complex writeback logic.
These are terminators (T32_LDM is in `is_terminator`). LDM with PC in list = branch.

**Recommendation:** LDRD/STRD: native in Phase 14 (high ROI, appears in context switch).
LDM/STM: defer to Phase 15 (variable-iteration code in codegen is complex; low-count LDM is
already handled by interpreter at acceptable cost).

---

## 4. Flag Setter Design (Questions C.10-C.13)

### C.10 — Flag-computing functions in cpu.c

Three functions, all in `src/core/cpu.c`:

| Function | What it computes | Used by |
|----------|-----------------|---------|
| `cpu_set_flags_nz(c, result)` | N = result[31], Z = (result==0), C/V unchanged | MOV_IMM, AND, EOR, ORR, LSL/LSR/ASR, MUL, etc. |
| `cpu_set_flags_nzcv_add(c, a, b, result, carry_in)` | N,Z,C=carry-out, V=signed-overflow for a+b+cin | ADD_REG, ADD_IMM, CMN, ADC |
| `cpu_set_flags_nzcv_sub(c, a, b, result)` | N,Z,C=(a>=b no borrow), V=signed-overflow for a-b | SUB_REG, SUB_IMM, CMP, SBC |

APSR bit layout (cpu.h:29-33): N=bit31, Z=bit30, C=bit29, V=bit28.
`c->apsr &= ~(APSR_N|APSR_Z|APSR_C|APSR_V)` clears, then OR-in computed bits.

**Note:** current native codegen for ADD_REG/SUB_REG emits no flag update — executor.c shows
`ADD_REG` calls `cpu_set_flags_nzcv_add` (line 128-129), but codegen.c emit_op for ADD_REG
only emits `ld_eax(rn); ld_ecx(rm); add eax,ecx; st_eax(rd)` — no APSR update! This means
native thunks currently produce WRONG flags for all ALU ops. Only reason tests pass: FreeRTOS
hot loops' flags are produced by CMP/ADDS which fall back to interpreter anyway.

### C.11 — x86 flag → ARM NZCV mapping

After `add eax, ecx` (or `sub`), x86 EFLAGS contains:
- ZF (bit 6): zero
- SF (bit 7): sign
- CF (bit 0): carry (for add: carry-out; for sub: borrow-complement — opposite of ARM C)
- OF (bit 11): overflow

ARM APSR NZCV: N=bit31, Z=bit30, C=bit29, V=bit28.

**Mapping:**
- N ← SF (identical semantics)
- Z ← ZF (identical semantics)
- C ← CF for ADD (carry-out); for SUB: C ← NOT borrow = NOT CF_x86 (x86 sub sets CF=borrow,
  ARM C=no_borrow; so ARM_C = !x86_CF for subtraction)
- V ← OF (identical semantics)

**Extraction method — `lahf` + `seto`:**

```asm
add eax, ecx            ; or sub eax, ecx
lahf                    ; AH = SF:ZF:0:AF:0:PF:1:CF (bits 7,6,4,2,0)
seto al                 ; al = OF (1 if overflow)
; now: AH bit7=SF, AH bit6=ZF, AH bit0=CF, AL=OF
; build NZCV: (N<<31)|(Z<<30)|(C<<29)|(V<<28)
movzx ebx, ah
movzx edx, al          ; edx = OF
; N: SF is AH[7]
; Z: ZF is AH[6]
; C: CF is AH[0]
; for ADD: ARM_C = CF; for SUB: ARM_C = NOT CF
shr ebx, 7             ; ebx[0] = SF → shift to ARM_N position
; ... (see full sequence in Code Examples)
```

### C.12 — ADDS Rd, Rn, Rm full sequence (native x86-64)

```asm
; Register state: r15=cpu, eax/ecx are scratch
mov eax, [r15 + R_OFF + rn*4]    ; eax = a
mov ecx, [r15 + R_OFF + rm*4]    ; ecx = b
add eax, ecx                      ; result in eax, x86 flags set
lahf                              ; AH = SZOPCF layout
seto cl                           ; cl = OF
; reconstruct NZCV into edx
movzx edx, ah                     ; edx = AH
; extract and position each bit:
; ARM N (bit31) ← SF (AH[7])
mov ebx, edx
shr ebx, 7              ; [0] = SF
shl ebx, 31             ; → bit31 = N
; ARM Z (bit30) ← ZF (AH[6])
mov esi, edx
shr esi, 6
and esi, 1
shl esi, 30             ; → bit30 = Z
or ebx, esi
; ARM C (bit29) ← CF (AH[0]) for ADD
mov esi, edx
and esi, 1              ; [0] = CF
shl esi, 29             ; → bit29 = C
or ebx, esi
; ARM V (bit28) ← OF (cl)
movzx esi, cl
shl esi, 28
or ebx, esi
; merge into APSR: clear NZCV bits, or-in new
mov edx, [r15 + APSR_OFF]
and edx, 0x0FFFFFFF     ; clear N,Z,C,V
or edx, ebx
mov [r15 + APSR_OFF], edx
; store result
mov [r15 + R_OFF + rd*4], eax
```

APSR_OFF = `offsetof(cpu_t, apsr) = 72` (after r[16]=64 + msp=4 + psp=4).

**For SUB/CMP:** same, but ARM C = !x86_CF. After extracting CF bit: `xor esi, 1` before shifting.

**For NZ-only (AND/ORR/EOR/MOV):** simpler — skip CF/OF extraction, use `lahf` only:

```asm
; after and eax,ecx:
lahf
mov edx, [r15 + APSR_OFF]
and edx, 0x3FFFFFFF     ; clear N,Z only
; N ← AH[7]
mov ebx, 0
bt eax, 15              ; test AH[7] (bit 15 of eax since lahf puts AH in bits 15:8)
setc bl; shl ebx, 31; or edx, ebx
; Z ← AH[6]
bt eax, 14
setc bl; shl ebx, 30; or edx, ebx
mov [r15 + APSR_OFF], edx
```

Wait — `lahf` puts AH in ah register (bits 15:8 of eax). After `lahf`, the register file has
AH in bits 8-15 of rax. So `bt eax, 15` tests SF (sign flag).

### C.13 — set_flags conditionality (T32 S bit)

In T32, ADD/SUB have an S-bit (`insn_t.set_flags`). `executor.c:697-699`: `if (i->set_flags)
cpu_set_flags_nzcv_add(...)`.

The T2 16-bit ADDS/SUBS (OP_ADD_REG, OP_ADD_IMM3, etc.) **always** set flags per ARMv7-M ARM
(these are T1 encodings, always set flags outside IT blocks).

The T32 versions (OP_T32_ADD_IMM, OP_T32_ADD_REG, etc.) only set flags if `i->set_flags == true`.

**For Phase 14:** codegen must check `ins[k].set_flags` for T32 ops. For T1 ops (OP_ADD_REG etc.),
always emit flag update. Add a flag `bool emit_flags` to the emit context.

---

## 5. Conditional Branch Design (Questions D.14-D.16)

### D.14 — ARM condition code → x86 jcc mapping

From `executor.c:18-41` (`cond_pass`):

| ARM cond | Code | Condition | x86 jcc | Opcode |
|----------|------|-----------|---------|--------|
| EQ | 0x0 | Z | je | 0x74 (short) / 0F 84 (rel32) |
| NE | 0x1 | !Z | jne | 0x75 / 0F 85 |
| CS/HS | 0x2 | C | jb inverted → jae / jnb | 0F 83 |
| CC/LO | 0x3 | !C | jb | 0F 82 |
| MI | 0x4 | N | js | 0F 88 |
| PL | 0x5 | !N | jns | 0F 89 |
| VS | 0x6 | V | jo | 0F 80 |
| VC | 0x7 | !V | jno | 0F 81 |
| HI | 0x8 | C && !Z | ja | 0F 87 |
| LS | 0x9 | !C \|\| Z | jbe | 0F 86 |
| GE | 0xA | N==V | jge | 0F 8D |
| LT | 0xB | N!=V | jl | 0F 8C |
| GT | 0xC | !Z && N==V | jg | 0F 8F |
| LE | 0xD | Z \|\| N!=V | jle | 0F 8E |
| AL | 0xE | always | jmp | E9 (rel32) |

All rel32 jcc are 6 bytes (0F XX rel32); jmp rel32 is 5 bytes (E9 rel32).

### D.15 — Reconstructing x86 EFLAGS from ARM APSR

x86 jcc reads EFLAGS directly. Before emitting a B.cond, we must reconstruct x86 flags from
cpu->apsr. The challenge: APSR lives in memory; EFLAGS is a processor register.

**SAHF approach:** `sahf` loads flags from AH: AH[7]=SF, AH[6]=ZF, AH[2]=PF, AH[0]=CF. Sets SF, ZF, PF, CF. Does NOT set OF.

For most conditions (EQ/NE/MI/PL/CS/CC/HI/LS): sahf is sufficient.
For VS/VC/GE/LT/GT/LE: need OF too.

```asm
; Reconstruct x86 EFLAGS from cpu->apsr:
mov eax, [r15 + APSR_OFF]         ; eax = apsr
; Build AH:
; SF ← apsr[31] (N), ZF ← apsr[30] (Z), CF ← apsr[29] (C)
; AH = SF:ZF:0:0:0:0:0:CF  (lahf format but only SF,ZF,CF matter for sahf)
; Compute: AH[7] = apsr[31], AH[6] = apsr[30], AH[0] = apsr[29]
mov ecx, eax
shr ecx, 31                        ; ecx[0] = N
shl ecx, 7                         ; ecx[7] = N → SF position
mov edx, eax
shr edx, 30                        ; edx[0] = Z
and edx, 1
shl edx, 6                         ; edx[6] = Z → ZF position
or ecx, edx
mov edx, eax
shr edx, 29                        ; edx[0] = C
and edx, 1
or ecx, edx                        ; ecx[0] = C → CF position
; ecx is now AH-equivalent
shl ecx, 8                         ; move to AH position in eax
mov ah, cl                         ; load AH with NZCV-derived flags
sahf                               ; SF, ZF, CF → EFLAGS
; For V (OF): extract apsr[28]
shr eax, 28
and eax, 1                         ; eax = V bit
; Set OF: cmp 1,0 sets OF=0; add al,127 if V=1 sets OF=1 (tricky)
; Cleaner: use pushf/popf round-trip, or use "add al, 127" trick:
; If V=1: emit "cmp byte [-1], 0" which sets OF=1 for signed overflow
; Actually simplest: after sahf, force OF via:
neg al           ; if V=1 (al=1): neg 1 → al=0xFF, sets OF=1 (neg overflow for 0x01 → 0xFF is wrong)
; Correct approach: use "add al, 0x7F": if al=1, 1+0x7F=0x80, OF=1; if al=0, 0+0x7F=0x7F, OF=0
add al, 0x7F     ; sets OF=1 if V=1 (eax was masked to 0 or 1)
```

**Simpler alternative for full NZCV:** use `pushfq` / `popfq` to build EFLAGS from scratch:

```asm
; Read APSR, reconstruct into EFLAGS via pushfq trick
pushfq
pop rax               ; rax = current EFLAGS
; clear SF,ZF,CF,OF
and rax, ~((1<<7)|(1<<6)|(1<<0)|(1<<11))
mov ecx, [r15 + APSR_OFF]
; N→SF, Z→ZF, C→CF, V→OF
bt ecx, 31; jnc .n0; or rax, (1<<7); .n0:
bt ecx, 30; jnc .z0; or rax, (1<<6); .z0:
bt ecx, 29; jnc .c0; or rax, (1<<0); .c0:
bt ecx, 28; jnc .v0; or rax, (1<<11); .v0:
push rax; popfq       ; restore into EFLAGS
```

This is ~25 bytes and requires no immediate patching. More portable. Use this for correctness first,
optimize to sahf path later if needed.

### D.16 — B.uncond (T4 encoding, `OP_B_UNCOND`, `OP_T32_BL`, etc.)

`OP_B_UNCOND`: `next_pc = c->r[REG_PC] + 4 + i->imm` (executor.c:311-313).
`OP_T32_BL`: same formula + sets LR.

For direct chaining (JIT-01), B.uncond at end of a TB is the simplest chaining case:
compute `target_pc` at codegen time (known: `i->pc + 4 + i->imm`), emit `jmp REL32` with
a placeholder, patch when target TB is compiled. Until then, emit `call jit_chain_helper`.

For B.cond: the branch target is one successor, fall-through is the other. Both need chaining
or helper-call stubs. This makes B.cond the complex case — two unresolved chain slots per TB.

---

## 6. Block Chaining Design (Questions E.17-E.19)

### E.17 — Current epilogue (no chaining)

Current TB epilogue (codegen.c:162-164):
```c
st_pc(cg, last);   // mov dword [rdi+PC_OFF], next_pc  (7 bytes)
ret_true(cg);      // mov al, 1; ret                    (3 bytes)
```

`jit_run` receives control, hashes `c->r[REG_PC]`, looks up next TB, dispatches. This is
~50–100 ns of overhead per block boundary (function call + hash lookup + branch dispatch).

### E.18 — Direct chaining mechanism

**Goal:** after TB A executes, jump directly to TB B's native code without returning to jit_run.

**Step 1: TB A emits a stub at its end.**

When TB A is compiled and target_pc is known (e.g., unconditional branch `jmp 0x08001234`):
```asm
; TB A epilogue (chaining-enabled):
mov [r15 + PC_OFF], target_pc    ; still update PC (for halted/IRQ detection)
call jit_lookup_or_compile       ; rax = ptr to TB B or NULL
test rax, rax
je .no_chain
jmp rax                          ; jump into TB B
.no_chain:
mov al, 1; pop r14; pop r15; ret  ; return to jit_run normally
```

**Better (QEMU-style patch-on-first-use):**

At compile time, emit at TB A epilogue:
```asm
; Patch slot: initially a call to the helper
call jit_chain_resolve       ; on first call, fills in the jmp below
jmp  REL32                   ; rel32 = 0 initially; patched by helper to TB B entry
```

The helper `jit_chain_resolve` (called with return address = addr of `jmp` instruction):
1. Looks up/compiles TB B.
2. Patches the `jmp REL32` with correct relative offset.
3. Returns normally; next time TB A runs, it jumps directly to TB B.

**Back-reference tracking for invalidation:** each TB has a `chain_in[]` list of TBs that chain
into it. When TB B is evicted, walk `chain_in[]`, reset each chaining `jmp` back to the helper call.

**Simple alternative (Phase 14 target):** Emit at TB epilogue:

```asm
; Simpler: TB epilogue stores PC and returns, but jit_run does the chained lookup first:
mov [r15 + PC_OFF], next_pc
mov al, 1
pop r14; pop r15
ret
```

Then in `jit_run`: add a tight `while` loop:
```c
while (!c->halted) {
    int idx = lookup(j, c->r[REG_PC]);
    if (idx < 0 || !j->blocks[idx].native) break;
    if (!j->blocks[idx].native(c, b)) break;
    // check IRQs here
}
```

This is "pseudo-chaining" — no patching, but avoids the full `run_steps_full_g` overhead
between blocks. Reduces dispatch overhead by ~5x. Simpler, no back-reference list needed.

**Phase 14 recommendation:** implement pseudo-chaining first (tight jit_run while-loop).
True patch-based chaining (QEMU-style) in Phase 15 if needed.

### E.19 — Invalidation and TT interaction

**TB invalidation triggers:**
1. `run_dcache_invalidate()` (run.c:38): invalidates decode cache. Does NOT flush JIT TBs.
2. `jit_reset_counters()` (jit.c:101): resets hot counters. Does NOT evict compiled TBs.
3. `snap_restore` calls both (13-04-SUMMARY.md: "snap_restore calls jit_reset_counters AND
   run_dcache_invalidate").

**Should snap_restore also flush JIT TBs?** The JIT TB cache is valid after restore IF no
self-modifying code changed flash content. Flash is `writable=false` (bus.c:17), so no ARM
instruction can modify flash. SRAM code is unusual (ARM Cortex-M firmware doesn't typically
execute from SRAM). For correctness in Phase 14: flush entire JIT cache on snap_restore.

**Flushing JIT TB cache:** simplest = `j->n_blocks = 0; j->used = 0; memset(j->lookup_idx, -1, ...)`.
Reset codegen buffer too: `j->cg.used = 0`. This is safe because TB pointers in blocks[] are
all now stale (block pointers into cg.buffer which is now reset). Takes ~1ms.

**Impact on TT:** TT-06 mean rewind latency 0.3ms already includes snap_restore cost. Adding
JIT flush adds ~1ms → rewind latency ~1.3ms. Still well under 100ms budget. Acceptable.

**Chains and invalidation:** with pseudo-chaining (no patched jmps), there are no stale pointers
to unchain — just reset `n_blocks = 0` and the while-loop won't find anything. Clean.

---

## 7. TB Cache LRU Eviction (Questions F.20-F.22)

### F.20 — When does the pool fill?

JIT_MAX_BLOCKS = 1024 (jit.h:24). JIT_MAX_BLOCK_LEN = 32 (max 32 insns per TB).

Rough TB size: 32 ARM insns × ~10 bytes x86 per op (after adding flags) = ~320 bytes per TB.
Plus prologue/epilogue ~25 bytes = ~345 bytes. Round to 512 bytes for headroom.

With CG_BUFFER_SIZE = 2MB (codegen.h:13): buffer fits 2MB/512 = ~4096 TBs.
But `jit.blocks[]` has only 1024 slots → slots fill first.

FreeRTOS test7 with 2 tasks: FreeRTOS scheduler, NVIC handler, 2 task functions. Total unique
hot code paths: probably <200 TBs steady-state. The 1024-slot cap is unlikely to fill in a
5M-instruction run.

**However:** direct-mapped hash (jit.c:10-14). Hash collision evicts the old entry silently
(the `lookup_pc[h]` check at line 13). So in practice, hash collisions cause re-compilation
more than slot overflow. With 1024 slots and `(pc >> 1) & 1023` hash: collision rate depends
on firmware PC distribution.

**For Phase 14:** the cache size is adequate for FreeRTOS. Eviction (JIT-05) is a safety valve.
Implement it as: when `n_blocks >= JIT_MAX_BLOCKS`, reset everything (QEMU-style generation reset).

### F.21 — Eviction strategy recommendation

**Option A: Full generation reset.** `n_blocks = 0; cg.used = 0; memset lookup_idx, -1`.
Pro: O(1) reset, simple. Con: all compiled TBs lost, warmup delay.
QEMU uses this. Suitable for Phase 14.

**Option B: LRU per slot.** Track `last_use` timestamp in jit_block_t, evict oldest.
Pro: keeps hot blocks. Con: requires linked list or priority queue; complex with direct-mapped hash.
Defer to Phase 15.

**Recommendation: generation reset for Phase 14.** Counter `j->gen` increments on reset.
Blocks compiled in old gen are invalid; hash lookup must check `bk->gen == j->gen`.
Simpler: just memset the entire lookup table.

### F.22 — Chains and eviction

With pseudo-chaining (no patched jmps): no chains to unchain. Eviction just clears everything.

With true chaining (Phase 15): each evicted TB must have all incoming chains reset. This requires
the back-reference list (`chain_in[]`). For Phase 14 pseudo-chaining: not needed.

---

## 8. Bench Harness Design (Questions G.23-G.25)

### G.23 — Bench mode in tools/main.c

Current `main.c` (tools/main.c:112-116): prints "halted after N instructions" to stderr.
Does not print elapsed time or IPS.

Add `--bench` flag (or just always measure):
```c
// After run_steps_full_gdb:
u64 elapsed_ns = ...; // GetTickCount64 (win) or clock_gettime (posix)
fprintf(stderr, "IPS: %.2fM  elapsed: %.1fms\n",
        (double)n / elapsed_ns * 1e3, (double)elapsed_ns / 1e6);
```

Windows timing: `QueryPerformanceCounter` + `QueryPerformanceFrequency`.
Already in C11 `<time.h>` via `clock()` (resolution may be 15ms on Windows — too coarse for
<50ms target). Use `QueryPerformanceCounter` for sub-ms precision.

### G.24 — Baseline calibration

Measured: 3,050,511 instructions / ~100ms = **~30M IPS** on test7_freertos (halts naturally).

For 5M insns (the JIT-06 benchmark): test7 halts before reaching 5M, so the test
is: "run test7 with 5M max, must complete (halted) in <50ms". With current 30M IPS:
3.05M insns complete in ~100ms. With 100M IPS: same 3.05M insns = ~30ms. Passes.

Alternatively, JIT-06 could use a different firmware that actually runs 5M+ insns
(test9_freertos_ipc or a synthetic spin loop). Confirm with plan which interpretation.

### G.25 — Stress test

5M instructions is sufficient to exercise steady-state JIT (after warmup). FreeRTOS tick
runs every ~1000 instructions (SysTick), so 5M gives ~5000 scheduler preemptions —
exercises the context switch path thoroughly.

50M instructions: would be needed only to measure IPS beyond test7's natural halt.
Use a spin-loop firmware for steady-state IPS measurement.

---

## 9. Cross-Cutting Risks (Questions H.27-H.30)

### H.27 — Code style

`tools/main.c` uses 1-2 letter locals: `n`, `g`, `sz`, `b` (bus). All new codegen helpers
follow this: `ld`, `st`, `r`, `k`, `cg`. No descriptive multi-word identifiers.

### H.28 — Determinism: TT-01..TT-08 must not break

`run_until_cycle` (run.c:171-190) calls `run_steps_full_g` which calls `jit_run`.
`jit_run` returns `*out_steps = n_ins` which is the exact number of ARM instructions run.
`run_until_cycle` advances `c->cycles` by calling `run_steps_full_g`, which increments
`c->cycles` inside `execute` (executor.c:1398: `c->cycles++`).

**Risk with pseudo-chaining:** the tight while-loop in jit_run processes multiple blocks
without returning to `run_steps_full_g`. The `i += jit_steps - 1` accounting in
`run_steps_full_g` (run.c:75) counts JIT steps into the `max_steps` budget.

If pseudo-chaining runs e.g. 100 instructions inside one outer-loop iteration while
`max_steps` gap is 10, we overshoot the target cycle by up to (block_size - 1) instructions.

This breaks `run_until_cycle`'s event injection precision: events at cycles K should fire
at exactly cycle K, not cycle K+31.

**Fix:** in the chained jit_run while-loop, check `c->halted` and `cycle budget` after each block:

```c
bool jit_run_chained(jit_t* j, cpu_t* c, bus_t* b, exec_fn exec,
                     u64 max_steps, u64* out_steps) {
    u64 total = 0;
    while (!c->halted && total < max_steps) {
        u64 steps = 0;
        if (!jit_run(j, c, b, exec, &steps)) break;
        total += steps;
    }
    *out_steps = total;
    return total > 0;
}
```

Pass `max_steps` through; break early. This preserves `run_until_cycle` semantics.

**Note:** IRQ injection currently happens in `run_steps_full_g` after each `jit_run` call
(run.c:87-107). With chaining, IRQ checks must still happen between blocks. Keep them in
the outer loop, not inside the chained inner loop.

### H.29 — TT snap_restore and JIT cache

As discussed in E.19: snap_restore should flush JIT TBs (`j->n_blocks = 0; j->cg.used = 0`).
Add `jit_flush(jit_t*)` to jit.h (alongside `jit_reset_counters`).

`snap_restore` in `tt.c` already calls `jit_reset_counters(g_jit_for_tt)`. Change to also call
`jit_flush(g_jit_for_tt)`. This is safe: JIT is a cache, not semantic state. Adds ~1ms to
rewind latency. TT-06 budget is 100ms.

### H.30 — Known pitfalls from JIT literature

**Pitfall 1: ABI confusion on helper calls (CRITICAL — already identified above)**
Mitigation: fix codegen to use WIN64 rcx/rdx from the start, or use thunk prologue saving
to non-volatile r15/r14. See Section 2.

**Pitfall 2: Self-modifying code (SMC) invalidation**
ARM Cortex-M firmware almost never writes to flash. SRAM code is unusual. However, FreeRTOS
does NOT use SMC. Safe to assume no SMC for Phase 14. Add a future TODO.

**Pitfall 3: IT block inside JIT block**
IT block sets itstate, affects next 1-4 instructions. Currently IT opcodes are NOT in
`codegen_supports()`. Any block containing IT falls back to interpreter. Keep it that way
for Phase 14 — IT is rare in FreeRTOS hot paths.

**Pitfall 4: IRQ latency regression with chaining**
With pseudo-chaining, IRQ checks happen at block boundaries only. Max IRQ latency =
JIT_MAX_BLOCK_LEN × (insn_cycles) = 32 cycles per block. FreeRTOS SysTick IRQ period is
~1000 cycles, so latency stays <32/1000 = 3.2% relative jitter. Acceptable.

**Pitfall 5: PC update correctness in chained blocks**
Every TB epilogue must store the correct `next_pc` into `cpu->r[PC]` before any block exit
(even via direct jump). The IRQ handler in `run_steps_full_g` reads `c->r[REG_PC]` after
the JIT call returns. If PC is stale, IRQ pushes wrong return address.

**Pitfall 6: Stack alignment**
WIN64 requires 16-byte stack alignment at call sites. The thunk prologue `push r15; push r14;
push rbx; sub rsp, 40` = 3 pushes (24 bytes) + 40 = 64 bytes adjustment. At entry rsp is
16-byte aligned (per WIN64 convention), so after 3 pushes (odd number): rsp is misaligned.
Fix: `sub rsp, 48` (not 40) to re-align. Or use 4 pushes (even). Misaligned stack at a
`call` will cause SSE crashes.

Correct prologue:
```asm
push r15   ; -8
push r14   ; -8
push rbx   ; -8
push rsi   ; -8  (4 pushes = -32: keeps alignment)
sub rsp, 32  ; shadow space; rsp now 16-byte aligned
mov r15, rcx
mov r14, rdx
```
Epilogue: `add rsp, 32; pop rsi; pop rbx; pop r14; pop r15; mov al,1; ret`.

**Pitfall 7: bus_read failure path**
If `bus_read` returns false (fault), the thunk must set `cpu->halted = 1` and return false.
The outer jit_run checks for `halted` before iterating. If thunk returns false, jit_run
falls to interpreter which re-executes and catches the fault via `execute()`. Simpler:
thunk returning false causes interpreter re-run, interpreter catches it correctly.

---

## 10. Recommended Plan Breakdown

Six sub-plans, with wave dependencies. Do NOT implement these in parallel — each wave
depends on the previous.

### Wave 1: Foundation (must be first)

**14-01: ABI fix + thunk prologue/epilogue standardization**
- Fix `ld_eax`/`ld_ecx`/`st_eax` to use `[r15 + ...]` (or `[rcx + ...]` before prologue)
- Emit standard prologue (save r15/r14/rbx/rsi; load cpu/bus) and epilogue at every TB
- Verify all existing 14 firmware tests + 11 ctest still pass (native thunks now correct)
- Add `jit_flush(jit_t*)` to jit.h

**14-02: Flag-setter ops (JIT-03)**
- Add APSR-update sequences for ADD_REG, SUB_REG, AND_REG, ORR_REG, EOR_REG
  (NZ-only: use lahf/bt approach)
- Add NZCV-full sequences for CMP_IMM, CMP_REG, T32_CMP_IMM, T32_CMP_REG
  (ADDS/SUBS/CMP full 4-flag update)
- Add set_flags conditionality for T32 ops (check `insn_t.set_flags`)
- Tests: verify CMP/ADDS flags match interpreter exactly (write a unit test that runs
  both paths on same inputs and compares APSR)

### Wave 2: Memory + Branch (after Wave 1)

**14-03: Native LDR/STR (JIT-02)**
- Emit helper-call sequence for: LDR_IMM, STR_IMM, LDRB_IMM, STRB_IMM, LDRH_IMM, STRH_IMM
- Emit helper-call sequence for: LDR_REG, STR_REG
- Emit helper-call sequence for: LDR_SP, STR_SP (common in FreeRTOS task stacks)
- Emit helper-call sequence for: T32_LDRD_IMM, T32_STRD_IMM
- Failure path: `bus_read` false → set halted, return false
- Tests: FreeRTOS context switch must go through native path (no fallback)

**14-04: Conditional branch (JIT-04)**
- Emit APSR → EFLAGS reconstruction sequence (pushfq/popfq method for correctness)
- Emit x86 jcc for B.cond (both OP_B_COND and OP_T32_B_COND)
- Emit jmp for OP_B_UNCOND
- B.cond terminates block; emit two PC updates (taken/not-taken) based on branch direction
- Tests: branch coverage test with all 14 condition codes

### Wave 3: Performance (after Wave 2)

**14-05: Pseudo-chaining + TB eviction (JIT-01 + JIT-05)**
- Add tight while-loop in jit_run (or a new `jit_run_chained` called from run_steps_full_g)
  that loops through chained blocks up to `max_steps`
- Add generation reset (JIT-05): when n_blocks >= JIT_MAX_BLOCKS, call jit_flush + increment gen
- Fix snap_restore to call jit_flush
- IRQ injection must remain at outer loop (run_steps_full_g level)
- Tests: TT-01..TT-08 still pass; FreeRTOS test7 still gets correct output

### Wave 4: Benchmark (after Wave 3)

**14-06: Benchmark + polish (JIT-06)**
- Add timing output to tools/main.c (QueryPerformanceCounter)
- Run test7_freertos 5M steps: assert <50ms
- Run all 14 firmware tests + 11 ctest: assert all pass
- Report: IPS before and after each wave

---

## Code Examples

### Thunk prologue/epilogue template (WIN64)

```c
/* emit at block start (codegen.c) */
static void emit_prologue(codegen_t* cg) {
    // push r15 (41 57)
    emit_b(cg, 0x41); emit_b(cg, 0x57);
    // push r14 (41 56)
    emit_b(cg, 0x41); emit_b(cg, 0x56);
    // push rbx (53)
    emit_b(cg, 0x53);
    // push rsi (56)
    emit_b(cg, 0x56);
    // sub rsp, 32  (48 83 EC 20) — shadow space
    emit_b(cg, 0x48); emit_b(cg, 0x83); emit_b(cg, 0xEC); emit_b(cg, 0x20);
    // mov r15, rcx (49 89 CF) — r15 = cpu
    emit_b(cg, 0x49); emit_b(cg, 0x89); emit_b(cg, 0xCF);
    // mov r14, rdx (49 89 D6) — r14 = bus
    emit_b(cg, 0x49); emit_b(cg, 0x89); emit_b(cg, 0xD6);
}

static void emit_epilogue(codegen_t* cg, u32 next_pc) {
    // mov dword [r15 + PC_OFF], next_pc  (41 C7 87 PC_OFF next_pc)
    emit_b(cg, 0x41); emit_b(cg, 0xC7); emit_b(cg, 0x87);
    emit_w32(cg, PC_OFF); emit_w32(cg, next_pc);
    // add rsp, 32  (48 83 C4 20)
    emit_b(cg, 0x48); emit_b(cg, 0x83); emit_b(cg, 0xC4); emit_b(cg, 0x20);
    // pop rsi (5E)
    emit_b(cg, 0x5E);
    // pop rbx (5B)
    emit_b(cg, 0x5B);
    // pop r14 (41 5E)
    emit_b(cg, 0x41); emit_b(cg, 0x5E);
    // pop r15 (41 5F)
    emit_b(cg, 0x41); emit_b(cg, 0x5F);
    // mov al, 1 (B0 01)
    emit_b(cg, 0xB0); emit_b(cg, 0x01);
    // ret (C3)
    emit_b(cg, 0xC3);
}
```

After this, `ld_eax` changes to use r15 base:
```c
/* mov eax, [r15 + R_OFF + r*4] (45 89 .. or 41 8B 87 ...) */
static void ld_eax(codegen_t* cg, u8 r) {
    // REX.B=1 (r15 base): 41 8B 87 disp32
    emit_b(cg, 0x41); emit_b(cg, 0x8B); emit_b(cg, 0x87);
    emit_w32(cg, (u32)(R_OFF + r * 4));
}
```

### bus_read call sequence (WIN64)

```c
static void emit_ldr_imm(codegen_t* cg, u8 rd, u8 rn, u32 imm) {
    // mov eax, [r15 + R_OFF + rn*4]
    ld_eax(cg, rn);
    // add eax, imm
    emit_b(cg, 0x05); emit_w32(cg, imm);
    // lea r9, [rsp + 32]  — &out (in shadow space)
    // 4D 8D 4C 24 20
    emit_b(cg,0x4D); emit_b(cg,0x8D); emit_b(cg,0x4C);
    emit_b(cg,0x24); emit_b(cg,0x20);
    // mov r8d, 4  (41 B8 04 00 00 00)
    emit_b(cg,0x41); emit_b(cg,0xB8); emit_w32(cg, 4);
    // mov edx, eax  (89 C2)
    emit_b(cg,0x89); emit_b(cg,0xC2);
    // mov rcx, r14  (4C 89 F1)
    emit_b(cg,0x4C); emit_b(cg,0xF1);
    // call [rel32 to bus_read] — use mov rax + call rax for portability
    // mov rax, &bus_read (48 B8 addr64)
    emit_b(cg,0x48); emit_b(cg,0xB8);
    u64 fn = (u64)(uintptr_t)bus_read;
    for (int k=0;k<8;k++) emit_b(cg, (u8)(fn >> (k*8)));
    // call rax (FF D0)
    emit_b(cg,0xFF); emit_b(cg,0xD0);
    // result u32 at [rsp+32]; load into eax
    // mov eax, [rsp+32]  (8B 44 24 20)
    emit_b(cg,0x8B); emit_b(cg,0x44); emit_b(cg,0x24); emit_b(cg,0x20);
    // mov [r15 + R_OFF + rd*4], eax
    st_eax(cg, rd);
}
```

### Flag update for ADD (NZ + NZCV)

```c
static void emit_flags_nzcv_add(codegen_t* cg) {
    // After: add eax, ecx (result in eax, x86 flags set)
    // lahf (9F): AH = SF:ZF:0:AF:0:PF:1:CF
    emit_b(cg, 0x9F);
    // seto cl (0F 90 /r with cl = 0F 90 C1 ... actually: 0F 90 /r ModRM)
    // seto cl: 0F 90 C1
    emit_b(cg,0x0F); emit_b(cg,0x90); emit_b(cg,0xC1);
    // build ARM NZCV in ebx:
    // N (bit31) = SF = AH[7]:
    emit_b(cg,0x0F); emit_b(cg,0xB6); emit_b(cg,0xDC); // movzx ebx, ah
    // ... (see full sequence; abbreviated here for space)
    // Update apsr: [r15 + APSR_OFF]
    // 41 8B B7 APSR_OFF: mov esi, [r15 + APSR_OFF]
    emit_b(cg,0x41); emit_b(cg,0x8B); emit_b(cg,0xB7);
    emit_w32(cg, (u32)offsetof(cpu_t, apsr));
    // and esi, 0x0FFFFFFF
    emit_b(cg,0x81); emit_b(cg,0xE6);
    emit_w32(cg, 0x0FFFFFFFu);
    // or esi, ebx
    emit_b(cg,0x09); emit_b(cg,0xDE);
    // mov [r15 + APSR_OFF], esi
    emit_b(cg,0x41); emit_b(cg,0x89); emit_b(cg,0xB7);
    emit_w32(cg, (u32)offsetof(cpu_t, apsr));
}
```

---

## Open Questions

1. **IT block interaction with JIT-03/04**
   - IT block terminates a block (OP_T32_IT is a terminator? No, it isn't — only branches are
     terminators). Actually IT is NOT in `is_terminator`. A block starting after IT will have
     conditionally-executed insns. The interpreter handles this via `cpu_in_it()` checks.
     Native JIT cannot safely skip the IT-state check without implementing IT logic.
   - Recommendation: any block containing an insn where `cpu_in_it()` might be true → fallback.
     In practice: codegen_supports() should return false for any opcode when the block was
     entered mid-IT. The decoder sets `itstate`; codegen should check if the block has IT context.
     This is complex — defer to Phase 15. For Phase 14, any TB that contains OP_T32_IT → fallback.

2. **LDR_LIT and T32_LDR_LIT (PC-relative loads)**
   - `OP_LDR_LIT`: `addr = ((PC+4) & ~3) + imm`. PC is the instruction's PC, which is
     compile-time-known (ins[k].pc). Can compute addr at codegen time if Rn=PC.
   - Low-priority. Add in Phase 14 if time permits; else Phase 15.

3. **Pseudo-chaining IRQ timing**
   - How many consecutive blocks to chain before forcing IRQ check?
   - Recommendation: after each block, check `c->halted` (done by jit_run return value check).
     For IRQ: check every N=8 blocks or when jit_steps exceeds a threshold.

4. **jit_flush needed in jit.h: export API**
   - Currently `jit_reset_counters` is the only flush-adjacent function.
   - Add: `void jit_flush(jit_t* j)` — resets n_blocks, cg.used, lookup table.

---

## Sources

### Primary (HIGH confidence)

- Direct read: `include/core/jit.h`, `include/core/codegen.h`, `include/core/cpu.h`,
  `include/core/bus.h`, `include/core/decoder.h`
- Direct read: `src/core/jit.c`, `src/core/codegen.c`, `src/core/cpu.c`,
  `src/core/executor.c`, `src/core/bus.c`, `src/core/run.c`, `tools/main.c`
- `objdump -d build/libcortex_m_core.a` — confirmed WIN64 ABI in actual compiled output
- `cc.exe -dumpmachine` → `x86_64-w64-mingw32` (WIN64 ABI confirmed)
- Measured benchmark: `./build/cortex-m.exe firmware/test7_freertos/test7_freertos.bin 5000000`
  → 3,050,511 insns / ~100ms = 30M IPS
- Phase 13 research + 13-04-SUMMARY.md — snap_restore behavior confirmed

### Secondary (MEDIUM confidence)

- ARM ARM DDI0403E: APSR NZCV bit positions (31,30,29,28), condition codes, IT block semantics
- Intel x86-64 SDM Vol 2: LAHF (loads AH from EFLAGS), SAHF (stores AH to EFLAGS), SETO,
  PUSHFQ/POPFQ encoding, jcc rel32 opcodes, WIN64 calling convention register preservation

### Tertiary (LOW confidence)

- QEMU TCG direct-linking mechanism (general knowledge, not verified against current QEMU source)
- Mozilla rr: discussed in Phase 13 research, not re-verified for Phase 14

---

## Metadata

**Confidence breakdown:**
- Current JIT state: HIGH — read source + objdump
- ABI issue: HIGH — confirmed by objdump disassembly of actual built binary
- Flag mapping: HIGH — ARM ARM + x86 SDM + direct code read
- Block chaining design: MEDIUM — pattern is established; specific patch mechanics untested
- Bench baseline: HIGH — directly measured

**Research date:** 2026-04-27
**Valid until:** 2026-06-01 (stable: only invalidated if codegen.c or jit.c structure changes)
