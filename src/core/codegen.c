#include "core/codegen.h"
#include "core/bus.h"
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/mman.h>
#endif

/* WIN64: rcx=cpu, rdx=bus on entry; non-volatile r15/r14 hold cpu/bus across helper-calls */

static void emit_b(codegen_t* cg, u8 b) {
    if (cg->used < cg->capacity) cg->buffer[cg->used++] = b;
}
static void emit_w32(codegen_t* cg, u32 v) {
    emit_b(cg, (u8)v); emit_b(cg, (u8)(v >> 8));
    emit_b(cg, (u8)(v >> 16)); emit_b(cg, (u8)(v >> 24));
}
static void emit_w64(codegen_t* cg, u64 v) {
    for (u32 k = 0; k < 8u; ++k) emit_b(cg, (u8)(v >> (k * 8u)));
}

/* mov eax, [r15 + CG_R_OFF + r*4]  ->  41 8B 87 disp32 */
static void ld_eax(codegen_t* cg, u8 r) {
    emit_b(cg, 0x41); emit_b(cg, 0x8B); emit_b(cg, 0x87);
    emit_w32(cg, CG_R_OFF + (u32)r * 4u);
}
/* mov ecx, [r15 + CG_R_OFF + r*4]  ->  41 8B 8F disp32 */
static void ld_ecx(codegen_t* cg, u8 r) {
    emit_b(cg, 0x41); emit_b(cg, 0x8B); emit_b(cg, 0x8F);
    emit_w32(cg, CG_R_OFF + (u32)r * 4u);
}
/* mov [r15 + CG_R_OFF + r*4], eax  ->  41 89 87 disp32 */
static void st_eax(codegen_t* cg, u8 r) {
    emit_b(cg, 0x41); emit_b(cg, 0x89); emit_b(cg, 0x87);
    emit_w32(cg, CG_R_OFF + (u32)r * 4u);
}
/* mov [r15 + CG_R_OFF + r*4], ecx  ->  41 89 8F disp32 */
static void st_ecx(codegen_t* cg, u8 r) {
    emit_b(cg, 0x41); emit_b(cg, 0x89); emit_b(cg, 0x8F);
    emit_w32(cg, CG_R_OFF + (u32)r * 4u);
}
/* mov dword [r15 + CG_PC_OFF], imm32  ->  41 C7 87 disp32 imm32 */
static void st_pc(codegen_t* cg, u32 pc) {
    emit_b(cg, 0x41); emit_b(cg, 0xC7); emit_b(cg, 0x87);
    emit_w32(cg, CG_PC_OFF);
    emit_w32(cg, pc);
}
static void mov_eax_imm(codegen_t* cg, u32 imm) {
    emit_b(cg, 0xB8); emit_w32(cg, imm);
}
static void op_add_ec(codegen_t* cg) { emit_b(cg, 0x01); emit_b(cg, 0xC8); }
static void op_sub_ec(codegen_t* cg) { emit_b(cg, 0x29); emit_b(cg, 0xC8); }
static void op_and_ec(codegen_t* cg) { emit_b(cg, 0x21); emit_b(cg, 0xC8); }
static void op_or_ec (codegen_t* cg) { emit_b(cg, 0x09); emit_b(cg, 0xC8); }
static void op_xor_ec(codegen_t* cg) { emit_b(cg, 0x31); emit_b(cg, 0xC8); }
static void add_imm(codegen_t* cg, u32 v) { emit_b(cg, 0x05); emit_w32(cg, v); }
static void sub_imm(codegen_t* cg, u32 v) { emit_b(cg, 0x2D); emit_w32(cg, v); }

/* WIN64 helper-call helpers */

/* xor ebx, ebx  (31 DB)  — zero failure flag at prologue end */
static void emit_clear_fail(codegen_t* cg) {
    emit_b(cg, 0x31); emit_b(cg, 0xDB);
}
/* mov rcx, r14  (4C 89 F1)  — arg0 = bus */
static void mov_rcx_r14(codegen_t* cg) {
    emit_b(cg, 0x4C); emit_b(cg, 0x89); emit_b(cg, 0xF1);
}
/* mov edx, eax  (89 C2)  — arg1 = addr (zero-extends) */
static void mov_edx_eax(codegen_t* cg) {
    emit_b(cg, 0x89); emit_b(cg, 0xC2);
}
/* mov r8d, imm32  (41 B8 imm32)  — arg2 = size */
static void mov_r8d_imm(codegen_t* cg, u32 v) {
    emit_b(cg, 0x41); emit_b(cg, 0xB8); emit_w32(cg, v);
}
/* lea r9, [rsp+0]  (4C 8D 0C 24)  — arg3 = &out_slot */
static void lea_r9_rsp0(codegen_t* cg) {
    emit_b(cg, 0x4C); emit_b(cg, 0x8D); emit_b(cg, 0x0C); emit_b(cg, 0x24);
}
/* mov r9d, [r15 + CG_R_OFF + r*4]  (45 8B 8F disp32)  — arg3 = r[rd] for STR */
static void mov_r9d_reg(codegen_t* cg, u8 r) {
    emit_b(cg, 0x45); emit_b(cg, 0x8B); emit_b(cg, 0x8F);
    emit_w32(cg, CG_R_OFF + (u32)r * 4u);
}
/* sub rsp, 16  (48 83 EC 10) */
static void sub_rsp_16(codegen_t* cg) {
    emit_b(cg, 0x48); emit_b(cg, 0x83); emit_b(cg, 0xEC); emit_b(cg, 0x10);
}
/* add rsp, 16  (48 83 C4 10) */
static void add_rsp_16(codegen_t* cg) {
    emit_b(cg, 0x48); emit_b(cg, 0x83); emit_b(cg, 0xC4); emit_b(cg, 0x10);
}
/* mov rax, imm64  (48 B8 imm64) */
static void mov_rax_imm64(codegen_t* cg, u64 v) {
    emit_b(cg, 0x48); emit_b(cg, 0xB8); emit_w64(cg, v);
}
/* call rax  (FF D0) */
static void call_rax(codegen_t* cg) {
    emit_b(cg, 0xFF); emit_b(cg, 0xD0);
}
/* mov ecx, [rsp+0]  (8B 0C 24)  — load out_slot result */
static void mov_ecx_rsp0(codegen_t* cg) {
    emit_b(cg, 0x8B); emit_b(cg, 0x0C); emit_b(cg, 0x24);
}
/* test al, al; jnz +skip  (84 C0 75 skip) — skip failure stub on success */
static void test_al_jnz(codegen_t* cg, u8 skip) {
    emit_b(cg, 0x84); emit_b(cg, 0xC0);
    emit_b(cg, 0x75); emit_b(cg, skip);
}
/* or bl, 1  (80 CB 01)  — mark failure */
static void or_bl_1(codegen_t* cg) {
    emit_b(cg, 0x80); emit_b(cg, 0xCB); emit_b(cg, 0x01);
}
/* jmp short +n  (EB NN) */
static void jmp_short(codegen_t* cg, u8 n) { emit_b(cg, 0xEB); emit_b(cg, n); }
/* and r9d, imm32  (41 81 E1 imm32)  — mask STR val for B/H */
static void and_r9d_imm(codegen_t* cg, u32 v) {
    emit_b(cg, 0x41); emit_b(cg, 0x81); emit_b(cg, 0xE1); emit_w32(cg, v);
}

/* ---- NZCV flag-setter helpers ---- */

