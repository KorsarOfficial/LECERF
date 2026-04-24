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

/* Thumb-2 32-bit decode — ARM ARM A5.3. Branches with link (B/BL) encoding T4. */
static u8 decode_thumb32(u16 w0, u16 w1, addr_t pc, insn_t* out) {
    set_undef(out, (u32)w0 << 16 | w1, pc);
    out->size = 4;
    out->raw = ((u32)w0 << 16) | w1;

    /* Branch family: w0 bits[15:11] = 11110, w1 bits[15] = 1 — A5.3.4 */
    if ((w0 & 0xF800u) == 0xF000u && (w1 & 0x8000u) == 0x8000u) {
        u32 S     = (w0 >> 10) & 1;
        u32 imm10 = w0 & 0x3FF;
        u32 J1    = (w1 >> 13) & 1;
        u32 C     = (w1 >> 12) & 1;
        u32 J2    = (w1 >> 11) & 1;
        u32 imm11 = w1 & 0x7FF;
        if (C == 1) {
            /* BL — encoding T1, ARM ARM A7.7.18 */
            u32 I1 = (~(J1 ^ S)) & 1;
            u32 I2 = (~(J2 ^ S)) & 1;
            u32 imm25 = (S << 24) | (I1 << 23) | (I2 << 22) |
                        (imm10 << 12) | (imm11 << 1);
            out->imm = (u32)sext(imm25, 25);
            out->op  = OP_T32_BL;
            return 4;
        }
        /* Unconditional B — encoding T4 */
        u32 I1 = (~(J1 ^ S)) & 1;
        u32 I2 = (~(J2 ^ S)) & 1;
        u32 imm25 = (S << 24) | (I1 << 23) | (I2 << 22) |
                    (imm10 << 12) | (imm11 << 1);
        out->imm = (u32)sext(imm25, 25);
        out->op  = OP_B_UNCOND;
        return 4;
    }

    /* Other Thumb-2 encodings (data proc, load/store, MSR/MRS, etc.)
       — implemented incrementally as real firmware demands them. */
    return 4;
}

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
