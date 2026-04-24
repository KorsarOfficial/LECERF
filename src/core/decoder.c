#include "core/decoder.h"
#include "core/bus.h"

/* Minimal Thumb-1 decoder skeleton. Grows with each phase.
   Full table in ARM ARM A5.2 (ARMv7-M, The Thumb instruction set encoding).
   Layout: bits 15:10 select top-level group. */

static void set_undef(insn_t* i, u32 raw, addr_t pc) {
    i->op = OP_UNDEFINED; i->pc = pc; i->size = 2; i->raw = raw;
    i->rd = i->rn = i->rm = i->rs = 0; i->imm = 0; i->cond = 0xE; i->reg_list = 0;
}

static u8 decode_thumb16(u16 w, addr_t pc, insn_t* out) {
    set_undef(out, w, pc);
    out->raw = w;

    /* ARM ARM A5.2.1: bit[15:11] discriminator.
       00011 = ADD/SUB reg/imm3, else 000xx = shift-immediate. */

    /* ADD/SUB register or immediate (3-bit): bit[15:11] = 00011 */
    if ((w & 0xF800u) == 0x1800u) {
        u8 i_bit = (w >> 10) & 1;
        u8 opc   = (w >> 9)  & 1;
        out->rn = (w >> 3) & 0x7;
        out->rd = w & 0x7;
        if (i_bit == 0) {
            out->rm = (w >> 6) & 0x7;
            out->op = opc ? OP_SUB_REG : OP_ADD_REG;
        } else {
            out->imm = (w >> 6) & 0x7;
            out->op = opc ? OP_SUB_IMM3 : OP_ADD_IMM3;
        }
        return 2;
    }

    /* Shift (immediate): bit[15:13] = 000 and NOT add/sub reg/imm3. */
    if ((w & 0xE000u) == 0x0000u) {
        u8 op = (w >> 11) & 0x3;
        u8 imm5 = (w >> 6) & 0x1F;
        out->rm = (w >> 3) & 0x7;
        out->rd = w & 0x7;
        out->imm = imm5;
        if (op == 0) out->op = OP_LSL_IMM;
        else if (op == 1) out->op = OP_LSR_IMM;
        else              out->op = OP_ASR_IMM;
        return 2;
    }

    /* Move/compare/add/subtract immediate (8-bit) */
    if ((w & 0xE000u) == 0x2000u) {
        u8 op = (w >> 11) & 0x3;
        out->rd = (w >> 8) & 0x7;
        out->rn = out->rd;
        out->imm = w & 0xFF;
        switch (op) {
            case 0: out->op = OP_MOV_IMM;  break;
            case 1: out->op = OP_CMP_IMM;  break;
            case 2: out->op = OP_ADD_IMM8; break;
            case 3: out->op = OP_SUB_IMM8; break;
        }
        return 2;
    }

    /* Data-processing (register) — ARM ARM A5.2.2 */
    if ((w & 0xFC00u) == 0x4000u) {
        u8 op = (w >> 6) & 0xF;
        out->rm = (w >> 3) & 0x7;
        out->rd = w & 0x7;
        out->rn = out->rd;
        static const opcode_t ops[16] = {
            OP_AND_REG, OP_EOR_REG, OP_LSL_REG, OP_LSR_REG,
            OP_ASR_REG, OP_ADC_REG, OP_SBC_REG, OP_ROR_REG,
            OP_TST_REG, OP_RSB_IMM, OP_CMP_REG, OP_CMN_REG,
            OP_ORR_REG, OP_MUL,     OP_BIC_REG, OP_MVN_REG
        };
        out->op = ops[op];
        return 2;
    }

    /* Special data/branch — ARM ARM A5.2.3: ADD/CMP/MOV hi-regs, BX/BLX */
    if ((w & 0xFC00u) == 0x4400u) {
        u8 opc = (w >> 8) & 0x3;
        u8 DN  = (w >> 7) & 0x1;
        u8 Rm  = (w >> 3) & 0xF;
        u8 Rdn = (DN << 3) | (w & 0x7);
        out->rm = Rm;
        if (opc == 0) {                 /* ADD */
            out->rn = Rdn; out->rd = Rdn; out->op = OP_ADD_REG_T2;
        } else if (opc == 1) {          /* CMP */
            out->rn = Rdn; out->op = OP_CMP_REG_T2;
        } else if (opc == 2) {          /* MOV */
            out->rd = Rdn; out->op = OP_MOV_REG;
        } else {                         /* BX / BLX */
            u8 L = (w >> 7) & 1;
            out->rm = (w >> 3) & 0xF;
            out->op = L ? OP_BLX_REG : OP_BX;
        }
        return 2;
    }

    /* LDR literal (PC-relative) — A5.2.4: 01001 Rt imm8 */
    if ((w & 0xF800u) == 0x4800u) {
        out->op  = OP_LDR_LIT;
        out->rd  = (w >> 8) & 0x7;
        out->imm = (w & 0xFF) << 2;
        return 2;
    }

    /* Load/store register — A5.2.9: 0101 opc3 Rm Rn Rt */
    if ((w & 0xF000u) == 0x5000u) {
        u8 opc = (w >> 9) & 0x7;
        out->rm = (w >> 6) & 0x7;
        out->rn = (w >> 3) & 0x7;
        out->rd = w & 0x7;
        static const opcode_t ops[8] = {
            OP_STR_REG, OP_STRH_REG, OP_STRB_REG, OP_LDRSB_REG,
            OP_LDR_REG, OP_LDRH_REG, OP_LDRB_REG, OP_LDRSH_REG
        };
        out->op = ops[opc];
        return 2;
    }

    /* Load/store word imm — A5.2.10: 0110 L imm5 Rn Rt (imm5 * 4) */
    if ((w & 0xF000u) == 0x6000u) {
        u8 L = (w >> 11) & 1;
        out->rn  = (w >> 3) & 0x7;
        out->rd  = w & 0x7;
        out->imm = ((w >> 6) & 0x1F) << 2;
        out->op  = L ? OP_LDR_IMM : OP_STR_IMM;
        return 2;
    }

    /* Load/store byte imm: 0111 L imm5 Rn Rt */
    if ((w & 0xF000u) == 0x7000u) {
        u8 L = (w >> 11) & 1;
        out->rn  = (w >> 3) & 0x7;
        out->rd  = w & 0x7;
        out->imm = (w >> 6) & 0x1F;
        out->op  = L ? OP_LDRB_IMM : OP_STRB_IMM;
        return 2;
    }

    /* Load/store halfword imm: 1000 L imm5 Rn Rt (imm5 * 2) */
    if ((w & 0xF000u) == 0x8000u) {
        u8 L = (w >> 11) & 1;
        out->rn  = (w >> 3) & 0x7;
        out->rd  = w & 0x7;
        out->imm = ((w >> 6) & 0x1F) << 1;
        out->op  = L ? OP_LDRH_IMM : OP_STRH_IMM;
        return 2;
    }

    /* Load/store SP-relative: 1001 L Rt imm8 (imm8 * 4) */
    if ((w & 0xF000u) == 0x9000u) {
        u8 L = (w >> 11) & 1;
        out->rd  = (w >> 8) & 0x7;
        out->imm = (w & 0xFF) << 2;
        out->op  = L ? OP_LDR_SP : OP_STR_SP;
        return 2;
    }

    /* ADR (PC-relative): 1010 0 Rd imm8 (imm8 * 4) */
    if ((w & 0xF800u) == 0xA000u) {
        out->rd  = (w >> 8) & 0x7;
        out->imm = (w & 0xFF) << 2;
        out->op  = OP_ADR;
        return 2;
    }

    /* ADD SP immediate (T1): 1010 1 Rd imm8 (imm8 * 4) */
    if ((w & 0xF800u) == 0xA800u) {
        out->rd  = (w >> 8) & 0x7;
        out->imm = (w & 0xFF) << 2;
        out->op  = OP_ADD_SP_IMM;
        return 2;
    }

    /* Miscellaneous 16-bit — A5.2.5: 1011 xxxx */
    if ((w & 0xF000u) == 0xB000u) {
        /* ADD/SUB SP, SP, #imm7*4: 1011 0000 0/1 imm7 */
        if ((w & 0xFF80u) == 0xB000u) {
            out->imm = (w & 0x7F) << 2;
            out->op  = OP_ADD_SP_SP;
            return 2;
        }
        if ((w & 0xFF80u) == 0xB080u) {
            out->imm = (w & 0x7F) << 2;
            out->op  = OP_SUB_SP_SP;
            return 2;
        }
        /* PUSH: 1011 010 M reg_list */
        if ((w & 0xFE00u) == 0xB400u) {
            out->op = OP_PUSH;
            out->reg_list = (w & 0xFF) | (((w >> 8) & 1) << 14); /* LR bit */
            return 2;
        }
        /* POP: 1011 110 P reg_list */
        if ((w & 0xFE00u) == 0xBC00u) {
            out->op = OP_POP;
            out->reg_list = (w & 0xFF) | (((w >> 8) & 1) << 15); /* PC bit */
            return 2;
        }
        /* CBZ/CBNZ — A7.7.21 / A7.7.22.
           1011 op 0 i 1 imm5(5) Rn(3). Fixed bit[10]=0, bit[8]=1. */
        if ((w & 0xF500u) == 0xB100u) {
            u32 op = (w >> 11) & 1;
            u32 i_bit = (w >> 9) & 1;
            u32 imm5 = (w >> 3) & 0x1F;
            out->rn = (u8)(w & 0x7);
            out->imm = (i_bit << 6) | (imm5 << 1);
            out->op = op ? OP_CBNZ : OP_CBZ;
            return 2;
        }
        /* IT: 1011 1111 cond mask, mask != 0000.  ARM ARM A7.7.38 */
        if ((w & 0xFF00u) == 0xBF00u && (w & 0x000F) != 0) {
            out->op = OP_T32_IT;
            out->it_first = (w >> 4) & 0xF;
            out->it_mask  = w & 0xF;
            return 2;
        }
        /* NOP / hints: BF00=NOP, BF10=YIELD, BF20=WFE, BF30=WFI, BF40=SEV */
        if ((w & 0xFF0Fu) == 0xBF00u) {
            u8 hint = (w >> 4) & 0xF;
            if (hint == 0)      out->op = OP_NOP;
            else if (hint == 1) out->op = OP_YIELD;
            else if (hint == 2) out->op = OP_WFE;
            else if (hint == 3) out->op = OP_WFI;
            else if (hint == 4) out->op = OP_SEV;
            else                out->op = OP_NOP;
            return 2;
        }
        /* Extend ops: SXTH/SXTB/UXTH/UXTB — 1011 0010 oo Rm Rd */
        if ((w & 0xFF00u) == 0xB200u) {
            u8 opc = (w >> 6) & 0x3;
            out->rm = (w >> 3) & 0x7;
            out->rd = w & 0x7;
            static const opcode_t ops[4] = { OP_SXTH, OP_SXTB, OP_UXTH, OP_UXTB };
            out->op = ops[opc];
            return 2;
        }
        /* REV / REV16 / REVSH — 1011 1010 oo Rm Rd */
        if ((w & 0xFF00u) == 0xBA00u) {
            u8 opc = (w >> 6) & 0x3;
            out->rm = (w >> 3) & 0x7;
            out->rd = w & 0x7;
            if (opc == 0)      out->op = OP_REV;
            else if (opc == 1) out->op = OP_REV16;
            else if (opc == 3) out->op = OP_REVSH;
            return 2;
        }
        /* BKPT: 1011 1110 imm8 */
        if ((w & 0xFF00u) == 0xBE00u) {
            out->op = OP_BKPT;
            out->imm = w & 0xFF;
            return 2;
        }
        /* CPS (T1): 10110110 011 im 0 A I F. ARM ARM A7.7.33.
           im=0 IE (enable), im=1 ID (disable). A/I/F select masks. */
        if ((w & 0xFFE0u) == 0xB660u) {
            u32 im = (w >> 4) & 1;
            u32 I_bit = (w >> 1) & 1;
            u32 F_bit = w & 1;
            out->op  = OP_CPS;
            out->imm = (im << 2) | (I_bit << 1) | F_bit;
            return 2;
        }
    }

    /* STMIA/LDMIA — A5.2.12: 1100 L Rn reg_list */
    if ((w & 0xF000u) == 0xC000u) {
        u8 L = (w >> 11) & 1;
        out->rn = (w >> 8) & 0x7;
        out->reg_list = w & 0xFF;
        out->op = L ? OP_LDM : OP_STM;
        return 2;
    }

    /* Conditional branch B<cond> imm8 (or SVC/UDF) — A5.2.6 */
    if ((w & 0xF000u) == 0xD000u) {
        u8 cond = (w >> 8) & 0xF;
        if (cond == 0xE) { out->op = OP_UDF; out->imm = w & 0xFF; return 2; }
        if (cond == 0xF) { out->op = OP_SVC; out->imm = w & 0xFF; return 2; }
        out->op = OP_B_COND;
        out->cond = cond;
        i32 off = (i32)(i8)(w & 0xFF);
        out->imm = (u32)(off << 1);
        return 2;
    }

    /* Unconditional branch B imm11 — A5.2.7 */
    if ((w & 0xF800u) == 0xE000u) {
        out->op = OP_B_UNCOND;
        out->cond = 0xE;
        i32 off = (i32)((w & 0x7FF) << 21) >> 20; /* sign-extend 11-bit, <<1 */
        out->imm = (u32)off;
        return 2;
    }

    /* NOP-hint 0xBF00. Hints form: 1011 1111 xxxx yyyy — A5.2.8.
       For Phase 1 we map 0xBF00 (NOP). Others extended later. */
    if (w == 0xBF00u) { out->op = OP_NOP; return 2; }

    /* Unknown — leave as UNDEFINED. */
    return 2;
}