/* lahf (9F) + seto cl (0F 90 C1): sample SF/ZF/CF into AH; OF into cl. */
static void emit_lahf_seto_cl(codegen_t* cg) {
    emit_b(cg, 0x9F);
    emit_b(cg, 0x0F); emit_b(cg, 0x90); emit_b(cg, 0xC1);
}
/* movzx edx, ah  (0F B6 D4) */
static void movzx_edx_ah(codegen_t* cg) {
    emit_b(cg, 0x0F); emit_b(cg, 0xB6); emit_b(cg, 0xD4);
}
/* mov esi, edx  (89 D6) */
static void mov_esi_edx(codegen_t* cg) {
    emit_b(cg, 0x89); emit_b(cg, 0xD6);
}
/* shr esi, imm8  (C1 EE imm8) */
static void shr_esi(codegen_t* cg, u8 n) {
    emit_b(cg, 0xC1); emit_b(cg, 0xEE); emit_b(cg, n);
}
/* shl esi, imm8  (C1 E6 imm8) */
static void shl_esi(codegen_t* cg, u8 n) {
    emit_b(cg, 0xC1); emit_b(cg, 0xE6); emit_b(cg, n);
}
/* mov r10d, edx  (41 89 D2) */
static void mov_r10d_edx(codegen_t* cg) {
    emit_b(cg, 0x41); emit_b(cg, 0x89); emit_b(cg, 0xD2);
}
/* shr r10d, imm8  (41 C1 EA imm8) */
static void shr_r10d(codegen_t* cg, u8 n) {
    emit_b(cg, 0x41); emit_b(cg, 0xC1); emit_b(cg, 0xEA); emit_b(cg, n);
}
/* and r10d, imm32  (41 81 E2 imm32) */
static void and_r10d_imm(codegen_t* cg, u32 v) {
    emit_b(cg, 0x41); emit_b(cg, 0x81); emit_b(cg, 0xE2); emit_w32(cg, v);
}
/* shl r10d, imm8  (41 C1 E2 imm8) */
static void shl_r10d(codegen_t* cg, u8 n) {
    emit_b(cg, 0x41); emit_b(cg, 0xC1); emit_b(cg, 0xE2); emit_b(cg, n);
}
/* or esi, r10d  (44 09 D6): r/m=esi, reg=r10 — REX.R=1 */
static void or_esi_r10d(codegen_t* cg) {
    emit_b(cg, 0x44); emit_b(cg, 0x09); emit_b(cg, 0xD6);
}
/* xor r10d, imm32  (41 81 F2 imm32) — ARM_C = NOT x86_CF for SUB */
static void xor_r10d_imm(codegen_t* cg, u32 v) {
    emit_b(cg, 0x41); emit_b(cg, 0x81); emit_b(cg, 0xF2); emit_w32(cg, v);
}
/* movzx r10d, cl  (44 0F B6 D1) */
static void movzx_r10d_cl(codegen_t* cg) {
    emit_b(cg, 0x44); emit_b(cg, 0x0F); emit_b(cg, 0xB6); emit_b(cg, 0xD1);
}
/* mov r10d, [r15 + CG_APSR_OFF]  (45 8B 97 disp32) */
static void ld_r10d_apsr(codegen_t* cg) {
    emit_b(cg, 0x45); emit_b(cg, 0x8B); emit_b(cg, 0x97);
    emit_w32(cg, CG_APSR_OFF);
}
/* mov [r15 + CG_APSR_OFF], r10d  (45 89 97 disp32) */
static void st_r10d_apsr(codegen_t* cg) {
    emit_b(cg, 0x45); emit_b(cg, 0x89); emit_b(cg, 0x97);
    emit_w32(cg, CG_APSR_OFF);
}
/* or r10d, esi  (41 09 F2): r/m=r10, reg=esi — REX.B=1 */
static void or_r10d_esi(codegen_t* cg) {
    emit_b(cg, 0x41); emit_b(cg, 0x09); emit_b(cg, 0xF2);
}
/* mov r11d, eax  (41 89 C3): save result before lahf clobbers AH */
static void mov_r11d_eax(codegen_t* cg) {
    emit_b(cg, 0x41); emit_b(cg, 0x89); emit_b(cg, 0xC3);
}
/* mov eax, r11d  (44 89 D8): restore result after flags sampled */
static void mov_eax_r11d(codegen_t* cg) {
    emit_b(cg, 0x44); emit_b(cg, 0x89); emit_b(cg, 0xD8);
}

/* Emit NZCV update after x86 ADD/SUB.
   is_sub=true: ARM_C = NOT x86_CF (no-borrow convention).
   lahf writes AH (bits[15:8] of rax) with EFLAGS, clobbering our result.
   Sequence: save result to r11d -> lahf+seto -> extract AH to edx -> build NZCV
   -> write apsr -> restore eax from r11d (for st_eax after us). */
static void emit_flags_nzcv(codegen_t* cg, bool is_sub) {
    mov_r11d_eax(cg);                /* r11d = result (before lahf clobbers AH) */
    emit_lahf_seto_cl(cg);           /* AH = SF:ZF:0:AF:0:PF:1:CF; cl = OF */
    movzx_edx_ah(cg);                /* edx = AH (flags); must be before mov eax,r11d */
    mov_eax_r11d(cg);                /* eax = result (restored for caller's st_eax) */
    /* N = (AH >> 7) << 31 -> esi */
    mov_esi_edx(cg);
    shr_esi(cg, 7u);
    shl_esi(cg, 31u);
    /* Z = ((AH >> 6) & 1) << 30 -> r10d, or into esi */
    mov_r10d_edx(cg);
    shr_r10d(cg, 6u);
    and_r10d_imm(cg, 1u);
    shl_r10d(cg, 30u);
    or_esi_r10d(cg);
    /* C = (AH & 1); for SUB: ARM_C = NOT x86_CF, so xor with 1 */
    mov_r10d_edx(cg);
    and_r10d_imm(cg, 1u);
    if (is_sub) xor_r10d_imm(cg, 1u);   /* ARM_C = NOT CF for subtract */
    shl_r10d(cg, 29u);
    or_esi_r10d(cg);
    /* V = OF (cl) << 28 -> r10d, or into esi */
    movzx_r10d_cl(cg);
    shl_r10d(cg, 28u);
    or_esi_r10d(cg);
    /* merge: apsr = (apsr & 0x0FFFFFFF) | esi */
    ld_r10d_apsr(cg);
    and_r10d_imm(cg, 0x0FFFFFFFu);
    or_r10d_esi(cg);
    st_r10d_apsr(cg);
}

/* NZ-only update for AND/ORR/EOR/TST/MOV-S. C and V unchanged.
   Same lahf/AH clobber concern: save eax to r11d, extract AH to edx,
   then restore eax. */
static void emit_flags_nz(codegen_t* cg) {
    mov_r11d_eax(cg);                /* r11d = result */
    emit_b(cg, 0x9F);               /* lahf */
    movzx_edx_ah(cg);                /* edx = AH; must precede mov eax,r11d */
    mov_eax_r11d(cg);                /* eax = result (restored for caller's st_eax) */
    /* N = (AH >> 7) << 31 */
    mov_esi_edx(cg);
    shr_esi(cg, 7u);
    shl_esi(cg, 31u);
    /* Z = ((AH >> 6) & 1) << 30 */
    mov_r10d_edx(cg);
    shr_r10d(cg, 6u);
    and_r10d_imm(cg, 1u);
    shl_r10d(cg, 30u);
    or_esi_r10d(cg);
    /* preserve C (bit 29) and V (bit 28); clear only N (bit 31) and Z (bit 30) */
    ld_r10d_apsr(cg);
    and_r10d_imm(cg, 0x3FFFFFFFu);
    or_r10d_esi(cg);
    st_r10d_apsr(cg);
}

/* WIN64 thunk prologue: save non-volatile r15/r14/rbx/rsi; shadow space; load cpu/bus.
   4 pushes (32B) + sub rsp,32 = 64B total; entry rsp is 8 mod 16 (ret addr on stack),
   after 4 pushes (32B mod 16 = 0) still 8 mod 16, sub 32 (0 mod 16) -> 8 mod 16 restored. */
static void emit_prologue(codegen_t* cg) {
    emit_b(cg, 0x41); emit_b(cg, 0x57);             /* push r15 */
    emit_b(cg, 0x41); emit_b(cg, 0x56);             /* push r14 */
    emit_b(cg, 0x53);                                /* push rbx */
    emit_b(cg, 0x56);                                /* push rsi */
    emit_b(cg, 0x48); emit_b(cg, 0x83); emit_b(cg, 0xEC); emit_b(cg, 0x20);  /* sub rsp,32 */
    emit_b(cg, 0x49); emit_b(cg, 0x89); emit_b(cg, 0xCF);                    /* mov r15,rcx */
    emit_b(cg, 0x49); emit_b(cg, 0x89); emit_b(cg, 0xD6);                    /* mov r14,rdx */
}

/* Epilogue: check bl failure flag; set halted+return false on fault, else return true.
   Byte layout (must match jz offset = 21):
     84 DB          test bl, bl        (2B)
     74 15          jz .ok             (2B)  ; skip 21 bytes of halt path
     41 C6 87 xx xx xx xx 01  mov byte [r15+CG_HALT_OFF], 1  (8B)
     48 83 C4 20    add rsp, 32        (4B)
     5E             pop rsi            (1B)
     5B             pop rbx            (1B)
     41 5E          pop r14            (2B)
     41 5F          pop r15            (2B)
     30 C0          xor al, al         (2B)
     C3             ret                (1B)  ; total halt path = 21B
   .ok:
     48 83 C4 20    add rsp, 32        (4B)
     5E 5B          pop rsi; pop rbx   (2B)
     41 5E 41 5F    pop r14; pop r15   (4B)
     B0 01          mov al, 1          (2B)
     C3             ret                (1B)
*/
static void emit_epilogue_check(codegen_t* cg) {
    emit_b(cg, 0x84); emit_b(cg, 0xDB);             /* test bl, bl */
    emit_b(cg, 0x74); emit_b(cg, 21u);              /* jz .ok (+21) */
    /* halt path (21 bytes) */
    emit_b(cg, 0x41); emit_b(cg, 0xC6); emit_b(cg, 0x87);  /* mov byte [r15+disp32], imm8 */
    emit_w32(cg, CG_HALT_OFF);
    emit_b(cg, 0x01);
    emit_b(cg, 0x48); emit_b(cg, 0x83); emit_b(cg, 0xC4); emit_b(cg, 0x20); /* add rsp,32 */
    emit_b(cg, 0x5E);                                /* pop rsi */
    emit_b(cg, 0x5B);                                /* pop rbx */
    emit_b(cg, 0x41); emit_b(cg, 0x5E);             /* pop r14 */
    emit_b(cg, 0x41); emit_b(cg, 0x5F);             /* pop r15 */
    emit_b(cg, 0x30); emit_b(cg, 0xC0);             /* xor al, al */
    emit_b(cg, 0xC3);                                /* ret */
    /* .ok: success path */
    emit_b(cg, 0x48); emit_b(cg, 0x83); emit_b(cg, 0xC4); emit_b(cg, 0x20); /* add rsp,32 */
    emit_b(cg, 0x5E); emit_b(cg, 0x5B);             /* pop rsi; pop rbx */
    emit_b(cg, 0x41); emit_b(cg, 0x5E);             /* pop r14 */
    emit_b(cg, 0x41); emit_b(cg, 0x5F);             /* pop r15 */
    emit_b(cg, 0xB0); emit_b(cg, 0x01);             /* mov al, 1 */
    emit_b(cg, 0xC3);                                /* ret */
}

