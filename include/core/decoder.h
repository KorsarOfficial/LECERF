#ifndef CORTEX_M_DECODER_H
#define CORTEX_M_DECODER_H

#include "core/types.h"

/* Decoded instruction. Encoding is identified by opcode enum, operands
   are normalized so the executor does not re-parse bits. */

typedef enum {
    OP_UNDEFINED = 0,

    /* ARMv6-M Thumb-1 encodings */
    OP_LSL_IMM, OP_LSR_IMM, OP_ASR_IMM,
    OP_ADD_REG, OP_SUB_REG,
    OP_ADD_IMM3, OP_SUB_IMM3,
    OP_MOV_IMM, OP_CMP_IMM, OP_ADD_IMM8, OP_SUB_IMM8,
    OP_AND_REG, OP_EOR_REG, OP_LSL_REG, OP_LSR_REG,
    OP_ASR_REG, OP_ADC_REG, OP_SBC_REG, OP_ROR_REG,
    OP_TST_REG, OP_RSB_IMM, OP_CMP_REG, OP_CMN_REG,
    OP_ORR_REG, OP_MUL, OP_BIC_REG, OP_MVN_REG,
    OP_ADD_REG_T2, OP_CMP_REG_T2, OP_MOV_REG,
    OP_BX, OP_BLX_REG,
    OP_LDR_LIT,
    OP_STR_REG, OP_STRH_REG, OP_STRB_REG, OP_LDRSB_REG,
    OP_LDR_REG, OP_LDRH_REG, OP_LDRB_REG, OP_LDRSH_REG,
    OP_STR_IMM, OP_LDR_IMM, OP_STRB_IMM, OP_LDRB_IMM,
    OP_STRH_IMM, OP_LDRH_IMM,
    OP_STR_SP, OP_LDR_SP,
    OP_ADR, OP_ADD_SP_IMM,
    OP_ADD_SP_SP, OP_SUB_SP_SP,
    OP_SXTH, OP_SXTB, OP_UXTH, OP_UXTB,
    OP_PUSH, OP_POP,
    OP_REV, OP_REV16, OP_REVSH,
    OP_CPS, OP_BKPT,
    OP_STM, OP_LDM,
    OP_B_COND, OP_B_UNCOND,
    OP_SVC, OP_UDF,
    OP_NOP, OP_YIELD, OP_WFE, OP_WFI, OP_SEV,
    OP_IT,
    OP_CBZ, OP_CBNZ,

    /* Thumb-2 32-bit encodings (ARMv7-M, ARM ARM A5.3) */
    OP_T32_BL,
    OP_T32_B_COND,                  /* B<cond>.W (T3 encoding) */

    /* Data processing modified immediate — A5.3.1.
       Carries S bit (set-flags) in cond field for executor. */
    OP_T32_AND_IMM, OP_T32_BIC_IMM,
    OP_T32_ORR_IMM, OP_T32_ORN_IMM,
    OP_T32_EOR_IMM, OP_T32_TEQ_IMM,
    OP_T32_ADD_IMM, OP_T32_ADC_IMM,
    OP_T32_SBC_IMM, OP_T32_SUB_IMM,
    OP_T32_RSB_IMM, OP_T32_TST_IMM,
    OP_T32_CMN_IMM, OP_T32_CMP_IMM,
    OP_T32_MOV_IMM, OP_T32_MVN_IMM,

    /* Plain immediate (12-bit) — A5.3.3 */
    OP_T32_ADDW, OP_T32_SUBW,
    OP_T32_MOVW, OP_T32_MOVT,
    OP_T32_ADR_T2, OP_T32_ADR_T3,

    /* Data processing register with shift — A5.3.11 */
    OP_T32_AND_REG, OP_T32_BIC_REG,
    OP_T32_ORR_REG, OP_T32_ORN_REG,
    OP_T32_EOR_REG, OP_T32_TEQ_REG,
    OP_T32_ADD_REG, OP_T32_ADC_REG,
    OP_T32_SBC_REG, OP_T32_SUB_REG,
    OP_T32_RSB_REG, OP_T32_TST_REG,
    OP_T32_CMN_REG, OP_T32_CMP_REG,
    OP_T32_MOV_REG, OP_T32_MVN_REG,

    /* Memory access */
    OP_T32_LDR_IMM, OP_T32_STR_IMM,
    OP_T32_LDR_LIT, OP_T32_LDR_REG, OP_T32_STR_REG,
    OP_T32_LDRB_IMM, OP_T32_STRB_IMM,
    OP_T32_LDRH_IMM, OP_T32_STRH_IMM,
    OP_T32_LDRSB_IMM, OP_T32_LDRSH_IMM,
    OP_T32_LDRD_IMM, OP_T32_STRD_IMM,
    OP_T32_LDM, OP_T32_STM,

    OP_T32_MSR, OP_T32_MRS,
    OP_T32_NOP,

    /* Multiply / divide (T32) */
    OP_T32_MUL,    OP_T32_MLA,    OP_T32_MLS,
    OP_T32_UMULL,  OP_T32_SMULL,
    OP_T32_UMLAL,  OP_T32_SMLAL,
    OP_T32_UDIV,   OP_T32_SDIV,

    /* Bitfield and saturating ops */
    OP_T32_BFI,    OP_T32_BFC,
    OP_T32_UBFX,   OP_T32_SBFX,
    OP_T32_CLZ,    OP_T32_RBIT,
    OP_T32_USAT,   OP_T32_SSAT,

    /* T32 register shift (Rd ← shift(Rn, Rm)). Separate from Thumb-1 LSL/etc. */
    OP_T32_LSL_R, OP_T32_LSR_R, OP_T32_ASR_R, OP_T32_ROR_R,

    /* VFP single-precision (Cortex-M4F) — A7.5 */
    OP_VLDR_S, OP_VSTR_S,
    OP_VADD_S, OP_VSUB_S, OP_VMUL_S, OP_VDIV_S,
    OP_VMOV_F_R, OP_VMOV_R_F,
    OP_VMOV_S, OP_VABS_S, OP_VNEG_S, OP_VSQRT_S,
    OP_VMOV_IMM_S,
    OP_VCMP_S, OP_VCVT_F_I, OP_VCVT_I_F,
    OP_VMRS, OP_VMSR,

    /* Table-branch byte/halfword */
    OP_T32_TBB,    OP_T32_TBH,

    /* IT block (16-bit but advanced state machine) */
    OP_T32_IT,

    OP_COUNT
} opcode_t;