static bool is_t32(u16 w) {
    /* ARM ARM A5.1: 11101, 11110, 11111 => 32-bit Thumb-2 */
    u16 top5 = (w >> 11) & 0x1F;
    return top5 == 0x1D || top5 == 0x1E || top5 == 0x1F;
}

/* Sign-extend (n-bit) value v to 32-bit. */
static i32 sext(u32 v, u8 n) {
    u32 m = 1u << (n - 1);
    return (i32)((v ^ m) - m);
}

/* Rotate right by n. */
static u32 ror32(u32 v, u32 n) {
    n &= 31;
    return n ? ((v >> n) | (v << (32 - n))) : v;
}

/* ThumbExpandImm — ARM ARM A5.3.2.
   Decodes a 12-bit modified immediate (i:imm3:imm8) into a 32-bit value. */
static u32 thumb_expand_imm(u32 i12) {
    u32 imm8 = i12 & 0xFF;
    u32 imm3i = (i12 >> 8) & 0xF;
    if ((imm3i >> 2) == 0) {
        /* Patterns based on imm3i bits[1:0]. */
        switch (imm3i & 3) {
            case 0: return imm8;
            case 1: return imm8 | (imm8 << 16);
            case 2: return (imm8 << 8) | (imm8 << 24);
            case 3: return imm8 | (imm8 << 8) | (imm8 << 16) | (imm8 << 24);
        }
    }
    /* Rotation form: 8-bit value with bit7=1, rotated right by (i:imm3:imm8[7]). */
    u32 unrot = imm8 | 0x80;
    u32 ror_n = (i12 >> 7) & 0x1F;
    return ror32(unrot, ror_n);
}