/* LDR Rd, [base_eax + imm]: bus_read(bus, addr, sz, &out) -> r[rd].
   Sequence (~67B):
     ld_eax(rn) + add_imm(imm)     -- addr in eax (skipped for LDR_LIT/LDR_REG callers)
     sub rsp,16                    -- 16B local: [rsp+0..3] = out slot
     WIN64: rcx=bus, rdx=addr, r8d=sz, r9=&out
     mov rax, &bus_read; call rax
     mov ecx, [rsp+0]; add rsp,16
     and ecx, mask (B/H only)
     test al; jnz +5 (skip fail stub)
     or bl,1; jmp +7 (skip store)
     st_ecx(rd)                    -- 7 bytes: success store
*/
static void emit_load_from_eax(codegen_t* cg, u8 rd, u32 sz) {
    sub_rsp_16(cg);                                         /* rsp -= 16; [rsp+0] = out slot */
    mov_rcx_r14(cg);                                        /* rcx = bus */
    mov_edx_eax(cg);                                        /* rdx = addr */
    mov_r8d_imm(cg, sz);                                    /* r8d = size */
    lea_r9_rsp0(cg);                                        /* r9 = &out */
    mov_rax_imm64(cg, (u64)(uintptr_t)bus_read);            /* rax = &bus_read */
    call_rax(cg);                                           /* call bus_read */
    mov_ecx_rsp0(cg);                                       /* ecx = out (before add rsp) */
    add_rsp_16(cg);                                         /* rsp += 16 */
    if (sz == 1u) {
        emit_b(cg, 0x81); emit_b(cg, 0xE1); emit_w32(cg, 0xFFu);     /* and ecx, 0xFF */
    } else if (sz == 2u) {
        emit_b(cg, 0x81); emit_b(cg, 0xE1); emit_w32(cg, 0xFFFFu);   /* and ecx, 0xFFFF */
    }
    /* test al; jnz +5 (skip: or bl,1 (3B) + jmp +7 (2B) = 5B) */
    test_al_jnz(cg, 5u);
    or_bl_1(cg);                                            /* failure: bl |= 1 */
    jmp_short(cg, 7u);                                      /* jump past st_ecx (7B) */
    st_ecx(cg, rd);                                         /* success: r[rd] = ecx (7B) */
}

static void emit_load(codegen_t* cg, u8 rd, u8 rn, u32 imm, u32 sz) {
    ld_eax(cg, rn);
    if (imm) add_imm(cg, imm);
    emit_load_from_eax(cg, rd, sz);
}

/* STR Rd, [base_eax + imm]: bus_write(bus, addr, sz, r[rd]).
   No result store; just check al for fault.
   Sequence (~52B):
     ld_eax(rn) + add_imm(imm)
     sub rsp,16
     WIN64: rcx=bus, rdx=addr, r8d=sz, r9d=r[rd] (masked for B/H)
     mov rax, &bus_write; call rax
     add rsp,16
     test al; jnz +3 (skip or bl,1)
     or bl,1
*/
static void emit_store(codegen_t* cg, u8 rd, u8 rn, u32 imm, u32 sz) {
    ld_eax(cg, rn);
    if (imm) add_imm(cg, imm);
    sub_rsp_16(cg);
    mov_rcx_r14(cg);                                        /* rcx = bus */
    mov_edx_eax(cg);                                        /* rdx = addr */
    mov_r8d_imm(cg, sz);                                    /* r8d = size */
    mov_r9d_reg(cg, rd);                                    /* r9d = r[rd] */
    if (sz == 1u) and_r9d_imm(cg, 0xFFu);
    else if (sz == 2u) and_r9d_imm(cg, 0xFFFFu);
    mov_rax_imm64(cg, (u64)(uintptr_t)bus_write);           /* rax = &bus_write */
    call_rax(cg);
    add_rsp_16(cg);
    /* test al; jnz +3 (skip or bl,1 (3B)) */
    test_al_jnz(cg, 3u);
    or_bl_1(cg);
}

/* Emit B #imm (unconditional): PC = pc+4+imm. Block terminates. */
static void emit_b_uncond(codegen_t* cg, const insn_t* i) {
    st_pc(cg, i->pc + 4u + i->imm);
}

/* Emit T32 BL: LR = (pc+size)|1; PC = (pc+4+imm)&~1. Block terminates. */
static void emit_t32_bl(codegen_t* cg, const insn_t* i) {
    u32 tgt = (i->pc + 4u + i->imm) & ~1u;
    u32 lnk = (i->pc + i->size) | 1u;
    mov_eax_imm(cg, lnk);
    st_eax(cg, REG_LR);
    st_pc(cg, tgt);
}

/* op -> x86 emit. */
bool codegen_supports(opcode_t op) {
    switch (op) {
        case OP_MOV_IMM:    case OP_MOV_REG:
        case OP_ADD_REG:    case OP_SUB_REG:
        case OP_ADD_IMM3:   case OP_ADD_IMM8:
        case OP_SUB_IMM3:   case OP_SUB_IMM8:
        case OP_AND_REG:    case OP_ORR_REG: case OP_EOR_REG:
        case OP_NOP:        case OP_T32_NOP:
        case OP_T32_MOV_IMM:
        case OP_T32_ADD_IMM:
        case OP_T32_SUB_IMM:
        case OP_T32_AND_IMM:
        case OP_T32_ORR_IMM:
        case OP_T32_EOR_IMM:
        case OP_T32_ADDW: case OP_T32_SUBW:
        case OP_T32_MOVW:
        /* flag-only ops (CMP/CMN/TST family) */
        case OP_CMP_IMM:    case OP_CMP_REG:    case OP_CMP_REG_T2:
        case OP_CMN_REG:    case OP_TST_REG:
        case OP_T32_CMP_IMM:    case OP_T32_CMP_REG:
        case OP_T32_CMN_IMM:    case OP_T32_CMN_REG:
        /* memory ops */
        case OP_LDR_IMM:    case OP_STR_IMM:
        case OP_LDRB_IMM:   case OP_STRB_IMM:
        case OP_LDRH_IMM:   case OP_STRH_IMM:
        case OP_LDR_SP:     case OP_STR_SP:
        case OP_LDR_LIT:
        case OP_LDR_REG:    case OP_STR_REG:
        case OP_T32_LDR_IMM:   case OP_T32_STR_IMM:
        case OP_T32_LDRB_IMM:  case OP_T32_STRB_IMM:
        case OP_T32_LDRH_IMM:  case OP_T32_STRH_IMM:
        case OP_T32_LDR_LIT:
        case OP_T32_LDR_REG:   case OP_T32_STR_REG:
        case OP_T32_LDRD_IMM:  case OP_T32_STRD_IMM:
        /* stack / multi-reg */
        case OP_PUSH:
        case OP_POP:
        case OP_T32_LDM:
        case OP_T32_STM:
        /* branch terminators */
        case OP_B_COND:  case OP_T32_B_COND:
        case OP_B_UNCOND:
        case OP_T32_BL:
            return true;
        default:
            return false;
    }
}

/* ---- PUSH/POP helpers ---- */

/* cmp eax, imm32  (3D imm32) 5B */
static void cmp_eax_imm32(codegen_t* cg, u32 v) {
    emit_b(cg, 0x3D); emit_w32(cg, v);
}
/* jcc rel8: opcode + signed byte offset (2B) */
static void jcc_rel8(codegen_t* cg, u8 op, u8 off) {
    emit_b(cg, op); emit_b(cg, off);
}
/* mov r10, imm64  (49 BA imm64)  10B */
static void mov_r10_imm64(codegen_t* cg, u64 v) {
    emit_b(cg, 0x49); emit_b(cg, 0xBA); emit_w64(cg, v);
}
/* mov r11d, eax zero-ext  (41 89 C3)  3B */
static void mov_r11d_eax_zero_ext(codegen_t* cg) {
    emit_b(cg, 0x41); emit_b(cg, 0x89); emit_b(cg, 0xC3);
}
/* add r11, r10  (4D 01 D3)  3B */
static void add_r11_r10(codegen_t* cg) {
    emit_b(cg, 0x4D); emit_b(cg, 0x01); emit_b(cg, 0xD3);
}
/* sub r11, imm32  (49 81 EB imm32)  7B */
static void sub_r11_imm32(codegen_t* cg, u32 v) {
    emit_b(cg, 0x49); emit_b(cg, 0x81); emit_b(cg, 0xEB); emit_w32(cg, v);
}
/* mov [r11 + disp32], ecx  (41 89 8B disp32)  7B */
static void mov_r11_off_ecx(codegen_t* cg, u32 disp) {
    emit_b(cg, 0x41); emit_b(cg, 0x89); emit_b(cg, 0x8B); emit_w32(cg, disp);
}
/* mov ecx, [r11 + disp32]  (41 8B 0B+disp) -> ModRM=0x8B rm=r11 reg=ecx
   Actually: 41 8B 8B disp32  (REX.B=1, mov ecx,[r11+disp32]) */