typedef struct insn_s {
    opcode_t op;
    u32 pc;           /* instruction address */
    u8  size;         /* 2 (Thumb) or 4 (Thumb-2) */
    u8  rd, rn, rm, rs;
    u32 imm;          /* pre-decoded immediate (zero/sign extended) */
    u32 raw;          /* raw 16 or 32 bit encoding for debug */
    u8  cond;         /* condition code (0xE = AL = unconditional) */
    u32 reg_list;     /* bitmask for PUSH/POP/LDM/STM */
    bool set_flags;   /* T32 S bit */
    bool writeback;   /* T32 LDR/STR pre-/post-indexed writeback */
    bool add;         /* T32 LDR/STR U bit (offset sign) */
    bool index;       /* T32 LDR/STR P bit (pre-index) */
    u8   shift_type;  /* T32 register shift: 0=LSL 1=LSR 2=ASR 3=ROR/RRX */
    u8   shift_n;     /* T32 register shift amount (0..31) */
    u8   it_mask;
    u8   it_first;
    /* VFP fields. Sd = single-precision destination. */
    u8   sd, sn, sm;
} insn_t;

/* Decode at PC. Returns size (2 or 4). Sets out->op = OP_UNDEFINED on fail. */
struct bus_s;
u8 decode(struct bus_s* bus, addr_t pc, insn_t* out);
void run_dcache_invalidate(void);

const char* opcode_name(opcode_t op);

#endif