/* Thumb-2 32-bit decode — ARM ARM A5.3. */
static u8 decode_thumb32(u16 w0, u16 w1, addr_t pc, insn_t* out) {
    set_undef(out, (u32)w0 << 16 | w1, pc);
    out->size = 4;
    out->raw = ((u32)w0 << 16) | w1;

    u32 op1 = (w0 >> 11) & 0x3;   /* bits 12:11 */
    u32 op2 = (w0 >> 4)  & 0x7F;  /* bits 10:4 */
    u32 op_w1 = (w1 >> 15) & 1;

    /* === Branches and miscellaneous control — A5.3.4 ===
       w0[15:11]=11110, w1[15]=1.
       BL:    w1[15:14]=11, w1[12]=1
       B T4:  w1[15:14]=10, w1[12]=1
       B T3:  w1[15:14]=10, w1[12]=0  (conditional) */
    if ((w0 & 0xF800u) == 0xF000u && (w1 & 0x8000u) == 0x8000u) {
        u32 S     = (w0 >> 10) & 1;
        u32 imm10 = w0 & 0x3FF;
        u32 op1   = (w1 >> 14) & 0x3;     /* bit[15:14] */
        u32 op12  = (w1 >> 12) & 1;       /* bit[12] */
        u32 J1    = (w1 >> 13) & 1;
        u32 J2    = (w1 >> 11) & 1;
        u32 imm11 = w1 & 0x7FF;

        if (op1 == 3 && op12 == 1) {
            /* BL T1 */
            u32 I1 = (~(J1 ^ S)) & 1;
            u32 I2 = (~(J2 ^ S)) & 1;
            u32 imm25 = (S << 24) | (I1 << 23) | (I2 << 22) |
                        (imm10 << 12) | (imm11 << 1);
            out->imm = (u32)sext(imm25, 25);
            out->op  = OP_T32_BL;
            return 4;
        }
        if (op1 == 2 && op12 == 1) {
            /* B T4 unconditional */
            u32 I1 = (~(J1 ^ S)) & 1;
            u32 I2 = (~(J2 ^ S)) & 1;
            u32 imm25 = (S << 24) | (I1 << 23) | (I2 << 22) |
                        (imm10 << 12) | (imm11 << 1);
            out->imm = (u32)sext(imm25, 25);
            out->op  = OP_B_UNCOND;
            return 4;
        }
        if (op1 == 2 && op12 == 0) {
            /* Misc-control vs B T3 conditional — disambiguate on w0 pattern. */
            /* MRS T1: w0 = 0xF3EF. Rd = w1[11:8], SYSm = w1[7:0]. */
            if (w0 == 0xF3EFu) {
                out->rd  = (u8)((w1 >> 8) & 0xF);
                out->imm = w1 & 0xFFu;
                out->op  = OP_T32_MRS;
                return 4;
            }
            /* MSR (reg) T1: w0[15:4] = 0xF38. Rn = w0[3:0]. */
            if ((w0 & 0xFFF0u) == 0xF380u) {
                out->rn  = (u8)(w0 & 0xF);
                out->imm = w1 & 0xFFu;
                out->rs  = (u8)((w1 >> 10) & 0x3);
                out->op  = OP_T32_MSR;
                return 4;
            }
            /* Hint / barrier: w0 = 0xF3AF — treat all as NOP. */
            if (w0 == 0xF3AFu) {
                out->op = OP_T32_NOP;
                return 4;
            }
            /* Otherwise: B T3 conditional. */
            u32 cond  = (w0 >> 6) & 0xF;
            u32 imm6  = w0 & 0x3F;
            u32 imm21 = (S << 20) | (J2 << 19) | (J1 << 18) |
                        (imm6 << 12) | (imm11 << 1);
            out->imm  = (u32)sext(imm21, 21);
            out->cond = (u8)cond;
            out->op   = OP_T32_B_COND;
            return 4;
        }
    }

    /* === Data processing (modified immediate) — A5.3.1 ===
       w0[15:11]=11110, w0[9]=0, w1[15]=0 */
    if ((w0 & 0xFA00u) == 0xF000u && op_w1 == 0) {
        u32 op4 = (w0 >> 5) & 0xF;
        u32 S   = (w0 >> 4) & 1;
        u32 Rn  = w0 & 0xF;
        u32 i_bit = (w0 >> 10) & 1;
        u32 imm3  = (w1 >> 12) & 0x7;
        u32 Rd    = (w1 >> 8) & 0xF;
        u32 imm8  = w1 & 0xFF;
        u32 i12   = (i_bit << 11) | (imm3 << 8) | imm8;

        out->rn = (u8)Rn;
        out->rd = (u8)Rd;
        out->imm = thumb_expand_imm(i12);
        out->set_flags = S != 0;

        switch (op4) {
            case 0:  out->op = (Rd == 15 && S) ? OP_T32_TST_IMM : OP_T32_AND_IMM; break;
            case 1:  out->op = OP_T32_BIC_IMM; break;
            case 2:  out->op = (Rn == 15) ? OP_T32_MOV_IMM : OP_T32_ORR_IMM; break;
            case 3:  out->op = (Rn == 15) ? OP_T32_MVN_IMM : OP_T32_ORN_IMM; break;
            case 4:  out->op = (Rd == 15 && S) ? OP_T32_TEQ_IMM : OP_T32_EOR_IMM; break;
            case 8:  out->op = (Rd == 15 && S) ? OP_T32_CMN_IMM : OP_T32_ADD_IMM; break;
            case 10: out->op = OP_T32_ADC_IMM; break;
            case 11: out->op = OP_T32_SBC_IMM; break;
            case 13: out->op = (Rd == 15 && S) ? OP_T32_CMP_IMM : OP_T32_SUB_IMM; break;
            case 14: out->op = OP_T32_RSB_IMM; break;
            default: break;
        }
        return 4;
    }

    /* === Data processing (plain binary immediate) — A5.3.3 ===
       w0[15:11]=11110, w0[9]=1, w1[15]=0 */
    if ((w0 & 0xFA00u) == 0xF200u && op_w1 == 0) {
        u32 op4 = (w0 >> 4) & 0x1F;
        u32 Rn  = w0 & 0xF;
        u32 i_bit = (w0 >> 10) & 1;
        u32 imm3  = (w1 >> 12) & 0x7;
        u32 Rd    = (w1 >> 8) & 0xF;
        u32 imm8  = w1 & 0xFF;
        u32 imm12 = (i_bit << 11) | (imm3 << 8) | imm8;

        out->rn = (u8)Rn; out->rd = (u8)Rd;

        if ((op4 & 0x1F) == 0x00) {     /* ADDW */
            out->imm = imm12;
            if (Rn == 15) { out->op = OP_T32_ADR_T3; }
            else          { out->op = OP_T32_ADDW; }
            return 4;
        }
        if ((op4 & 0x1F) == 0x0A) {     /* SUBW */
            out->imm = imm12;
            if (Rn == 15) { out->op = OP_T32_ADR_T2; }
            else          { out->op = OP_T32_SUBW; }
            return 4;
        }
        if ((op4 & 0x1F) == 0x04) {     /* MOVW imm16 */
            u32 imm4 = w0 & 0xF;
            out->imm = (imm4 << 12) | imm12;
            out->op = OP_T32_MOVW;
            return 4;
        }
        if ((op4 & 0x1F) == 0x0C) {     /* MOVT imm16 */
            u32 imm4 = w0 & 0xF;
            out->imm = (imm4 << 12) | imm12;
            out->op = OP_T32_MOVT;
            return 4;
        }
        return 4;
    }

    /* === Load/store single — A5.3.10 ===
       w0[15:9]=1111100 (MSB byte 0xF8 family). Cover all common LDR/STR encodings. */
    if ((w0 & 0xFE00u) == 0xF800u) {
        u32 size  = (w0 >> 5) & 0x3;     /* 00=B, 01=H, 10=W */
        u32 L     = (w0 >> 4) & 1;       /* 0=store 1=load */
        u32 S_bit = (w0 >> 8) & 1;       /* sign-extend on load */
        u32 Rn    = w0 & 0xF;
        u32 Rt    = (w1 >> 12) & 0xF;
        u32 op_field = (w1 >> 6) & 0x3F;

        if (Rn == 15) {
            /* PC-relative: T2 LDR literal — w1[11:0] = imm12, w0[7]=U */
            u32 U   = (w0 >> 7) & 1;
            u32 imm = w1 & 0xFFF;
            out->rd = (u8)Rt; out->imm = imm; out->add = U != 0;
            if (L && size == 2) out->op = OP_T32_LDR_LIT;
            return 4;
        }

        if ((w0 & 0x0080u) != 0) {
            /* T3: imm12 unsigned offset, no writeback */
            u32 imm12 = w1 & 0xFFF;
            out->rn = (u8)Rn; out->rd = (u8)Rt; out->imm = imm12;
            out->add = true; out->index = true; out->writeback = false;
            if (size == 2) out->op = L ? OP_T32_LDR_IMM  : OP_T32_STR_IMM;
            else if (size == 1) out->op = L ? OP_T32_LDRH_IMM : OP_T32_STRH_IMM;
            else                out->op = L ? OP_T32_LDRB_IMM : OP_T32_STRB_IMM;
            if (L && S_bit) {
                if (size == 0) out->op = OP_T32_LDRSB_IMM;
                else if (size == 1) out->op = OP_T32_LDRSH_IMM;
            }
            return 4;
        }

        if ((op_field & 0x24) == 0x24 || (op_field & 0x3C) == 0x30 ||
            (op_field & 0x3C) == 0x38 || (op_field & 0x3C) == 0x24 ||
            (op_field & 0x3C) == 0x2C) {
            /* T4: imm8 with P/U/W bits in w1[10:8] */
            u32 P = (w1 >> 10) & 1;
            u32 U = (w1 >> 9)  & 1;
            u32 W = (w1 >> 8)  & 1;
            u32 imm8 = w1 & 0xFF;
            out->rn = (u8)Rn; out->rd = (u8)Rt; out->imm = imm8;
            out->index = P != 0; out->add = U != 0; out->writeback = W != 0;
            if (size == 2) out->op = L ? OP_T32_LDR_IMM  : OP_T32_STR_IMM;
            else if (size == 1) out->op = L ? OP_T32_LDRH_IMM : OP_T32_STRH_IMM;
            else                out->op = L ? OP_T32_LDRB_IMM : OP_T32_STRB_IMM;
            if (L && S_bit) {
                if (size == 0) out->op = OP_T32_LDRSB_IMM;
                else if (size == 1) out->op = OP_T32_LDRSH_IMM;
            }
            return 4;
        }

        if (op_field == 0) {
            /* Register offset: Rm in w1[3:0], shift in w1[5:4] */
            u32 Rm = w1 & 0xF;
            u32 sh = (w1 >> 4) & 0x3;
            out->rn = (u8)Rn; out->rd = (u8)Rt; out->rm = (u8)Rm;
            out->shift_n = (u8)sh; out->shift_type = 0;
            out->index = true; out->add = true; out->writeback = false;
            if (size == 2) out->op = L ? OP_T32_LDR_REG : OP_T32_STR_REG;
            return 4;
        }
        return 4;
    }

    /* === Load/store dual (LDRD/STRD) — A5.3.6 ===
       w0[15:9]=1110100, w0[6]=1 */
    if ((w0 & 0xFE40u) == 0xE840u) {
        u32 P = (w0 >> 8) & 1;
        u32 U = (w0 >> 7) & 1;
        u32 W = (w0 >> 5) & 1;
        u32 L = (w0 >> 4) & 1;
        u32 Rn = w0 & 0xF;
        u32 Rt = (w1 >> 12) & 0xF;
        u32 Rt2 = (w1 >> 8) & 0xF;
        u32 imm8 = w1 & 0xFF;
        out->rn = (u8)Rn; out->rd = (u8)Rt; out->rs = (u8)Rt2;
        out->imm = imm8 << 2;
        out->index = P != 0; out->add = U != 0; out->writeback = W != 0;
        out->op = L ? OP_T32_LDRD_IMM : OP_T32_STRD_IMM;
        return 4;
    }

    /* === Data processing (register) — A5.3.11 ===
       w0[15:9] = 1110101 covers both 0xEA__ and 0xEB__.
       op4 = w0[8:5] is 4-bit opcode. */
    if ((w0 & 0xFE00u) == 0xEA00u) {
        u32 op4 = (w0 >> 5) & 0xF;
        u32 S   = (w0 >> 4) & 1;
        u32 Rn  = w0 & 0xF;
        u32 imm3 = (w1 >> 12) & 0x7;
        u32 Rd   = (w1 >> 8) & 0xF;
        u32 imm2 = (w1 >> 6) & 0x3;
        u32 typ  = (w1 >> 4) & 0x3;
        u32 Rm   = w1 & 0xF;

        out->rn = (u8)Rn; out->rd = (u8)Rd; out->rm = (u8)Rm;
        out->set_flags = S != 0;
        out->shift_type = (u8)typ;
        out->shift_n = (u8)((imm3 << 2) | imm2);

        switch (op4) {
            case 0:  out->op = (Rd == 15 && S) ? OP_T32_TST_REG : OP_T32_AND_REG; break;
            case 1:  out->op = OP_T32_BIC_REG; break;
            case 2:  out->op = (Rn == 15) ? OP_T32_MOV_REG : OP_T32_ORR_REG; break;
            case 3:  out->op = (Rn == 15) ? OP_T32_MVN_REG : OP_T32_ORN_REG; break;
            case 4:  out->op = (Rd == 15 && S) ? OP_T32_TEQ_REG : OP_T32_EOR_REG; break;
            case 8:  out->op = (Rd == 15 && S) ? OP_T32_CMN_REG : OP_T32_ADD_REG; break;
            case 10: out->op = OP_T32_ADC_REG; break;
            case 11: out->op = OP_T32_SBC_REG; break;
            case 13: out->op = (Rd == 15 && S) ? OP_T32_CMP_REG : OP_T32_SUB_REG; break;
            case 14: out->op = OP_T32_RSB_REG; break;
            default: break;
        }
        return 4;
    }

    /* === T32 shift register (LSL/LSR/ASR/ROR) — A5.3.12 ===
       w0 = 1111 1010 0 typ(2) S Rn, w1 = 1111 Rd 0000 Rm
       bit[7:5] = 0,typ[1],typ[0]; bit[4] = S; but decode table uses bits[24:21] = 0,0,0,typ. */
    if ((w0 & 0xFFA0u) == 0xFA00u && (w1 & 0xF0F0u) == 0xF000u) {
        u32 typ = (w0 >> 5) & 0x3;
        out->rn = (u8)(w0 & 0xF);        /* value to shift */
        out->rd = (u8)((w1 >> 8) & 0xF);
        out->rm = (u8)(w1 & 0xF);        /* shift amount */
        out->set_flags = (w0 >> 4) & 1;
        switch (typ) {
            case 0: out->op = OP_T32_LSL_R; break;
            case 1: out->op = OP_T32_LSR_R; break;
            case 2: out->op = OP_T32_ASR_R; break;
            case 3: out->op = OP_T32_ROR_R; break;
        }
        return 4;
    }

    /* === 32-bit multiply / multiply-accumulate / divide — A5.3.17 / A5.3.18 ===
       Multiply (4-op): w0[15:4] = 1111 1011 0xxx, w1[15:4] = 1111 op4 Rd
       Long multiply:  w0[15:4] = 1111 1011 1xxx */
    if ((w0 & 0xFF80u) == 0xFB00u) {
        u32 op1 = (w0 >> 4) & 0x7;
        u32 Rn  = w0 & 0xF;
        u32 Ra  = (w1 >> 12) & 0xF;
        u32 Rd  = (w1 >> 8) & 0xF;
        u32 op2 = (w1 >> 4) & 0xF;
        u32 Rm  = w1 & 0xF;
        out->rn = (u8)Rn; out->rm = (u8)Rm; out->rd = (u8)Rd; out->rs = (u8)Ra;

        if (op1 == 0 && op2 == 0) {
            out->op = (Ra == 15) ? OP_T32_MUL : OP_T32_MLA;
            return 4;
        }
        if (op1 == 0 && op2 == 1) { out->op = OP_T32_MLS; return 4; }
        return 4;
    }
    if ((w0 & 0xFF80u) == 0xFB80u) {
        u32 op1 = (w0 >> 4) & 0x7;
        u32 Rn  = w0 & 0xF;
        u32 RdLo = (w1 >> 12) & 0xF;
        u32 RdHi = (w1 >> 8) & 0xF;
        u32 op2  = (w1 >> 4) & 0xF;
        u32 Rm   = w1 & 0xF;
        out->rn = (u8)Rn; out->rm = (u8)Rm;
        out->rd = (u8)RdLo; out->rs = (u8)RdHi;

        switch (op1) {
            case 0: out->op = OP_T32_SMULL; return 4;
            case 1: if (op2 == 0xF) { out->op = OP_T32_SDIV; return 4; } break;
            case 2: out->op = OP_T32_UMULL; return 4;
            case 3: if (op2 == 0xF) { out->op = OP_T32_UDIV; return 4; } break;
            case 4: out->op = OP_T32_SMLAL; return 4;
            case 6: out->op = OP_T32_UMLAL; return 4;
            default: break;
        }
        return 4;
    }

    /* === T32 Load/Store multiple — A5.3.5 ===
       w0[15:9]=1110100. bit[8]=P, bit[7]=U (P,U form IA/DB).
       IA: P=0, U=1 (0xE88x/0xE89x base)
       DB: P=1, U=0 (0xE90x/0xE91x base)
       bit[5]=W (writeback), bit[4]=L (1=load).  w1 = reglist. */
    if ((w0 & 0xFE40u) == 0xE800u && ((w0 >> 7) & 1) != ((w0 >> 8) & 1)) {
        u32 W  = (w0 >> 5) & 1;
        u32 L  = (w0 >> 4) & 1;
        u32 Rn = w0 & 0xF;
        u32 P  = (w0 >> 8) & 1;     /* 1 = DB, 0 = IA */
        out->rn = (u8)Rn;
        out->reg_list = w1;
        out->writeback = W != 0;
        out->add = (P == 0);
        out->op = L ? OP_T32_LDM : OP_T32_STM;
        return 4;
    }

    /* === TBB / TBH — A5.3.7 ===
       w0 = 1110 1000 1101 Rn, w1 = 1111 0000 000 H Rm
       H=0 TBB (byte), H=1 TBH (halfword) */
    if ((w0 & 0xFFF0u) == 0xE8D0u && (w1 & 0xFFE0u) == 0xF000u) {
        out->rn = (u8)(w0 & 0xF);
        out->rm = (u8)(w1 & 0xF);
        out->op = (w1 & 0x10) ? OP_T32_TBH : OP_T32_TBB;
        return 4;
    }

    /* === Bitfield operations (T32 plain-imm with specific op) — A5.3.3 ===
       BFI  T1: w0 = 11110_0_11_0110_Rn, w1 = 0_imm3_Rd_imm2_0_msb
       BFC  T1: same as BFI with Rn=1111
       UBFX T1: w0 = 11110_0_11_1100_Rn, w1 = 0_imm3_Rd_imm2_0_widthm1
       SBFX T1: w0 = 11110_0_11_0100_Rn */
    if ((w0 & 0xFBF0u) == 0xF360u) {
        /* BFI / BFC */
        u32 Rn = w0 & 0xF;
        u32 imm3 = (w1 >> 12) & 0x7;
        u32 Rd   = (w1 >> 8) & 0xF;
        u32 imm2 = (w1 >> 6) & 0x3;
        u32 msb  = w1 & 0x1F;
        u32 lsb  = (imm3 << 2) | imm2;
        out->rn = (u8)Rn; out->rd = (u8)Rd;
        out->imm = (msb << 8) | lsb;
        out->op = (Rn == 15) ? OP_T32_BFC : OP_T32_BFI;
        return 4;
    }
    if ((w0 & 0xFBF0u) == 0xF3C0u) {
        /* UBFX */
        u32 Rn = w0 & 0xF;
        u32 imm3 = (w1 >> 12) & 0x7;
        u32 Rd   = (w1 >> 8) & 0xF;
        u32 imm2 = (w1 >> 6) & 0x3;
        u32 widthm1 = w1 & 0x1F;
        u32 lsb  = (imm3 << 2) | imm2;
        out->rn = (u8)Rn; out->rd = (u8)Rd;
        out->imm = (widthm1 << 8) | lsb;
        out->op = OP_T32_UBFX;
        return 4;
    }
    if ((w0 & 0xFBF0u) == 0xF340u) {
        /* SBFX */
        u32 Rn = w0 & 0xF;
        u32 imm3 = (w1 >> 12) & 0x7;
        u32 Rd   = (w1 >> 8) & 0xF;
        u32 imm2 = (w1 >> 6) & 0x3;
        u32 widthm1 = w1 & 0x1F;
        u32 lsb  = (imm3 << 2) | imm2;
        out->rn = (u8)Rn; out->rd = (u8)Rd;
        out->imm = (widthm1 << 8) | lsb;
        out->op = OP_T32_SBFX;
        return 4;
    }

    /* === CLZ / RBIT — A5.3.13, A5.3.14 ===
       w0 = 1111 1010 1x11 Rm, w1 = 1111 Rd 10xy Rm (duplicate Rm) */
    if ((w0 & 0xFFF0u) == 0xFAB0u && (w1 & 0xF0F0u) == 0xF080u) {
        out->rm = (u8)(w0 & 0xF);
        out->rd = (u8)((w1 >> 8) & 0xF);
        out->op = OP_T32_CLZ;
        return 4;
    }
    if ((w0 & 0xFFF0u) == 0xFA90u && (w1 & 0xF0F0u) == 0xF0A0u) {
        out->rm = (u8)(w0 & 0xF);
        out->rd = (u8)((w1 >> 8) & 0xF);
        out->op = OP_T32_RBIT;
        return 4;
    }

    /* === MRS (T1) — A7.7.69 ===
       w0 = 1111_0011_1110_1111 = 0xF3EF, w1[15:12]=1000, w1[11:8]=Rd, w1[7:0]=SYSm */
    if (w0 == 0xF3EFu && (w1 & 0xF000u) == 0x8000u) {
        out->rd  = (u8)((w1 >> 8) & 0xF);
        out->imm = w1 & 0xFFu;        /* SYSm selector */
        out->op  = OP_T32_MRS;
        return 4;
    }
    /* === MSR (reg, T1) — A7.7.73 ===
       w0 = 1111_0011_1000_Rn, w1[15:12]=1000, w1[11:10]=mask,
       w1[9:8]=00, w1[7:0]=SYSm */
    if ((w0 & 0xFFF0u) == 0xF380u && (w1 & 0xFF00u) == 0x8800u) {
        out->rn   = (u8)(w0 & 0xF);
        out->imm  = w1 & 0xFFu;       /* SYSm */
        out->rs   = (u8)((w1 >> 10) & 0x3); /* mask */
        out->op   = OP_T32_MSR;
        return 4;
    }

    return 4;
}