static void mov_ecx_r11_off(codegen_t* cg, u32 disp) {
    emit_b(cg, 0x41); emit_b(cg, 0x8B); emit_b(cg, 0x8B); emit_w32(cg, disp);
}
/* add edx, imm32  (81 C2 imm32)  6B */
static void add_edx_imm(codegen_t* cg, u32 v) {
    emit_b(cg, 0x81); emit_b(cg, 0xC2); emit_w32(cg, v);
}
/* cmp ecx, imm32  (81 F9 imm32)  6B */
static void cmp_ecx_imm32(codegen_t* cg, u32 v) {
    emit_b(cg, 0x81); emit_b(cg, 0xF9); emit_w32(cg, v);
}
/* and ecx, imm32  (81 E1 imm32)  6B */
static void and_ecx_imm32(codegen_t* cg, u32 v) {
    emit_b(cg, 0x81); emit_b(cg, 0xE1); emit_w32(cg, v);
}
/* OP_PUSH T1 native emitter */
static void emit_push_v(codegen_t* cg, bus_t* b, u16 reg_list) {
    u32 cnt = 0;
    for (int k = 0; k <= 14; k++) if (reg_list & (1u << k)) cnt++;
    if (cnt == 0) return;

    ld_eax(cg, REG_SP);
    sub_imm(cg, cnt * 4u);
    st_eax(cg, REG_SP);

    region_t* sram = b ? bus_find_flat(b, 0x20000000u) : NULL;
    if (sram && sram->buf) {
        u32 fbase = sram->base;
        u32 fsize = sram->size;
        /* fast body: r10_imm64(10) + r11d_eax(3) + add(3) + sub(7) = 23B setup
           + cnt*(ld_ecx(7) + store(7)) = cnt*14B */
        u32 fast_body = 23u + cnt * 14u;
        /* slow body: per-reg: ld_eax(7)+sub_rsp(4)+mov_rcx(3)+mov_edx(2)+add_edx(6)+mov_r8d(6)+
                              mov_r9d(7)+mov_rax(10)+call(2)+add_rsp(4)+test(4)+or(3) = 58B/reg.
           Note: ld_eax is inside the loop to refresh eax after call_rax clobbers it. */
        u32 slow_body = cnt * 58u;
        /* jmp size: 2B if slow fits in rel8, else 5B for near jmp */
        u32 jmp_sz = (slow_body <= 127u) ? 2u : 5u;
        /* jb/ja targets: from end of jcc(2B) to start of slow_path */
        u32 jb_target = 5u + 2u + fast_body + jmp_sz;   /* cmp2(5)+ja(2)+fast+jmp */
        u32 ja_target = fast_body + jmp_sz;              /* fast+jmp */
        if (jb_target > 127u || ja_target > 127u) {
            goto slow_only_push;
        }

        u32 hi = fbase + fsize - cnt * 4u;
        cmp_eax_imm32(cg, fbase);
        jcc_rel8(cg, 0x72, (u8)jb_target);
        cmp_eax_imm32(cg, hi);
        jcc_rel8(cg, 0x77, (u8)ja_target);

        /* fast path */
        mov_r10_imm64(cg, (u64)(uintptr_t)sram->buf);
        mov_r11d_eax_zero_ext(cg);
        add_r11_r10(cg);
        sub_r11_imm32(cg, fbase);
        u32 k_off = 0;
        for (int k = 0; k <= 14; k++) {
            if (!(reg_list & (1u << k))) continue;
            if (k == 14) ld_ecx(cg, REG_LR);
            else         ld_ecx(cg, (u8)k);
            mov_r11_off_ecx(cg, k_off);
            k_off += 4u;
        }
        /* jmp over inline slow path */
        if (slow_body <= 127u) {
            jmp_short(cg, (u8)slow_body);
        } else {
            emit_b(cg, 0xE9); emit_w32(cg, slow_body);
        }

        /* slow path: ld_eax(REG_SP) inside loop to refresh after call_rax clobbers rax */
        k_off = 0;
        for (int k = 0; k <= 14; k++) {
            if (!(reg_list & (1u << k))) continue;
            ld_eax(cg, REG_SP);
            sub_rsp_16(cg);
            mov_rcx_r14(cg);
            mov_edx_eax(cg);
            add_edx_imm(cg, k_off);
            mov_r8d_imm(cg, 4u);
            if (k == 14) mov_r9d_reg(cg, REG_LR);
            else         mov_r9d_reg(cg, (u8)k);
            mov_rax_imm64(cg, (u64)(uintptr_t)bus_write);
            call_rax(cg);
            add_rsp_16(cg);
            test_al_jnz(cg, 3u);
            or_bl_1(cg);
            k_off += 4u;
        }
        return;
    }

slow_only_push:;
    /* slow path only: ld_eax(REG_SP) inside loop to refresh after call_rax clobbers rax */
    u32 k_off = 0;
    for (int k = 0; k <= 14; k++) {
        if (!(reg_list & (1u << k))) continue;
        ld_eax(cg, REG_SP);
        sub_rsp_16(cg);
        mov_rcx_r14(cg);
        mov_edx_eax(cg);
        add_edx_imm(cg, k_off);
        mov_r8d_imm(cg, 4u);
        if (k == 14) mov_r9d_reg(cg, REG_LR);
        else         mov_r9d_reg(cg, (u8)k);
        mov_rax_imm64(cg, (u64)(uintptr_t)bus_write);
        call_rax(cg);
        add_rsp_16(cg);
        test_al_jnz(cg, 3u);
        or_bl_1(cg);
        k_off += 4u;
    }
}

/* Emit POP (OP_POP T1): reg_list bits 0..7=R0..R7 bit15=PC.
   ARM: load from sp ascending, then sp += cnt*4.
   PC special: mask &~1, check EXC_RETURN (top byte 0xFF -> bl fallback), else store to CG_PC_OFF. */