/* Public for tests. */
u32 thumb_expand_imm_pub(u32 i12) { return thumb_expand_imm(i12); }

u8 decode(struct bus_s* bus, addr_t pc, insn_t* out) {
    u16 w0 = bus_r16(bus, pc);
    if (!is_t32(w0)) {
        return decode_thumb16(w0, pc, out);
    }
    u16 w1 = bus_r16(bus, pc + 2);
    return decode_thumb32(w0, w1, pc, out);
}

const char* opcode_name(opcode_t op) {
    static const char* names[] = {
        [OP_UNDEFINED] = "UNDEF",
        [OP_LSL_IMM]   = "LSL",
        [OP_LSR_IMM]   = "LSR",
        [OP_ASR_IMM]   = "ASR",
        [OP_ADD_REG]   = "ADD",
        [OP_SUB_REG]   = "SUB",
        [OP_ADD_IMM3]  = "ADD",
        [OP_SUB_IMM3]  = "SUB",
        [OP_MOV_IMM]   = "MOV",
        [OP_CMP_IMM]   = "CMP",
        [OP_ADD_IMM8]  = "ADD",
        [OP_SUB_IMM8]  = "SUB",
        [OP_AND_REG]   = "AND",
        [OP_EOR_REG]   = "EOR",
        [OP_MUL]       = "MUL",
        [OP_B_COND]    = "B.cond",
        [OP_B_UNCOND]  = "B",
        [OP_SVC]       = "SVC",
        [OP_UDF]       = "UDF",
        [OP_NOP]       = "NOP",
    };
    if (op >= OP_COUNT) return "???";
    const char* n = names[op];
    return n ? n : "???";
}