static void emit_pop(codegen_t* cg, bus_t* b, u16 reg_list) {
    u32 cnt = 0;
    for (int k = 0; k <= 15; k++) if (reg_list & (1u << k)) cnt++;
    if (cnt == 0) return;
    bool has_pc = (reg_list & (1u << 15)) != 0;

    /* load current SP */
    ld_eax(cg, REG_SP);   /* eax = sp_old */

    region_t* sram = b ? bus_find_flat(b, 0x20000000u) : NULL;
    if (sram && sram->buf) {
        u32 fbase = sram->base;
        u32 fsize = sram->size;
        /* fast body: setup(23) + cnt*(load_from_r11(7)+store_to_reg_or_pc(7)) + sp_update(7+5+7=19)
           PC slot replaces st_ecx(7B) with cmp(6)+jae(2)+and(6)+store(7)+jmp(2)+or_bl_1(3) = 26B
           extra vs st_ecx = 26-7 = 19B -> +19 */
        u32 fast_body = 23u + cnt * 14u + 19u;
        if (has_pc) fast_body += 6u + 2u + 6u + 7u + 2u + 3u;  /* cmp+jae+and+store+jmp+or_bl_1 */
        /* slow body: per-reg ld_eax(7) inside loop + cnt*(60B non-PC or 56B PC) + sp_update(7+5+7) + pc_commit
           pc_commit: cmp_r10d(7)+jae(2)+and_r10d(7)+store(7)+jmp_short(2)+or_bl_1(3) = 28B
           ld_eax is now INSIDE each loop iteration (7B each) to refresh base after call_rax clobbers rax */
        u32 slow_body = 0u;
        for (int k = 0; k <= 15; k++) {
            if (!(reg_list & (1u << k))) continue;
            slow_body += 7u + ((k == 15) ? 56u : 60u);  /* ld_eax(7) per iter */
        }
        slow_body += 7u + 5u + 7u;
        if (has_pc) slow_body += 7u + 2u + 7u + 7u + 2u + 3u;  /* 28B */
        u32 jmp_sz = (slow_body <= 127u) ? 2u : 5u;
        u32 ja_target = fast_body + jmp_sz;         /* from end of ja(2) to slow */
        u32 jb_target = 5u + 2u + fast_body + jmp_sz; /* from end of jb(2) */
        if (jb_target > 127u || ja_target > 127u) goto slow_only_pop;

        u32 hi = fbase + fsize - cnt * 4u;
        cmp_eax_imm32(cg, fbase);
        jcc_rel8(cg, 0x72, (u8)jb_target);
        cmp_eax_imm32(cg, hi);
        jcc_rel8(cg, 0x77, (u8)ja_target);

        /* fast: r10=buf, r11=buf+(eax-fbase) */
        mov_r10_imm64(cg, (u64)(uintptr_t)sram->buf);
        mov_r11d_eax_zero_ext(cg);
        add_r11_r10(cg);
        sub_r11_imm32(cg, fbase);

        u32 k_off = 0;
        for (int k = 0; k <= 15; k++) {
            if (!(reg_list & (1u << k))) continue;
            mov_ecx_r11_off(cg, k_off);   /* ecx = [r11 + k_off] = mem[sp + k_off] */
            if (k == 15) {
                /* PC: check EXC_RETURN; if so mark failure so interpreter handles it.
                   Layout: cmp(6)+jae+15(2)+and(6)+store(7)+jmp+3(2)+or_bl_1(3) = 26B
                   jae +15 skips and(6)+store(7)+jmp(2)=15B, lands at or_bl_1. */
                cmp_ecx_imm32(cg, 0xFFFFFFF0u);              /* 6B */
                jcc_rel8(cg, 0x73, 6u + 7u + 2u);            /* jae +15 -> or_bl_1 */
                /* not EXC_RETURN: mask Thumb bit, store PC */
                and_ecx_imm32(cg, ~1u);                       /* 6B */
                emit_b(cg, 0x41); emit_b(cg, 0x89); emit_b(cg, 0x8F);
                emit_w32(cg, CG_PC_OFF);                      /* 7B: mov [r15+CG_PC_OFF],ecx */
                jmp_short(cg, 3u);                            /* 2B: skip or_bl_1 */
                or_bl_1(cg);                                  /* 3B: EXC_RETURN -> TB fail */
            } else {
                st_ecx(cg, (u8)k);
            }
            k_off += 4u;
        }
        /* SP += cnt*4 */
        ld_eax(cg, REG_SP);
        add_imm(cg, cnt * 4u);
        st_eax(cg, REG_SP);

        /* jmp over inline slow path (slow_body pre-computed above) */
        if (slow_body <= 127u) {
            jmp_short(cg, (u8)slow_body);
        } else {
            emit_b(cg, 0xE9); emit_w32(cg, slow_body);
        }
        /* slow path (reached via bounds-check fallthrough only): */
        {
            u32 sl_off = 0;
            for (int k = 0; k <= 15; k++) {
                if (!(reg_list & (1u << k))) continue;
                ld_eax(cg, REG_SP);   /* reload: call_rax clobbers rax */
                sub_rsp_16(cg);
                mov_rcx_r14(cg);
                mov_edx_eax(cg);
                add_edx_imm(cg, sl_off);
                mov_r8d_imm(cg, 4u);
                lea_r9_rsp0(cg);
                mov_rax_imm64(cg, (u64)(uintptr_t)bus_read);
                call_rax(cg);
                mov_ecx_rsp0(cg);
                add_rsp_16(cg);
                test_al_jnz(cg, 5u);
                or_bl_1(cg);
                jmp_short(cg, 7u);
                if (k == 15) {
                    emit_b(cg, 0x44); emit_b(cg, 0x89); emit_b(cg, 0xC8); /* mov r10d, ecx */
                } else {
                    st_ecx(cg, (u8)k);
                }
                sl_off += 4u;
            }
        }
        ld_eax(cg, REG_SP);
        add_imm(cg, cnt * 4u);
        st_eax(cg, REG_SP);
        if (has_pc) {
            /* cmp_r10d(7)+jae+16(2)+and_r10d(7)+store(7)+jmp+3(2)+or_bl_1(3) = 28B
               jae +16 skips and(7)+store(7)+jmp(2)=16B, lands at or_bl_1. */
            emit_b(cg, 0x41); emit_b(cg, 0x81); emit_b(cg, 0xFA); emit_w32(cg, 0xFFFFFFF0u); /* 7B */
            jcc_rel8(cg, 0x73, 7u + 7u + 2u);                                                  /* jae +16 */
            emit_b(cg, 0x41); emit_b(cg, 0x81); emit_b(cg, 0xE2); emit_w32(cg, ~1u);          /* 7B */
            emit_b(cg, 0x45); emit_b(cg, 0x89); emit_b(cg, 0x97); emit_w32(cg, CG_PC_OFF);   /* 7B */
            jmp_short(cg, 3u);                                                                  /* 2B */
            or_bl_1(cg);                                                                        /* 3B */
        }
        return;
    }

slow_only_pop:;
    /* slow path only (no flat SRAM or bounds overflow) */
    {
        u32 sp_off = 0;
        for (int k = 0; k <= 15; k++) {
            if (!(reg_list & (1u << k))) continue;
            ld_eax(cg, REG_SP);   /* reload: call_rax clobbers rax */
            sub_rsp_16(cg);
            mov_rcx_r14(cg);
            mov_edx_eax(cg);
            add_edx_imm(cg, sp_off);
            mov_r8d_imm(cg, 4u);
            lea_r9_rsp0(cg);
            mov_rax_imm64(cg, (u64)(uintptr_t)bus_read);
            call_rax(cg);
            mov_ecx_rsp0(cg);
            add_rsp_16(cg);
            test_al_jnz(cg, 5u);
            or_bl_1(cg);
            jmp_short(cg, 7u);
            if (k == 15) {
                emit_b(cg, 0x44); emit_b(cg, 0x89); emit_b(cg, 0xC8); /* mov r10d, ecx */
            } else {
                st_ecx(cg, (u8)k);
            }
            sp_off += 4u;
        }
        ld_eax(cg, REG_SP);
        add_imm(cg, cnt * 4u);
        st_eax(cg, REG_SP);
        if (has_pc) {
            /* cmp_r10d(7)+jae+16(2)+and_r10d(7)+store(7)+jmp+3(2)+or_bl_1(3) = 28B */
            emit_b(cg, 0x41); emit_b(cg, 0x81); emit_b(cg, 0xFA); emit_w32(cg, 0xFFFFFFF0u); /* 7B */
            jcc_rel8(cg, 0x73, 7u + 7u + 2u);                                                  /* jae +16 */
            emit_b(cg, 0x41); emit_b(cg, 0x81); emit_b(cg, 0xE2); emit_w32(cg, ~1u);          /* 7B */
            emit_b(cg, 0x45); emit_b(cg, 0x89); emit_b(cg, 0x97); emit_w32(cg, CG_PC_OFF);   /* 7B */
            jmp_short(cg, 3u);                                                                  /* 2B */
            or_bl_1(cg);                                                                        /* 3B */
        }
    }
}

/* Emit T32 LDM/STM: IA or DB, with optional writeback, 16-bit reg_list.
   is_load: true=LDM, false=STM; is_db: true=DecrementBefore; writeback: update rn. */
static void emit_ldm_stm(codegen_t* cg, bus_t* b, u8 rn, u16 reg_list,
                          bool is_load, bool is_db, bool writeback, u32 insn_pc) {
    u32 cnt = 0;
    for (int k = 0; k <= 15; k++) if (reg_list & (1u << k)) cnt++;
    if (cnt == 0) return;
    bool has_pc = is_load && (reg_list & (1u << 15));

    /* eax = base = c->r[rn] */
    ld_eax(cg, rn);
    /* if DB: eax -= cnt*4 (compute start address) */
    if (is_db) sub_imm(cg, cnt * 4u);
    /* eax = start address for all accesses */

    region_t* sram = b ? bus_find_flat(b, 0x20000000u) : NULL;
    if (sram && sram->buf) {
        u32 fbase = sram->base;
        u32 fsize = sram->size;
        /* fast body = setup(23) + per_reg + writeback(19 if wb) */
        u32 fast_body = 23u;
        for (int k = 0; k <= 15; k++) {
            if (!(reg_list & (1u << k))) continue;
            if (is_load) {
                fast_body += 14u; /* mov_ecx_r11_off(7)+st_ecx(7) */
                if (k == 15) fast_body += 6u+2u+6u+7u+2u+3u - 7u; /* replace st_ecx(7) with cmp+jae+and+store+jmp+or_bl_1 */
            } else {
                if (k == 15) {
                    fast_body += 5u + 7u + 7u + 5u; /* mov_eax_imm(5)+mov_r11_off_eax(7)+ld_eax(7)+sub_imm?(5) */
                    if (!is_db) fast_body -= 5u; /* sub_imm only if is_db */
                } else {
                    fast_body += 14u; /* ld_ecx(7)+mov_r11_off_ecx(7) */
                }
            }
        }
        if (writeback) fast_body += 7u + 5u + 7u; /* ld_eax+add/sub+st_eax */
        /* slow body: per-reg ld_eax+optional sub_imm inside loop to refresh base after call_rax clobbers rax.
           ld_eax(7) + optional sub_imm(5 if is_db) per iteration; no ld_eax before loop. */
        u32 slow_body = 0u;
        for (int k = 0; k <= 15; k++) {
            if (!(reg_list & (1u << k))) continue;
            slow_body += 7u;                           /* ld_eax per iter */
            if (is_db) slow_body += 5u;                /* sub_imm per iter */
            slow_body += is_load ? ((k == 15) ? 56u : 60u)
                                 : ((k == 15) ? 50u : 51u);
        }
        if (writeback) slow_body += 7u+5u+7u;
        if (has_pc) slow_body += 7u + 2u + 7u + 7u + 2u + 3u;  /* cmp+jae+and+store+jmp+or_bl_1=28B */
        u32 jmp_sz = (slow_body <= 127u) ? 2u : 5u;
        u32 ja_target = fast_body + jmp_sz;
        u32 jb_target = 5u + 2u + fast_body + jmp_sz;
        if (jb_target > 127u || ja_target > 127u) goto slow_only_ldm;

        u32 hi = fbase + fsize - cnt * 4u;
        cmp_eax_imm32(cg, fbase);
        jcc_rel8(cg, 0x72, (u8)jb_target);
        cmp_eax_imm32(cg, hi);
        jcc_rel8(cg, 0x77, (u8)ja_target);

        /* fast: r11 = buf + (eax - fbase) */
        mov_r10_imm64(cg, (u64)(uintptr_t)sram->buf);
        mov_r11d_eax_zero_ext(cg);
        add_r11_r10(cg);
        sub_r11_imm32(cg, fbase);

        u32 k_off = 0;
        for (int k = 0; k <= 15; k++) {
            if (!(reg_list & (1u << k))) continue;
            if (is_load) {
                mov_ecx_r11_off(cg, k_off);
                if (k == 15) {
                    /* EXC_RETURN: mark failure; else mask Thumb bit and store PC.
                       cmp(6)+jae+15(2)+and(6)+store(7)+jmp+3(2)+or_bl_1(3) = 26B */
                    cmp_ecx_imm32(cg, 0xFFFFFFF0u);              /* 6B */
                    jcc_rel8(cg, 0x73, 6u + 7u + 2u);            /* jae +15 -> or_bl_1 */
                    and_ecx_imm32(cg, ~1u);                       /* 6B */
                    emit_b(cg, 0x41); emit_b(cg, 0x89); emit_b(cg, 0x8F);
                    emit_w32(cg, CG_PC_OFF);                      /* 7B */
                    jmp_short(cg, 3u);                            /* 2B: skip or_bl_1 */
                    or_bl_1(cg);                                  /* 3B */
                } else {
                    st_ecx(cg, (u8)k);
                }
            } else {
                /* STM */
                if (k == 15) {
                    /* STM with PC: store compile-time insn_pc+4 */
                    mov_eax_imm(cg, insn_pc + 4u);
                    /* mov [r11+k_off], eax */
                    emit_b(cg, 0x41); emit_b(cg, 0x89); emit_b(cg, 0x83); emit_w32(cg, k_off);
                    ld_eax(cg, rn);   /* restore eax=base after mov_eax_imm clobbered it */
                    if (is_db) sub_imm(cg, cnt * 4u);
                } else {
                    ld_ecx(cg, (u8)k);
                    mov_r11_off_ecx(cg, k_off);
                }
            }
            k_off += 4u;
        }

        /* writeback */
        if (writeback) {
            /* IA: rn = start + cnt*4 = start + k_off */
            /* DB: rn = start (already eax if is_db) */
            ld_eax(cg, rn);
            if (is_db) {
                sub_imm(cg, cnt * 4u);
                st_eax(cg, rn);
            } else {
                add_imm(cg, cnt * 4u);
                st_eax(cg, rn);
            }
        }

        /* jmp over inline slow path (slow_body pre-computed above) */
        if (slow_body <= 127u) {
            jmp_short(cg, (u8)slow_body);
        } else {
            emit_b(cg, 0xE9); emit_w32(cg, slow_body);
        }
        /* inline slow path for bounds-fail case */
        /* ld_eax + optional sub_imm now INSIDE loop to refresh base after call_rax clobbers rax */
        {
            u32 sl_off = 0;
            for (int k = 0; k <= 15; k++) {
                if (!(reg_list & (1u << k))) continue;
                ld_eax(cg, rn);
                if (is_db) sub_imm(cg, cnt * 4u);
                if (is_load) {
                    sub_rsp_16(cg); mov_rcx_r14(cg); mov_edx_eax(cg); add_edx_imm(cg, sl_off);
                    mov_r8d_imm(cg, 4u); lea_r9_rsp0(cg);
                    mov_rax_imm64(cg, (u64)(uintptr_t)bus_read); call_rax(cg);
                    mov_ecx_rsp0(cg); add_rsp_16(cg);
                    test_al_jnz(cg, 5u); or_bl_1(cg); jmp_short(cg, 7u);
                    if (k == 15) { emit_b(cg, 0x44); emit_b(cg, 0x89); emit_b(cg, 0xC8); }
                    else         { st_ecx(cg, (u8)k); }
                } else {
                    sub_rsp_16(cg); mov_rcx_r14(cg); mov_edx_eax(cg); add_edx_imm(cg, sl_off);
                    mov_r8d_imm(cg, 4u);
                    if (k == 15) { emit_b(cg, 0x41); emit_b(cg, 0xB9); emit_w32(cg, insn_pc + 4u); }
                    else         { mov_r9d_reg(cg, (u8)k); }
                    mov_rax_imm64(cg, (u64)(uintptr_t)bus_write); call_rax(cg);
                    add_rsp_16(cg); test_al_jnz(cg, 3u); or_bl_1(cg);
                }
                sl_off += 4u;
            }
        }
        if (writeback) {
            ld_eax(cg, rn);
            if (is_db) { sub_imm(cg, cnt * 4u); st_eax(cg, rn); }
            else        { add_imm(cg, cnt * 4u); st_eax(cg, rn); }
        }
        if (has_pc) {
            /* cmp_r10d(7)+jae+16(2)+and_r10d(7)+store(7)+jmp+3(2)+or_bl_1(3) = 28B */
            emit_b(cg, 0x41); emit_b(cg, 0x81); emit_b(cg, 0xFA); emit_w32(cg, 0xFFFFFFF0u); /* 7B */
            jcc_rel8(cg, 0x73, 7u + 7u + 2u);                                                  /* jae +16 */
            emit_b(cg, 0x41); emit_b(cg, 0x81); emit_b(cg, 0xE2); emit_w32(cg, ~1u);          /* 7B */
            emit_b(cg, 0x45); emit_b(cg, 0x89); emit_b(cg, 0x97); emit_w32(cg, CG_PC_OFF);   /* 7B */
            jmp_short(cg, 3u);                                                                  /* 2B */
            or_bl_1(cg);                                                                        /* 3B */
        }
        return;
    }

slow_only_ldm:;
    /* ld_eax + optional sub_imm now INSIDE loop to refresh base after call_rax clobbers rax */
    {
        u32 k_off = 0;
        for (int k = 0; k <= 15; k++) {
            if (!(reg_list & (1u << k))) continue;
            ld_eax(cg, rn);
            if (is_db) sub_imm(cg, cnt * 4u);
            if (is_load) {
                sub_rsp_16(cg);
                mov_rcx_r14(cg);
                mov_edx_eax(cg);
                add_edx_imm(cg, k_off);
                mov_r8d_imm(cg, 4u);
                lea_r9_rsp0(cg);
                mov_rax_imm64(cg, (u64)(uintptr_t)bus_read);
                call_rax(cg);
                mov_ecx_rsp0(cg);
                add_rsp_16(cg);
                test_al_jnz(cg, 5u);
                or_bl_1(cg);
                jmp_short(cg, 7u);
                if (k == 15) {
                    emit_b(cg, 0x44); emit_b(cg, 0x89); emit_b(cg, 0xC8); /* mov r10d, ecx (save PC) */
                } else {
                    st_ecx(cg, (u8)k);
                }
            } else {
                sub_rsp_16(cg);
                mov_rcx_r14(cg);
                mov_edx_eax(cg);
                add_edx_imm(cg, k_off);
                mov_r8d_imm(cg, 4u);
                if (k == 15) {
                    /* STM PC: write insn_pc+4 */
                    emit_b(cg, 0x41); emit_b(cg, 0xB9); emit_w32(cg, insn_pc + 4u); /* mov r9d, imm32 */
                } else {
                    mov_r9d_reg(cg, (u8)k);
                }
                mov_rax_imm64(cg, (u64)(uintptr_t)bus_write);
                call_rax(cg);
                add_rsp_16(cg);
                test_al_jnz(cg, 3u);
                or_bl_1(cg);
            }
            k_off += 4u;
        }
        /* writeback */
        if (writeback) {
            ld_eax(cg, rn);
            if (is_db) {
                sub_imm(cg, cnt * 4u);
                st_eax(cg, rn);
            } else {
                add_imm(cg, cnt * 4u);
                st_eax(cg, rn);
            }
        }
        /* PC commit for LDM with PC */
        if (has_pc) {
            /* cmp_r10d(7)+jae+16(2)+and_r10d(7)+store(7)+jmp+3(2)+or_bl_1(3) = 28B */
            emit_b(cg, 0x41); emit_b(cg, 0x81); emit_b(cg, 0xFA); emit_w32(cg, 0xFFFFFFF0u); /* 7B */
            jcc_rel8(cg, 0x73, 7u + 7u + 2u);                                                  /* jae +16 */
            emit_b(cg, 0x41); emit_b(cg, 0x81); emit_b(cg, 0xE2); emit_w32(cg, ~1u);          /* 7B */
            emit_b(cg, 0x45); emit_b(cg, 0x89); emit_b(cg, 0x97); emit_w32(cg, CG_PC_OFF);   /* 7B */
            jmp_short(cg, 3u);                                                                  /* 2B */
            or_bl_1(cg);                                                                        /* 3B */
        }
    }
}

/* ---- B.cond fast-path helpers (no pushfq/popfq) ---- */

/* mov al, [r15 + CG_APSR_OFF + 3]  -> 41 8A 87 disp32 (7B) */
static void ld_al_apsr_byte3(codegen_t* cg) {
    emit_b(cg, 0x41); emit_b(cg, 0x8A); emit_b(cg, 0x87);
    emit_w32(cg, CG_APSR_OFF + 3u);
}
/* shr al, n  -> C0 E8 n (3B) */
static void shr_al(codegen_t* cg, u8 n) {
    emit_b(cg, 0xC0); emit_b(cg, 0xE8); emit_b(cg, n);
}
/* test al, imm8 -> A8 imm8 (2B) */
static void test_al_imm8(codegen_t* cg, u8 m) {
    emit_b(cg, 0xA8); emit_b(cg, m);
}
/* and al, imm8 -> 24 imm8 (2B) */
static void and_al_imm8(codegen_t* cg, u8 m) {
    emit_b(cg, 0x24); emit_b(cg, m);
}
/* cmp al, imm8 -> 3C imm8 (2B) */
static void cmp_al_imm8(codegen_t* cg, u8 m) {
    emit_b(cg, 0x3C); emit_b(cg, m);
}
/* mov ah, al  -> 88 C4 (2B) */
static void mov_ah_al(codegen_t* cg) { emit_b(cg, 0x88); emit_b(cg, 0xC4); }
/* shr ah, n  -> C0 EC n (3B) */
static void shr_ah(codegen_t* cg, u8 n) {
    emit_b(cg, 0xC0); emit_b(cg, 0xEC); emit_b(cg, n);
}
/* xor al, ah -> 30 E0 (2B) */
static void xor_al_ah(codegen_t* cg) { emit_b(cg, 0x30); emit_b(cg, 0xE0); }
/* jcc rel32: 0F 8X disp32 (6B) */
static void emit_jcc_rel32(codegen_t* cg, u8 jcc, u32 rel32) {
    emit_b(cg, 0x0F); emit_b(cg, jcc); emit_w32(cg, rel32);
}

/* B.cond fast path: mov al,[r15+APSR+3]; optional shr/test/and/cmp/xor; jcc rel32.
   Layout: cond-test + jcc rel32(13) + st_pc(fall,11B) + jmp_short(11B) + st_pc(taken,11B).
   al layout (byte3 of apsr): bit7=N bit6=Z bit5=C bit4=V. After shr 4: bit3=N bit2=Z bit1=C bit0=V. */
static void emit_b_cond_fast(codegen_t* cg, const insn_t* i) {
    if (i->cond == 0xE) { st_pc(cg, i->pc + 4u + i->imm); return; }
    if (i->cond == 0xF) { st_pc(cg, i->pc + i->size);     return; }

    u32 tgt = i->pc + 4u + i->imm;
    u32 fal = i->pc + i->size;

    ld_al_apsr_byte3(cg);                        /* 7B */
    bool need_low = (i->cond >= 0x8);
    if (need_low) shr_al(cg, 4u);               /* 3B: al = 0bNZCV */

    u8 jcc_op;
    switch (i->cond) {
        /* simple bit-test on byte3 (no shr); test sets ZF; jnz=taken-when-bit-set */
        case 0x0: test_al_imm8(cg, 0x40); jcc_op = 0x85; break; /* EQ: Z bit -> jne if Z=1 */
        case 0x1: test_al_imm8(cg, 0x40); jcc_op = 0x84; break; /* NE: jz  if Z=0 */
        case 0x2: test_al_imm8(cg, 0x20); jcc_op = 0x85; break; /* CS: jne if C=1 */
        case 0x3: test_al_imm8(cg, 0x20); jcc_op = 0x84; break; /* CC: jz  if C=0 */
        case 0x4: test_al_imm8(cg, 0x80); jcc_op = 0x85; break; /* MI: jne if N=1 */
        case 0x5: test_al_imm8(cg, 0x80); jcc_op = 0x84; break; /* PL: jz  if N=0 */
        case 0x6: test_al_imm8(cg, 0x10); jcc_op = 0x85; break; /* VS: jne if V=1 */
        case 0x7: test_al_imm8(cg, 0x10); jcc_op = 0x84; break; /* VC: jz  if V=0 */
        /* composite on low nibble (after shr 4): al = 0bNZCV */
        case 0x8: /* HI: C=1 AND Z=0 -> al&0x06==0x02 */
            and_al_imm8(cg, 0x06); cmp_al_imm8(cg, 0x02); jcc_op = 0x84; break;
        case 0x9: /* LS: C=0 OR Z=1 */
            and_al_imm8(cg, 0x06); cmp_al_imm8(cg, 0x02); jcc_op = 0x85; break;
        case 0xA: /* GE: N==V */
            mov_ah_al(cg); shr_ah(cg, 3u); xor_al_ah(cg);
            test_al_imm8(cg, 0x01); jcc_op = 0x84; break;
        case 0xB: /* LT: N!=V */
            mov_ah_al(cg); shr_ah(cg, 3u); xor_al_ah(cg);
            test_al_imm8(cg, 0x01); jcc_op = 0x85; break;
        case 0xC: /* GT: Z=0 AND N==V */
            mov_ah_al(cg); shr_ah(cg, 3u); xor_al_ah(cg);
            test_al_imm8(cg, 0x05); jcc_op = 0x84; break;
        case 0xD: /* LE: Z=1 OR N!=V */
            mov_ah_al(cg); shr_ah(cg, 3u); xor_al_ah(cg);
            test_al_imm8(cg, 0x05); jcc_op = 0x85; break;
        default: return;
    }
    /* jcc rel32=13: skip 11B(fall st_pc) + 2B(jmp_short) = land at taken st_pc */
    emit_jcc_rel32(cg, jcc_op, 13u);
    st_pc(cg, fal);        /* 11B */
    jmp_short(cg, 11u);    /* 2B: skip taken */
    st_pc(cg, tgt);        /* 11B */
}

static void emit_op(codegen_t* cg, bus_t* b, const insn_t* i) {  /* NOLINT */
    switch (i->op) {
        case OP_NOP: case OP_T32_NOP: break;

        /* MOV: T1 MOV_IMM always sets NZ; T32 MOV_IMM/MOVW conditional on set_flags */
        case OP_MOV_IMM:
            mov_eax_imm(cg, i->imm);
            /* test eax,eax (85 C0) to set NZ flags; T1 always sets */
            emit_b(cg, 0x85); emit_b(cg, 0xC0);
            emit_flags_nz(cg);
            st_eax(cg, i->rd); break;

        case OP_T32_MOV_IMM:
            mov_eax_imm(cg, i->imm);
            if (i->set_flags) {
                emit_b(cg, 0x85); emit_b(cg, 0xC0);
                emit_flags_nz(cg);
            }
            st_eax(cg, i->rd); break;

        case OP_T32_MOVW:
            mov_eax_imm(cg, i->imm);
            st_eax(cg, i->rd); break;   /* MOVW never sets flags */

        case OP_MOV_REG:
            ld_eax(cg, i->rm);
            if (i->set_flags) {
                emit_b(cg, 0x85); emit_b(cg, 0xC0);
                emit_flags_nz(cg);
            }
            st_eax(cg, i->rd); break;

        /* T1 ADD_REG/SUB_REG: always set flags outside IT */
        case OP_ADD_REG:
            ld_eax(cg, i->rn); ld_ecx(cg, i->rm); op_add_ec(cg);
            emit_flags_nzcv(cg, false);
            st_eax(cg, i->rd); break;

        case OP_SUB_REG:
            ld_eax(cg, i->rn); ld_ecx(cg, i->rm); op_sub_ec(cg);
            emit_flags_nzcv(cg, true);
            st_eax(cg, i->rd); break;

        /* T1 ADD_IMM3/IMM8: always set flags */
        case OP_ADD_IMM3:
            ld_eax(cg, i->rn); add_imm(cg, i->imm);
            emit_flags_nzcv(cg, false);
            st_eax(cg, i->rd); break;

        case OP_ADD_IMM8:
            ld_eax(cg, i->rn); add_imm(cg, i->imm);
            emit_flags_nzcv(cg, false);
            st_eax(cg, i->rd); break;

        /* T1 SUB_IMM3/IMM8: always set flags */
        case OP_SUB_IMM3:
            ld_eax(cg, i->rn); sub_imm(cg, i->imm);
            emit_flags_nzcv(cg, true);
            st_eax(cg, i->rd); break;

        case OP_SUB_IMM8:
            ld_eax(cg, i->rn); sub_imm(cg, i->imm);
            emit_flags_nzcv(cg, true);
            st_eax(cg, i->rd); break;

        /* T32 ADD_IMM: S-bit conditional */
        case OP_T32_ADD_IMM:
            ld_eax(cg, i->rn); add_imm(cg, i->imm);
            if (i->set_flags) emit_flags_nzcv(cg, false);
            st_eax(cg, i->rd); break;

        /* T32 ADDW: T4 encoding, no S bit, never sets flags */
        case OP_T32_ADDW:
            ld_eax(cg, i->rn); add_imm(cg, i->imm);
            st_eax(cg, i->rd); break;

        /* T32 SUB_IMM: S-bit conditional */
        case OP_T32_SUB_IMM:
            ld_eax(cg, i->rn); sub_imm(cg, i->imm);
            if (i->set_flags) emit_flags_nzcv(cg, true);
            st_eax(cg, i->rd); break;

        /* T32 SUBW: T4 encoding, no S bit, never sets flags */
        case OP_T32_SUBW:
            ld_eax(cg, i->rn); sub_imm(cg, i->imm);
            st_eax(cg, i->rd); break;

        /* T1 AND_REG: always sets NZ */
        case OP_AND_REG:
            ld_eax(cg, i->rn); ld_ecx(cg, i->rm); op_and_ec(cg);
            emit_flags_nz(cg);
            st_eax(cg, i->rd); break;

        case OP_T32_AND_IMM:
            ld_eax(cg, i->rn);
            emit_b(cg, 0x25); emit_w32(cg, i->imm);
            if (i->set_flags) emit_flags_nz(cg);
            st_eax(cg, i->rd); break;

        /* T1 ORR_REG: always sets NZ */
        case OP_ORR_REG:
            ld_eax(cg, i->rn); ld_ecx(cg, i->rm); op_or_ec(cg);
            emit_flags_nz(cg);
            st_eax(cg, i->rd); break;

        case OP_T32_ORR_IMM:
            ld_eax(cg, i->rn);
            emit_b(cg, 0x0D); emit_w32(cg, i->imm);
            if (i->set_flags) emit_flags_nz(cg);
            st_eax(cg, i->rd); break;

        /* T1 EOR_REG: always sets NZ */
        case OP_EOR_REG:
            ld_eax(cg, i->rn); ld_ecx(cg, i->rm); op_xor_ec(cg);
            emit_flags_nz(cg);
            st_eax(cg, i->rd); break;

        case OP_T32_EOR_IMM:
            ld_eax(cg, i->rn);
            emit_b(cg, 0x35); emit_w32(cg, i->imm);
            if (i->set_flags) emit_flags_nz(cg);
            st_eax(cg, i->rd); break;

        /* CMP family: compute result, set NZCV, discard result */
        case OP_CMP_IMM: case OP_T32_CMP_IMM:
            ld_eax(cg, i->rn);
            sub_imm(cg, i->imm);
            emit_flags_nzcv(cg, true);
            break;  /* no store */

        case OP_CMP_REG: case OP_CMP_REG_T2: case OP_T32_CMP_REG:
            ld_eax(cg, i->rn); ld_ecx(cg, i->rm);
            op_sub_ec(cg);
            emit_flags_nzcv(cg, true);
            break;  /* no store */

        case OP_CMN_REG: case OP_T32_CMN_REG:
            ld_eax(cg, i->rn); ld_ecx(cg, i->rm);
            op_add_ec(cg);
            emit_flags_nzcv(cg, false);
            break;  /* no store */

        case OP_T32_CMN_IMM:
            ld_eax(cg, i->rn);
            add_imm(cg, i->imm);
            emit_flags_nzcv(cg, false);
            break;  /* no store */

        case OP_TST_REG:
            ld_eax(cg, i->rn); ld_ecx(cg, i->rm);
            op_and_ec(cg);
            emit_flags_nz(cg);
            break;  /* no store; TST = AND with discard, NZ only */

        /* === LDR/STR immediate offset === */
        case OP_LDR_IMM:    case OP_T32_LDR_IMM:
            emit_load(cg, i->rd, i->rn, i->imm, 4u); break;
        case OP_LDRB_IMM:   case OP_T32_LDRB_IMM:
            emit_load(cg, i->rd, i->rn, i->imm, 1u); break;
        case OP_LDRH_IMM:   case OP_T32_LDRH_IMM:
            emit_load(cg, i->rd, i->rn, i->imm, 2u); break;
        case OP_STR_IMM:    case OP_T32_STR_IMM:
            emit_store(cg, i->rd, i->rn, i->imm, 4u); break;
        case OP_STRB_IMM:   case OP_T32_STRB_IMM:
            emit_store(cg, i->rd, i->rn, i->imm, 1u); break;
        case OP_STRH_IMM:   case OP_T32_STRH_IMM:
            emit_store(cg, i->rd, i->rn, i->imm, 2u); break;

        /* SP-relative */
        case OP_LDR_SP:
            emit_load(cg, i->rd, REG_SP, i->imm, 4u); break;
        case OP_STR_SP:
            emit_store(cg, i->rd, REG_SP, i->imm, 4u); break;

        /* LDR_LIT: addr = ((PC+4)&~3) + imm; baked at compile time */
        case OP_LDR_LIT:    case OP_T32_LDR_LIT: {
            u32 a = ((i->pc + 4u) & ~3u) + i->imm;
            mov_eax_imm(cg, a);                             /* eax = literal addr */
            emit_load_from_eax(cg, i->rd, 4u);
            break;
        }

        /* Register-offset LDR/STR */
        case OP_LDR_REG:    case OP_T32_LDR_REG:
            ld_eax(cg, i->rn); ld_ecx(cg, i->rm); op_add_ec(cg);
            emit_load_from_eax(cg, i->rd, 4u);
            break;
        case OP_STR_REG:    case OP_T32_STR_REG: {
            ld_eax(cg, i->rn); ld_ecx(cg, i->rm); op_add_ec(cg);
            sub_rsp_16(cg);
            mov_rcx_r14(cg); mov_edx_eax(cg); mov_r8d_imm(cg, 4u);
            mov_r9d_reg(cg, i->rd);
            mov_rax_imm64(cg, (u64)(uintptr_t)bus_write);
            call_rax(cg);
            add_rsp_16(cg);
            test_al_jnz(cg, 3u); or_bl_1(cg);
            break;
        }

        /* LDRD: two consecutive loads; second reg is i->rs */
        case OP_T32_LDRD_IMM:
            emit_load(cg, i->rd, i->rn, i->imm,      4u);
            emit_load(cg, i->rs, i->rn, i->imm + 4u, 4u);
            break;

        /* STRD: two consecutive stores */
        case OP_T32_STRD_IMM:
            emit_store(cg, i->rd, i->rn, i->imm,      4u);
            emit_store(cg, i->rs, i->rn, i->imm + 4u, 4u);
            break;

        /* stack ops */
        case OP_PUSH:
            emit_push_v(cg, b, (u16)i->reg_list); break;
        case OP_POP:
            emit_pop(cg, b, (u16)i->reg_list); break;

        /* T32 LDM/STM */
        case OP_T32_LDM:
        case OP_T32_STM: {
            bool is_load = (i->op == OP_T32_LDM);
            emit_ldm_stm(cg, b, i->rn, (u16)i->reg_list, is_load, !i->add, i->writeback, i->pc);
            break;
        }

        /* branch terminators */
        case OP_B_UNCOND:
            emit_b_uncond(cg, i); break;
        case OP_B_COND: case OP_T32_B_COND:
            emit_b_cond_fast(cg, i); break;
        case OP_T32_BL:
            emit_t32_bl(cg, i); break;

        default: break;
    }
}

bool codegen_init(codegen_t* cg) {
    cg->capacity = CG_BUFFER_SIZE;
    cg->used = 0;
#ifdef _WIN32
    cg->buffer = (u8*)VirtualAlloc(NULL, CG_BUFFER_SIZE,
                                   MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
#else
    cg->buffer = (u8*)mmap(NULL, CG_BUFFER_SIZE,
                           PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cg->buffer == MAP_FAILED) cg->buffer = NULL;
#endif
    return cg->buffer != NULL;
}

void codegen_free(codegen_t* cg) {
    if (!cg->buffer) return;
#ifdef _WIN32
    VirtualFree(cg->buffer, 0, MEM_RELEASE);
#else
    munmap(cg->buffer, cg->capacity);
#endif
    cg->buffer = NULL;
}

/* T32 memory ops (T4 form) carry writeback/index/add fields that emit_load/emit_store
   does not implement.  Only the simple T3 form (add=1,index=1,writeback=0) is safe
   to compile natively.  Return false for any insn that needs the interpreter. */
static bool insn_native_ok(const insn_t* i) {
    switch (i->op) {
        case OP_T32_LDR_IMM:  case OP_T32_STR_IMM:
        case OP_T32_LDRB_IMM: case OP_T32_STRB_IMM:
        case OP_T32_LDRH_IMM: case OP_T32_STRH_IMM:
        case OP_T32_LDRD_IMM: case OP_T32_STRD_IMM:
            return i->add && i->index && !i->writeback;
        /* POP/LDM with PC in reg_list may produce an EXC_RETURN value.
           The epilogue always sets c->halted on bl=1, so we cannot use the
           or_bl_1 TB-fail path to hand off to the interpreter safely.
           Refuse native compilation; let the interpreter call exc_return(). */
        case OP_POP:
        case OP_T32_LDM:
            return (i->reg_list & (1u << 15)) == 0;
        default:
            return true;
    }
}

cg_thunk_t codegen_emit(codegen_t* cg, bus_t* b, const insn_t* ins, u8 n) {
    for (u8 k = 0; k < n; ++k) if (!codegen_supports(ins[k].op)) return NULL;
    for (u8 k = 0; k < n; ++k) if (!insn_native_ok(&ins[k]))     return NULL;
    if (cg->used + (u32)n * 160u + 64u > cg->capacity) return NULL;
    u8* start = cg->buffer + cg->used;
    emit_prologue(cg);
    emit_clear_fail(cg);                /* xor ebx,ebx — failure flag */
    for (u8 k = 0; k < n; ++k) emit_op(cg, b, &ins[k]);
    /* Branch terminators write PC internally; all others need explicit trailing st_pc.
       OP_POP / OP_T32_LDM are terminators in is_terminator() but insn_native_ok()
       guarantees they only reach here without PC in reg_list - so they never write
       CG_PC_OFF. Emit the fall-through PC after them just like any non-branch insn. */
    opcode_t last_op = ins[n - 1].op;
    bool wrote_pc = (last_op == OP_B_COND    || last_op == OP_T32_B_COND ||
                     last_op == OP_B_UNCOND  || last_op == OP_T32_BL);
    if (!wrote_pc) {
        u32 last = ins[n - 1].pc + ins[n - 1].size;
        st_pc(cg, last);
    }
    emit_epilogue_check(cg);            /* bl-check: halt-or-success */
    return (cg_thunk_t)(uintptr_t)start;
}
