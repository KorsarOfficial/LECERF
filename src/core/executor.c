#include <stdio.h>
#include <stdlib.h>
#include "core/cpu.h"
#include "core/bus.h"
#include "core/decoder.h"

/* Set EMU_TRACE=1 env var to enable SVC/PendSV trace. */
static int trace_on(void) {
    static int v = -1;
    if (v < 0) {
        const char* s = getenv("EMU_TRACE");
        v = (s && s[0] && s[0] != '0') ? 1 : 0;
    }
    return v;
}

/* Condition code evaluation. ARM ARM A7.3. */
static bool cond_pass(u32 apsr, u8 cond) {
    bool n = (apsr & APSR_N) != 0;
    bool z = (apsr & APSR_Z) != 0;
    bool c = (apsr & APSR_C) != 0;
    bool v = (apsr & APSR_V) != 0;
    switch (cond & 0xF) {
        case 0x0: return z;                 /* EQ */
        case 0x1: return !z;                /* NE */
        case 0x2: return c;                 /* CS/HS */
        case 0x3: return !c;                /* CC/LO */
        case 0x4: return n;                 /* MI */
        case 0x5: return !n;                /* PL */
        case 0x6: return v;                 /* VS */
        case 0x7: return !v;                /* VC */
        case 0x8: return c && !z;           /* HI */
        case 0x9: return !c || z;           /* LS */
        case 0xA: return n == v;            /* GE */
        case 0xB: return n != v;            /* LT */
        case 0xC: return !z && (n == v);    /* GT */
        case 0xD: return z || (n != v);     /* LE */
        case 0xE: return true;              /* AL */
        case 0xF: return true;              /* reserved */
    }
    return false;
}

/* Apply Thumb-2 register shift type/amount to value. */
static u32 t32_shift(u32 v, u8 type, u8 n) {
    if (n == 0 && type == 0) return v;
    switch (type & 3) {
        case 0: return n >= 32 ? 0 : (v << n);                /* LSL */
        case 1: return n >= 32 ? 0 : (v >> n);                /* LSR */
        case 2: { i32 sv = (i32)v;
                  return n >= 32 ? (u32)(sv >> 31) : (u32)(sv >> n); } /* ASR */
        case 3: return n ? ((v >> n) | (v << (32 - n))) : v;  /* ROR */
    }
    return v;
}

/* Returns true if opcode is a comparison that always updates flags
   (these still write APSR even inside IT block). */
static bool op_is_flagsetter(opcode_t op) {
    switch (op) {
        case OP_CMP_IMM: case OP_CMP_REG: case OP_CMP_REG_T2:
        case OP_CMN_REG: case OP_TST_REG:
        case OP_T32_CMP_IMM: case OP_T32_CMP_REG:
        case OP_T32_CMN_IMM: case OP_T32_CMN_REG:
        case OP_T32_TST_IMM: case OP_T32_TST_REG:
        case OP_T32_TEQ_IMM: case OP_T32_TEQ_REG:
            return true;
        default:
            return false;
    }
}

/* Executor: given decoded insn, mutate CPU/bus state. Returns false on fault. */
bool execute(cpu_t* c, bus_t* bus, const insn_t* i) {
    /* Advance PC to next by default; branches override. */
    u32 next_pc = c->r[REG_PC] + i->size;

    /* Inside IT block, save APSR; only comparison ops should write it. */
    bool in_it = cpu_in_it(c);
    u32 saved_apsr = c->apsr;

    /* If we're in an IT block and the current condition fails, NOP this insn
       but still advance the IT state and PC. */
    if (in_it) {
        u8 cond = cpu_it_cond(c);
        bool n = (c->apsr & APSR_N) != 0, z = (c->apsr & APSR_Z) != 0;
        bool cf = (c->apsr & APSR_C) != 0, vf = (c->apsr & APSR_V) != 0;
        bool pass;
        switch (cond & 0xF) {
            case 0x0: pass = z; break;
            case 0x1: pass = !z; break;
            case 0x2: pass = cf; break;
            case 0x3: pass = !cf; break;
            case 0x4: pass = n; break;
            case 0x5: pass = !n; break;
            case 0x6: pass = vf; break;
            case 0x7: pass = !vf; break;
            case 0x8: pass = cf && !z; break;
            case 0x9: pass = !cf || z; break;
            case 0xA: pass = n == vf; break;
            case 0xB: pass = n != vf; break;
            case 0xC: pass = !z && (n == vf); break;
            case 0xD: pass = z || (n != vf); break;
            default:  pass = true; break;
        }
        if (!pass) {
            cpu_it_advance(c);
            c->r[REG_PC] = next_pc;
            c->cycles++;
            return true;
        }
    }

    switch (i->op) {
        case OP_NOP:
            break;

        case OP_MOV_IMM: {
            c->r[i->rd] = i->imm;
            cpu_set_flags_nz(c, i->imm);
            break;
        }

        case OP_ADD_REG: {
            u32 a = c->r[i->rn], b = c->r[i->rm];
            u32 r = a + b;
            c->r[i->rd] = r;
            cpu_set_flags_nzcv_add(c, a, b, r, false);
            break;
        }

        case OP_SUB_REG: {
            u32 a = c->r[i->rn], b = c->r[i->rm];
            u32 r = a - b;
            c->r[i->rd] = r;
            cpu_set_flags_nzcv_sub(c, a, b, r);
            break;
        }

        case OP_ADD_IMM3:
        case OP_ADD_IMM8: {
            u32 a = c->r[i->rn], b = i->imm;
            u32 r = a + b;
            c->r[i->rd] = r;
            cpu_set_flags_nzcv_add(c, a, b, r, false);
            break;
        }

        case OP_SUB_IMM3:
        case OP_SUB_IMM8: {
            u32 a = c->r[i->rn], b = i->imm;
            u32 r = a - b;
            c->r[i->rd] = r;
            cpu_set_flags_nzcv_sub(c, a, b, r);
            break;
        }

        case OP_CMP_IMM: {
            u32 a = c->r[i->rn], b = i->imm;
            cpu_set_flags_nzcv_sub(c, a, b, a - b);
            break;
        }

        case OP_CMP_REG: {
            u32 a = c->r[i->rn], b = c->r[i->rm];
            cpu_set_flags_nzcv_sub(c, a, b, a - b);
            break;
        }

        case OP_AND_REG: {
            u32 r = c->r[i->rn] & c->r[i->rm];
            c->r[i->rd] = r;
            cpu_set_flags_nz(c, r);
            break;
        }

        case OP_EOR_REG: {
            u32 r = c->r[i->rn] ^ c->r[i->rm];
            c->r[i->rd] = r;
            cpu_set_flags_nz(c, r);
            break;
        }

        case OP_LSL_IMM: {
            u32 v = c->r[i->rm];
            u32 r = i->imm ? (v << i->imm) : v;
            c->r[i->rd] = r;
            cpu_set_flags_nz(c, r);
            break;
        }

        case OP_LSR_IMM: {
            u32 v = c->r[i->rm];
            u32 sh = i->imm ? i->imm : 32;
            u32 r = sh >= 32 ? 0 : (v >> sh);
            c->r[i->rd] = r;
            cpu_set_flags_nz(c, r);
            break;
        }

        case OP_MUL: {
            u32 r = c->r[i->rn] * c->r[i->rm];
            c->r[i->rd] = r;
            cpu_set_flags_nz(c, r);
            break;
        }

        case OP_ORR_REG: {
            u32 r = c->r[i->rn] | c->r[i->rm];
            c->r[i->rd] = r;
            cpu_set_flags_nz(c, r);
            break;
        }
        case OP_BIC_REG: {
            u32 r = c->r[i->rn] & ~c->r[i->rm];
            c->r[i->rd] = r;
            cpu_set_flags_nz(c, r);
            break;
        }
        case OP_MVN_REG: {
            u32 r = ~c->r[i->rm];
            c->r[i->rd] = r;
            cpu_set_flags_nz(c, r);
            break;
        }
        case OP_TST_REG: {
            u32 r = c->r[i->rn] & c->r[i->rm];
            cpu_set_flags_nz(c, r);
            break;
        }
        case OP_CMN_REG: {
            u32 a = c->r[i->rn], b = c->r[i->rm];
            cpu_set_flags_nzcv_add(c, a, b, a + b, false);
            break;
        }
        case OP_RSB_IMM: {
            /* Encoded as RSBS Rd, Rn, #0 — negation. */
            u32 a = 0, b = c->r[i->rn];
            u32 r = a - b;
            c->r[i->rd] = r;
            cpu_set_flags_nzcv_sub(c, a, b, r);
            break;
        }
        case OP_ADC_REG: {
            u32 a = c->r[i->rn], b = c->r[i->rm];
            bool ci = (c->apsr & APSR_C) != 0;
            u32 r = a + b + (ci ? 1u : 0u);
            c->r[i->rd] = r;
            cpu_set_flags_nzcv_add(c, a, b, r, ci);
            break;
        }
        case OP_SBC_REG: {
            /* SBC = a + ~b + C. Carry-clear means borrow. */
            u32 a = c->r[i->rn], b = c->r[i->rm];
            bool ci = (c->apsr & APSR_C) != 0;
            u32 nb = ~b;
            u32 r = a + nb + (ci ? 1u : 0u);
            c->r[i->rd] = r;
            cpu_set_flags_nzcv_add(c, a, nb, r, ci);
            break;
        }
        case OP_ASR_IMM: {
            u32 v = c->r[i->rm];
            u32 sh = i->imm ? i->imm : 32;
            i32 r = sh >= 32 ? ((i32)v >> 31) : ((i32)v >> sh);
            c->r[i->rd] = (u32)r;
            cpu_set_flags_nz(c, (u32)r);
            break;
        }
        case OP_LSL_REG: {
            u32 sh = c->r[i->rm] & 0xFF;
            u32 v  = c->r[i->rd];
            u32 r  = sh >= 32 ? 0 : (v << sh);
            c->r[i->rd] = r;
            cpu_set_flags_nz(c, r);
            break;
        }
        case OP_LSR_REG: {
            u32 sh = c->r[i->rm] & 0xFF;
            u32 v  = c->r[i->rd];
            u32 r  = sh >= 32 ? 0 : (v >> sh);
            c->r[i->rd] = r;
            cpu_set_flags_nz(c, r);
            break;
        }
        case OP_ASR_REG: {
            u32 sh = c->r[i->rm] & 0xFF;
            i32 v  = (i32)c->r[i->rd];
            i32 r  = sh >= 32 ? (v >> 31) : (v >> sh);
            c->r[i->rd] = (u32)r;
            cpu_set_flags_nz(c, (u32)r);
            break;
        }
        case OP_ROR_REG: {
            u32 sh = c->r[i->rm] & 0x1F;
            u32 v  = c->r[i->rd];
            u32 r  = sh ? ((v >> sh) | (v << (32 - sh))) : v;
            c->r[i->rd] = r;
            cpu_set_flags_nz(c, r);
            break;
        }

        case OP_B_COND: {
            if (cond_pass(c->apsr, i->cond)) {
                /* PC at branch = current + 4 (pipeline offset per ARM ARM). */
                next_pc = c->r[REG_PC] + 4 + i->imm;
            }
            break;
        }

        case OP_B_UNCOND: {
            next_pc = c->r[REG_PC] + 4 + i->imm;
            break;
        }

        case OP_CBZ: {
            if (c->r[i->rn] == 0) next_pc = c->r[REG_PC] + 4 + i->imm;
            break;
        }
        case OP_CBNZ: {
            if (c->r[i->rn] != 0) next_pc = c->r[REG_PC] + 4 + i->imm;
            break;
        }

        case OP_UDF:
            c->halted = true;
            return false;

        case OP_SVC: {
            c->r[REG_PC] = next_pc;
            extern bool exc_enter(cpu_t*, bus_t*, u8);
            if (!exc_enter(c, bus, 11)) { c->halted = true; return false; }
            return true;
        }

        /* === Hi-register operations === */
        case OP_ADD_REG_T2: {
            /* ADD can use hi regs. PC reads as PC+4. */
            u32 a = (i->rn == REG_PC) ? (c->r[REG_PC] + 4) : c->r[i->rn];
            u32 b = (i->rm == REG_PC) ? (c->r[REG_PC] + 4) : c->r[i->rm];
            u32 r = a + b;
            if (i->rd == REG_PC) { next_pc = r & ~1u; break; }
            c->r[i->rd] = r;
            break;
        }
        case OP_CMP_REG_T2: {
            u32 a = (i->rn == REG_PC) ? (c->r[REG_PC] + 4) : c->r[i->rn];
            u32 b = (i->rm == REG_PC) ? (c->r[REG_PC] + 4) : c->r[i->rm];
            cpu_set_flags_nzcv_sub(c, a, b, a - b);
            break;
        }
        case OP_MOV_REG: {
            u32 v = (i->rm == REG_PC) ? (c->r[REG_PC] + 4) : c->r[i->rm];
            if (i->rd == REG_PC) { next_pc = v & ~1u; break; }
            c->r[i->rd] = v;
            break;
        }
        case OP_BX: {
            u32 t = c->r[i->rm];
            /* EXC_RETURN values have bits[31:28] = 1111. Trigger exception exit. */
            if ((t & 0xFFFFFFF0u) == 0xFFFFFFF0u && c->mode == MODE_HANDLER) {
                extern bool exc_return(cpu_t*, bus_t*, u32);
                if (exc_return(c, bus, t)) { return true; }
                c->halted = true; return false;
            }
            /* Cortex-M stays in Thumb; LSB must be 1 per ARM ARM B1.4.1. */
            next_pc = t & ~1u;
            break;
        }
        case OP_BLX_REG: {
            u32 t = c->r[i->rm];
            c->r[REG_LR] = (c->r[REG_PC] + 2) | 1u;
            next_pc = t & ~1u;
            break;
        }

        /* === Load/store === */
        case OP_LDR_LIT: {
            /* PC for literal = (PC + 4) & ~3 (aligned). */
            addr_t a = ((c->r[REG_PC] + 4) & ~3u) + i->imm;
            u32 v = 0;
            if (!bus_read(bus, a, 4, &v)) { c->halted = true; return false; }
            c->r[i->rd] = v;
            break;
        }
        case OP_LDR_IMM: {
            addr_t a = c->r[i->rn] + i->imm;
            u32 v = 0;
            if (!bus_read(bus, a, 4, &v)) { c->halted = true; return false; }
            c->r[i->rd] = v;
            break;
        }
        case OP_STR_IMM: {
            addr_t a = c->r[i->rn] + i->imm;
            if (!bus_write(bus, a, 4, c->r[i->rd])) { c->halted = true; return false; }
            break;
        }
        case OP_LDRB_IMM: {
            addr_t a = c->r[i->rn] + i->imm;
            u32 v = 0;
            if (!bus_read(bus, a, 1, &v)) { c->halted = true; return false; }
            c->r[i->rd] = v & 0xFF;
            break;
        }
        case OP_STRB_IMM: {
            addr_t a = c->r[i->rn] + i->imm;
            if (!bus_write(bus, a, 1, c->r[i->rd] & 0xFF)) { c->halted = true; return false; }
            break;
        }
        case OP_LDRH_IMM: {
            addr_t a = c->r[i->rn] + i->imm;
            u32 v = 0;
            if (!bus_read(bus, a, 2, &v)) { c->halted = true; return false; }
            c->r[i->rd] = v & 0xFFFF;
            break;
        }
        case OP_STRH_IMM: {
            addr_t a = c->r[i->rn] + i->imm;
            if (!bus_write(bus, a, 2, c->r[i->rd] & 0xFFFF)) { c->halted = true; return false; }
            break;
        }
        case OP_LDR_REG: {
            addr_t a = c->r[i->rn] + c->r[i->rm];
            u32 v = 0;
            if (!bus_read(bus, a, 4, &v)) { c->halted = true; return false; }
            c->r[i->rd] = v;
            break;
        }
        case OP_STR_REG: {
            addr_t a = c->r[i->rn] + c->r[i->rm];
            if (!bus_write(bus, a, 4, c->r[i->rd])) { c->halted = true; return false; }
            break;
        }
        case OP_LDRB_REG: {
            addr_t a = c->r[i->rn] + c->r[i->rm];
            u32 v = 0;
            if (!bus_read(bus, a, 1, &v)) { c->halted = true; return false; }
            c->r[i->rd] = v & 0xFF;
            break;
        }
        case OP_STRB_REG: {
            addr_t a = c->r[i->rn] + c->r[i->rm];
            if (!bus_write(bus, a, 1, c->r[i->rd] & 0xFF)) { c->halted = true; return false; }
            break;
        }
        case OP_LDRH_REG: {
            addr_t a = c->r[i->rn] + c->r[i->rm];
            u32 v = 0;
            if (!bus_read(bus, a, 2, &v)) { c->halted = true; return false; }
            c->r[i->rd] = v & 0xFFFF;
            break;
        }
        case OP_STRH_REG: {
            addr_t a = c->r[i->rn] + c->r[i->rm];
            if (!bus_write(bus, a, 2, c->r[i->rd] & 0xFFFF)) { c->halted = true; return false; }
            break;
        }
        case OP_LDRSB_REG: {
            addr_t a = c->r[i->rn] + c->r[i->rm];
            u32 v = 0;
            if (!bus_read(bus, a, 1, &v)) { c->halted = true; return false; }
            c->r[i->rd] = (u32)(i32)(i8)(u8)v;
            break;
        }
        case OP_LDRSH_REG: {
            addr_t a = c->r[i->rn] + c->r[i->rm];
            u32 v = 0;
            if (!bus_read(bus, a, 2, &v)) { c->halted = true; return false; }
            c->r[i->rd] = (u32)(i32)(i16)(u16)v;
            break;
        }
        case OP_LDR_SP: {
            addr_t a = c->r[REG_SP] + i->imm;
            u32 v = 0;
            if (!bus_read(bus, a, 4, &v)) { c->halted = true; return false; }
            c->r[i->rd] = v;
            break;
        }
        case OP_STR_SP: {
            addr_t a = c->r[REG_SP] + i->imm;
            if (!bus_write(bus, a, 4, c->r[i->rd])) { c->halted = true; return false; }
            break;
        }
        case OP_ADR: {
            c->r[i->rd] = ((c->r[REG_PC] + 4) & ~3u) + i->imm;
            break;
        }
        case OP_ADD_SP_IMM: {
            c->r[i->rd] = c->r[REG_SP] + i->imm;
            break;
        }
        case OP_ADD_SP_SP: {
            c->r[REG_SP] += i->imm;
            break;
        }
        case OP_SUB_SP_SP: {
            c->r[REG_SP] -= i->imm;
            break;
        }

        /* === Extend / byte-reverse === */
        case OP_SXTH: c->r[i->rd] = (u32)(i32)(i16)(u16)c->r[i->rm]; break;
        case OP_SXTB: c->r[i->rd] = (u32)(i32)(i8)(u8)c->r[i->rm];   break;
        case OP_UXTH: c->r[i->rd] = c->r[i->rm] & 0xFFFFu;            break;
        case OP_UXTB: c->r[i->rd] = c->r[i->rm] & 0xFFu;              break;
        case OP_REV: {
            u32 v = c->r[i->rm];
            c->r[i->rd] = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
                          ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
            break;
        }
        case OP_REV16: {
            u32 v = c->r[i->rm];
            c->r[i->rd] = ((v & 0xFF) << 8) | ((v >> 8) & 0xFF) |
                          ((v & 0xFF0000) << 8) | ((v >> 8) & 0xFF0000);
            break;
        }
        case OP_REVSH: {
            u32 v = c->r[i->rm];
            u16 low = (u16)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF));
            c->r[i->rd] = (u32)(i32)(i16)low;
            break;
        }

        /* === Stack ops === */
        case OP_PUSH: {
            /* reg_list bits 0..7 = R0..R7, bit 14 = LR. Push from high to low. */
            u32 cnt = 0;
            for (int k = 0; k < 15; ++k) if (i->reg_list & (1u << k)) cnt++;
            addr_t sp = c->r[REG_SP] - cnt * 4;
            c->r[REG_SP] = sp;
            for (int k = 0; k < 15; ++k) {
                if (i->reg_list & (1u << k)) {
                    u32 val = (k == 14) ? c->r[REG_LR] : c->r[k];
                    if (!bus_write(bus, sp, 4, val)) { c->halted = true; return false; }
                    sp += 4;
                }
            }
            break;
        }
        case OP_POP: {
            addr_t sp = c->r[REG_SP];
            u32 pc_val = 0;
            bool pop_pc = false;
            for (int k = 0; k < 16; ++k) {
                if (i->reg_list & (1u << k)) {
                    u32 v = 0;
                    if (!bus_read(bus, sp, 4, &v)) { c->halted = true; return false; }
                    if (k == 15) { pop_pc = true; pc_val = v; }
                    else c->r[k] = v;
                    sp += 4;
                }
            }
            c->r[REG_SP] = sp;
            if (pop_pc) {
                if ((pc_val & 0xFFFFFFF0u) == 0xFFFFFFF0u && c->mode == MODE_HANDLER) {
                    extern bool exc_return(cpu_t*, bus_t*, u32);
                    if (exc_return(c, bus, pc_val)) return true;
                    c->halted = true; return false;
                }
                next_pc = pc_val & ~1u;
            }
            break;
        }
        case OP_STM: {
            addr_t a = c->r[i->rn];
            for (int k = 0; k < 8; ++k) {
                if (i->reg_list & (1u << k)) {
                    if (!bus_write(bus, a, 4, c->r[k])) { c->halted = true; return false; }
                    a += 4;
                }
            }
            c->r[i->rn] = a;
            break;
        }
        case OP_LDM: {
            addr_t a = c->r[i->rn];
            bool wb = (i->reg_list & (1u << i->rn)) == 0;
            for (int k = 0; k < 8; ++k) {
                if (i->reg_list & (1u << k)) {
                    u32 v = 0;
                    if (!bus_read(bus, a, 4, &v)) { c->halted = true; return false; }
                    c->r[k] = v;
                    a += 4;
                }
            }
            if (wb) c->r[i->rn] = a;
            break;
        }

        /* === Hints === */
        case OP_YIELD: case OP_WFE: case OP_WFI: case OP_SEV:
            /* Treat as NOP in Phase 2 (no scheduler, no events). */
            break;
        case OP_BKPT:
            c->halted = true;
            return true;

        case OP_CPS: {
            u32 im = (i->imm >> 2) & 1;
            u32 I_bit = (i->imm >> 1) & 1;
            u32 F_bit = i->imm & 1;
            if (im) {
                /* disable */
                if (I_bit) c->primask = 1;
                if (F_bit) c->faultmask = 1;
            } else {
                /* enable */
                if (I_bit) c->primask = 0;
                if (F_bit) c->faultmask = 0;
            }
            break;
        }

        /* === Thumb-2 branch with link === */
        case OP_T32_BL: {
            c->r[REG_LR] = (c->r[REG_PC] + 4) | 1u;
            next_pc = (c->r[REG_PC] + 4 + i->imm) & ~1u;
            break;
        }
        case OP_T32_B_COND: {
            /* Cond was already pre-checked? No — T32_B_COND doesn't go through IT.
               We must evaluate its own cond field here. */
            u8 cond = i->cond;
            bool nf = (c->apsr & APSR_N) != 0, zf = (c->apsr & APSR_Z) != 0;
            bool cf = (c->apsr & APSR_C) != 0, vf = (c->apsr & APSR_V) != 0;
            bool pass = false;
            switch (cond) {
                case 0x0: pass = zf; break;
                case 0x1: pass = !zf; break;
                case 0x2: pass = cf; break;
                case 0x3: pass = !cf; break;
                case 0x4: pass = nf; break;
                case 0x5: pass = !nf; break;
                case 0x6: pass = vf; break;
                case 0x7: pass = !vf; break;
                case 0x8: pass = cf && !zf; break;
                case 0x9: pass = !cf || zf; break;
                case 0xA: pass = nf == vf; break;
                case 0xB: pass = nf != vf; break;
                case 0xC: pass = !zf && (nf == vf); break;
                case 0xD: pass = zf || (nf != vf); break;
            }
            if (pass) next_pc = c->r[REG_PC] + 4 + i->imm;
            break;
        }

        /* === IT block setup === */
        case OP_T32_IT: {
            c->itstate = (u8)((i->it_first << 4) | i->it_mask);
            break;
        }

        /* === T32 modified immediate data-proc === */
        case OP_T32_AND_IMM: {
            u32 r = c->r[i->rn] & i->imm;
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nz(c, r);
            break;
        }
        case OP_T32_BIC_IMM: {
            u32 r = c->r[i->rn] & ~i->imm;
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nz(c, r);
            break;
        }
        case OP_T32_ORR_IMM: {
            u32 r = c->r[i->rn] | i->imm;
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nz(c, r);
            break;
        }
        case OP_T32_ORN_IMM: {
            u32 r = c->r[i->rn] | ~i->imm;
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nz(c, r);
            break;
        }
        case OP_T32_EOR_IMM: {
            u32 r = c->r[i->rn] ^ i->imm;
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nz(c, r);
            break;
        }
        case OP_T32_TEQ_IMM: {
            u32 r = c->r[i->rn] ^ i->imm;
            cpu_set_flags_nz(c, r);
            break;
        }
        case OP_T32_TST_IMM: {
            u32 r = c->r[i->rn] & i->imm;
            cpu_set_flags_nz(c, r);
            break;
        }
        case OP_T32_ADD_IMM: {
            u32 a = c->r[i->rn], b = i->imm;
            u32 r = a + b;
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nzcv_add(c, a, b, r, false);
            break;
        }
        case OP_T32_ADC_IMM: {
            u32 a = c->r[i->rn], b = i->imm;
            bool ci = (c->apsr & APSR_C) != 0;
            u32 r = a + b + (ci ? 1u : 0u);
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nzcv_add(c, a, b, r, ci);
            break;
        }
        case OP_T32_SBC_IMM: {
            u32 a = c->r[i->rn], b = i->imm;
            bool ci = (c->apsr & APSR_C) != 0;
            u32 nb = ~b;
            u32 r = a + nb + (ci ? 1u : 0u);
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nzcv_add(c, a, nb, r, ci);
            break;
        }
        case OP_T32_SUB_IMM: {
            u32 a = c->r[i->rn], b = i->imm;
            u32 r = a - b;
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nzcv_sub(c, a, b, r);
            break;
        }
        case OP_T32_RSB_IMM: {
            u32 a = i->imm, b = c->r[i->rn];
            u32 r = a - b;
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nzcv_sub(c, a, b, r);
            break;
        }
        case OP_T32_CMN_IMM: {
            u32 a = c->r[i->rn], b = i->imm;
            cpu_set_flags_nzcv_add(c, a, b, a + b, false);
            break;
        }
        case OP_T32_CMP_IMM: {
            u32 a = c->r[i->rn], b = i->imm;
            cpu_set_flags_nzcv_sub(c, a, b, a - b);
            break;
        }
        case OP_T32_MOV_IMM: {
            c->r[i->rd] = i->imm;
            if (i->set_flags) cpu_set_flags_nz(c, i->imm);
            break;
        }
        case OP_T32_MVN_IMM: {
            u32 r = ~i->imm;
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nz(c, r);
            break;
        }

        /* === T32 plain immediate === */
        case OP_T32_ADDW: {
            c->r[i->rd] = c->r[i->rn] + i->imm;
            break;
        }
        case OP_T32_SUBW: {
            c->r[i->rd] = c->r[i->rn] - i->imm;
            break;
        }
        case OP_T32_MOVW: {
            c->r[i->rd] = i->imm; /* zero-extended 16-bit */
            break;
        }
        case OP_T32_MOVT: {
            c->r[i->rd] = (c->r[i->rd] & 0x0000FFFFu) | ((i->imm & 0xFFFF) << 16);
            break;
        }
        case OP_T32_ADR_T2: {
            /* SUBW Rd, PC, #imm12 — PC-aligned base */
            c->r[i->rd] = ((c->r[REG_PC] + 4) & ~3u) - i->imm;
            break;
        }
        case OP_T32_ADR_T3: {
            c->r[i->rd] = ((c->r[REG_PC] + 4) & ~3u) + i->imm;
            break;
        }

        /* === T32 register data-proc with shift === */
        case OP_T32_AND_REG: {
            u32 b = t32_shift(c->r[i->rm], i->shift_type, i->shift_n);
            u32 r = c->r[i->rn] & b;
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nz(c, r);
            break;
        }
        case OP_T32_BIC_REG: {
            u32 b = t32_shift(c->r[i->rm], i->shift_type, i->shift_n);
            u32 r = c->r[i->rn] & ~b;
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nz(c, r);
            break;
        }
        case OP_T32_ORR_REG: {
            u32 b = t32_shift(c->r[i->rm], i->shift_type, i->shift_n);
            u32 r = c->r[i->rn] | b;
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nz(c, r);
            break;
        }
        case OP_T32_ORN_REG: {
            u32 b = t32_shift(c->r[i->rm], i->shift_type, i->shift_n);
            u32 r = c->r[i->rn] | ~b;
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nz(c, r);
            break;
        }
        case OP_T32_EOR_REG: {
            u32 b = t32_shift(c->r[i->rm], i->shift_type, i->shift_n);
            u32 r = c->r[i->rn] ^ b;
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nz(c, r);
            break;
        }
        case OP_T32_TEQ_REG: {
            u32 b = t32_shift(c->r[i->rm], i->shift_type, i->shift_n);
            cpu_set_flags_nz(c, c->r[i->rn] ^ b);
            break;
        }
        case OP_T32_TST_REG: {
            u32 b = t32_shift(c->r[i->rm], i->shift_type, i->shift_n);
            cpu_set_flags_nz(c, c->r[i->rn] & b);
            break;
        }
        case OP_T32_MOV_REG: {
            u32 v = t32_shift(c->r[i->rm], i->shift_type, i->shift_n);
            c->r[i->rd] = v;
            if (i->set_flags) cpu_set_flags_nz(c, v);
            break;
        }
        case OP_T32_MVN_REG: {
            u32 v = ~t32_shift(c->r[i->rm], i->shift_type, i->shift_n);
            c->r[i->rd] = v;
            if (i->set_flags) cpu_set_flags_nz(c, v);
            break;
        }
        case OP_T32_ADD_REG: {
            u32 a = c->r[i->rn];
            u32 b = t32_shift(c->r[i->rm], i->shift_type, i->shift_n);
            u32 r = a + b;
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nzcv_add(c, a, b, r, false);
            break;
        }
        case OP_T32_ADC_REG: {
            u32 a = c->r[i->rn];
            u32 b = t32_shift(c->r[i->rm], i->shift_type, i->shift_n);
            bool ci = (c->apsr & APSR_C) != 0;
            u32 r = a + b + (ci ? 1u : 0u);
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nzcv_add(c, a, b, r, ci);
            break;
        }
        case OP_T32_SBC_REG: {
            u32 a = c->r[i->rn];
            u32 b = t32_shift(c->r[i->rm], i->shift_type, i->shift_n);
            bool ci = (c->apsr & APSR_C) != 0;
            u32 nb = ~b;
            u32 r = a + nb + (ci ? 1u : 0u);
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nzcv_add(c, a, nb, r, ci);
            break;
        }
        case OP_T32_SUB_REG: {
            u32 a = c->r[i->rn];
            u32 b = t32_shift(c->r[i->rm], i->shift_type, i->shift_n);
            u32 r = a - b;
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nzcv_sub(c, a, b, r);
            break;
        }
        case OP_T32_RSB_REG: {
            u32 a = t32_shift(c->r[i->rm], i->shift_type, i->shift_n);
            u32 b = c->r[i->rn];
            u32 r = a - b;
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nzcv_sub(c, a, b, r);
            break;
        }
        case OP_T32_CMN_REG: {
            u32 a = c->r[i->rn];
            u32 b = t32_shift(c->r[i->rm], i->shift_type, i->shift_n);
            cpu_set_flags_nzcv_add(c, a, b, a + b, false);
            break;
        }
        case OP_T32_CMP_REG: {
            u32 a = c->r[i->rn];
            u32 b = t32_shift(c->r[i->rm], i->shift_type, i->shift_n);
            cpu_set_flags_nzcv_sub(c, a, b, a - b);
            break;
        }

        /* === T32 load/store immediate === */
        case OP_T32_LDR_IMM:
        case OP_T32_LDRH_IMM:
        case OP_T32_LDRB_IMM:
        case OP_T32_LDRSB_IMM:
        case OP_T32_LDRSH_IMM:
        case OP_T32_STR_IMM:
        case OP_T32_STRH_IMM:
        case OP_T32_STRB_IMM: {
            u32 base = c->r[i->rn];
            u32 off  = i->imm;
            u32 a    = i->add ? base + off : base - off;
            addr_t ea = i->index ? a : base;
            u32 sz = (i->op == OP_T32_LDR_IMM || i->op == OP_T32_STR_IMM) ? 4
                   : (i->op == OP_T32_LDRH_IMM || i->op == OP_T32_LDRSH_IMM ||
                      i->op == OP_T32_STRH_IMM) ? 2 : 1;
            bool is_load = (i->op == OP_T32_LDR_IMM || i->op == OP_T32_LDRH_IMM ||
                            i->op == OP_T32_LDRB_IMM || i->op == OP_T32_LDRSB_IMM ||
                            i->op == OP_T32_LDRSH_IMM);
            if (is_load) {
                u32 v = 0;
                if (!bus_read(bus, ea, sz, &v)) { c->halted = true; return false; }
                if (i->op == OP_T32_LDRSB_IMM) v = (u32)(i32)(i8)(u8)v;
                else if (i->op == OP_T32_LDRSH_IMM) v = (u32)(i32)(i16)(u16)v;
                c->r[i->rd] = v;
            } else {
                if (!bus_write(bus, ea, sz, c->r[i->rd])) {
                    c->halted = true; return false;
                }
            }
            if (i->writeback) c->r[i->rn] = a;
            break;
        }

        case OP_T32_LDR_LIT: {
            addr_t base = (c->r[REG_PC] + 4) & ~3u;
            addr_t ea = i->add ? base + i->imm : base - i->imm;
            u32 v = 0;
            if (!bus_read(bus, ea, 4, &v)) { c->halted = true; return false; }
            c->r[i->rd] = v;
            break;
        }

        case OP_T32_LDR_REG: {
            u32 ea = c->r[i->rn] + (c->r[i->rm] << i->shift_n);
            u32 v = 0;
            if (!bus_read(bus, ea, 4, &v)) { c->halted = true; return false; }
            c->r[i->rd] = v;
            break;
        }
        case OP_T32_STR_REG: {
            u32 ea = c->r[i->rn] + (c->r[i->rm] << i->shift_n);
            if (!bus_write(bus, ea, 4, c->r[i->rd])) { c->halted = true; return false; }
            break;
        }

        case OP_T32_LDRD_IMM: {
            u32 base = c->r[i->rn];
            u32 off  = i->imm;
            u32 a    = i->add ? base + off : base - off;
            addr_t ea = i->index ? a : base;
            u32 v0 = 0, v1 = 0;
            if (!bus_read(bus, ea,     4, &v0)) { c->halted = true; return false; }
            if (!bus_read(bus, ea + 4, 4, &v1)) { c->halted = true; return false; }
            c->r[i->rd] = v0;
            c->r[i->rs] = v1;
            if (i->writeback) c->r[i->rn] = a;
            break;
        }
        case OP_T32_STRD_IMM: {
            u32 base = c->r[i->rn];
            u32 off  = i->imm;
            u32 a    = i->add ? base + off : base - off;
            addr_t ea = i->index ? a : base;
            if (!bus_write(bus, ea,     4, c->r[i->rd])) { c->halted = true; return false; }
            if (!bus_write(bus, ea + 4, 4, c->r[i->rs])) { c->halted = true; return false; }
            if (i->writeback) c->r[i->rn] = a;
            break;
        }

        case OP_T32_NOP:
            break;

        /* === T32 Load/Store multiple (IA increment-after / DB decrement-before) === */
        case OP_T32_LDM:
        case OP_T32_STM: {
            bool is_load = (i->op == OP_T32_LDM);
            u32 cnt = 0;
            for (int k = 0; k < 16; ++k) if (i->reg_list & (1u << k)) cnt++;
            addr_t base = c->r[i->rn];
            addr_t a = i->add ? base : (base - cnt * 4);
            addr_t start = a;
            for (int k = 0; k < 16; ++k) {
                if (i->reg_list & (1u << k)) {
                    if (is_load) {
                        u32 v = 0;
                        if (!bus_read(bus, a, 4, &v)) { c->halted = true; return false; }
                        if (k == 15) next_pc = v & ~1u;
                        else c->r[k] = v;
                    } else {
                        u32 v = (k == 15) ? (c->r[REG_PC] + 4) : c->r[k];
                        if (!bus_write(bus, a, 4, v)) { c->halted = true; return false; }
                    }
                    a += 4;
                }
            }
            if (i->writeback) {
                c->r[i->rn] = i->add ? a : start;
            }
            break;
        }

        /* === T32 multiply / divide === */
        case OP_T32_MUL: {
            c->r[i->rd] = c->r[i->rn] * c->r[i->rm];
            break;
        }
        case OP_T32_MLA: {
            c->r[i->rd] = c->r[i->rn] * c->r[i->rm] + c->r[i->rs];
            break;
        }
        case OP_T32_MLS: {
            c->r[i->rd] = c->r[i->rs] - c->r[i->rn] * c->r[i->rm];
            break;
        }
        case OP_T32_UMULL: {
            u64 p = (u64)c->r[i->rn] * (u64)c->r[i->rm];
            c->r[i->rd] = (u32)(p & 0xFFFFFFFFu);  /* RdLo */
            c->r[i->rs] = (u32)(p >> 32);           /* RdHi */
            break;
        }
        case OP_T32_SMULL: {
            i64 p = (i64)(i32)c->r[i->rn] * (i64)(i32)c->r[i->rm];
            c->r[i->rd] = (u32)((u64)p & 0xFFFFFFFFu);
            c->r[i->rs] = (u32)((u64)p >> 32);
            break;
        }
        case OP_T32_UMLAL: {
            u64 acc = ((u64)c->r[i->rs] << 32) | c->r[i->rd];
            u64 p   = (u64)c->r[i->rn] * (u64)c->r[i->rm];
            u64 r   = acc + p;
            c->r[i->rd] = (u32)(r & 0xFFFFFFFFu);
            c->r[i->rs] = (u32)(r >> 32);
            break;
        }
        case OP_T32_SMLAL: {
            u64 acc = ((u64)c->r[i->rs] << 32) | c->r[i->rd];
            i64 p   = (i64)(i32)c->r[i->rn] * (i64)(i32)c->r[i->rm];
            u64 r   = acc + (u64)p;
            c->r[i->rd] = (u32)(r & 0xFFFFFFFFu);
            c->r[i->rs] = (u32)(r >> 32);
            break;
        }
        case OP_T32_UDIV: {
            u32 b = c->r[i->rm];
            c->r[i->rd] = b ? (c->r[i->rn] / b) : 0u;
            break;
        }
        case OP_T32_SDIV: {
            i32 b = (i32)c->r[i->rm];
            if (b == 0) c->r[i->rd] = 0;
            else if (b == -1 && c->r[i->rn] == 0x80000000u) c->r[i->rd] = 0x80000000u;
            else c->r[i->rd] = (u32)((i32)c->r[i->rn] / b);
            break;
        }

        /* === MRS / MSR — access special registers (ARM ARM B1.4.4) === */
        case OP_T32_MRS: {
            u32 sysm = i->imm;
            u32 v = 0;
            switch (sysm) {
                case 0: case 1: case 2: case 3:
                    v = c->apsr | c->ipsr | c->epsr;
                    if (sysm == 1) v = c->ipsr;        /* IPSR only */
                    if (sysm == 2) v = c->epsr;        /* EPSR only */
                    break;
                case 5: v = c->ipsr; break;
                case 8: v = c->msp; break;
                case 9: v = c->psp; break;
                case 16: v = c->primask; break;
                case 17: v = c->basepri; break;
                case 19: v = c->faultmask; break;
                case 20: v = c->control; break;
                default: break;
            }
            c->r[i->rd] = v;
            break;
        }
        case OP_T32_MSR: {
            u32 sysm = i->imm;
            u32 v = c->r[i->rn];
            switch (sysm) {
                case 0: case 1: case 2: case 3:
                    /* mask bit[1] writes NZCVQ of APSR */
                    if (i->rs & 2) c->apsr = (c->apsr & ~0xF8000000u) | (v & 0xF8000000u);
                    break;
                case 8: c->msp = v & ~3u; if (c->mode == MODE_THREAD && !(c->control & 2)) c->r[REG_SP] = c->msp; break;
                case 9: c->psp = v & ~3u; if (c->mode == MODE_THREAD && (c->control & 2)) c->r[REG_SP] = c->psp; break;
                case 16: c->primask = v & 1u; break;
                case 17: c->basepri = v & 0xFFu; break;
                case 19: c->faultmask = v & 1u; break;
                case 20: {
                    /* CONTROL: bit[0] nPriv, bit[1] SPSEL (0=MSP, 1=PSP) */
                    u32 old_spsel = c->control & 2u;
                    c->control = v & 0x3u;
                    u32 new_spsel = c->control & 2u;
                    if (c->mode == MODE_THREAD && old_spsel != new_spsel) {
                        if (new_spsel) { c->msp = c->r[REG_SP]; c->r[REG_SP] = c->psp; }
                        else           { c->psp = c->r[REG_SP]; c->r[REG_SP] = c->msp; }
                    }
                    break;
                }
                default: break;
            }
            break;
        }

        /* === T32 shift register (Rd = shift(Rn, Rm)) === */
        case OP_T32_LSL_R: {
            u32 sh = c->r[i->rm] & 0xFF;
            u32 r = sh >= 32 ? 0 : (c->r[i->rn] << sh);
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nz(c, r);
            break;
        }
        case OP_T32_LSR_R: {
            u32 sh = c->r[i->rm] & 0xFF;
            u32 r = sh >= 32 ? 0 : (c->r[i->rn] >> sh);
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nz(c, r);
            break;
        }
        case OP_T32_ASR_R: {
            u32 sh = c->r[i->rm] & 0xFF;
            i32 v = (i32)c->r[i->rn];
            i32 r = sh >= 32 ? (v >> 31) : (v >> sh);
            c->r[i->rd] = (u32)r;
            if (i->set_flags) cpu_set_flags_nz(c, (u32)r);
            break;
        }
        case OP_T32_ROR_R: {
            u32 sh = c->r[i->rm] & 0x1F;
            u32 v = c->r[i->rn];
            u32 r = sh ? ((v >> sh) | (v << (32 - sh))) : v;
            c->r[i->rd] = r;
            if (i->set_flags) cpu_set_flags_nz(c, r);
            break;
        }

        /* === Bitfield ops === */
        case OP_T32_BFI: {
            u32 lsb = i->imm & 0x1F;
            u32 msb = (i->imm >> 8) & 0x1F;
            u32 width = msb - lsb + 1;
            u32 mask = width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1u);
            u32 src = c->r[i->rn] & mask;
            c->r[i->rd] = (c->r[i->rd] & ~(mask << lsb)) | (src << lsb);
            break;
        }
        case OP_T32_BFC: {
            u32 lsb = i->imm & 0x1F;
            u32 msb = (i->imm >> 8) & 0x1F;
            u32 width = msb - lsb + 1;
            u32 mask = width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1u);
            c->r[i->rd] = c->r[i->rd] & ~(mask << lsb);
            break;
        }
        case OP_T32_UBFX: {
            u32 lsb = i->imm & 0x1F;
            u32 widthm1 = (i->imm >> 8) & 0x1F;
            u32 width = widthm1 + 1;
            u32 mask = width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1u);
            c->r[i->rd] = (c->r[i->rn] >> lsb) & mask;
            break;
        }
        case OP_T32_SBFX: {
            u32 lsb = i->imm & 0x1F;
            u32 widthm1 = (i->imm >> 8) & 0x1F;
            u32 width = widthm1 + 1;
            u32 val = (c->r[i->rn] >> lsb) & (width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1u));
            u32 sign = 1u << (width - 1);
            c->r[i->rd] = (val ^ sign) - sign; /* sign-extend */
            break;
        }
        case OP_T32_CLZ: {
            u32 v = c->r[i->rm];
            if (v == 0) { c->r[i->rd] = 32; break; }
            u32 n = 0;
            while ((v & 0x80000000u) == 0) { n++; v <<= 1; }
            c->r[i->rd] = n;
            break;
        }
        case OP_T32_RBIT: {
            u32 v = c->r[i->rm];
            v = ((v & 0xAAAAAAAAu) >> 1) | ((v & 0x55555555u) << 1);
            v = ((v & 0xCCCCCCCCu) >> 2) | ((v & 0x33333333u) << 2);
            v = ((v & 0xF0F0F0F0u) >> 4) | ((v & 0x0F0F0F0Fu) << 4);
            v = ((v & 0xFF00FF00u) >> 8) | ((v & 0x00FF00FFu) << 8);
            v = (v >> 16) | (v << 16);
            c->r[i->rd] = v;
            break;
        }

        /* === Table branch byte / halfword === */
        case OP_T32_TBB: {
            addr_t base = (i->rn == REG_PC) ? (c->r[REG_PC] + 4) : c->r[i->rn];
            addr_t idx  = base + c->r[i->rm];
            u32 v = 0;
            if (!bus_read(bus, idx, 1, &v)) { c->halted = true; return false; }
            next_pc = c->r[REG_PC] + 4 + (v * 2);
            break;
        }
        case OP_T32_TBH: {
            addr_t base = (i->rn == REG_PC) ? (c->r[REG_PC] + 4) : c->r[i->rn];
            addr_t idx  = base + (c->r[i->rm] << 1);
            u32 v = 0;
            if (!bus_read(bus, idx, 2, &v)) { c->halted = true; return false; }
            next_pc = c->r[REG_PC] + 4 + (v * 2);
            break;
        }

        default:
            c->halted = true;
            return false;
    }

    /* Restore APSR if inside IT block and the insn isn't a comparison. */
    if (in_it && !op_is_flagsetter(i->op)) {
        c->apsr = saved_apsr;
    }

    /* Advance IT state if we executed a non-IT instruction inside an IT block. */
    if (in_it && i->op != OP_T32_IT) {
        cpu_it_advance(c);
    }

    c->r[REG_PC] = next_pc;
    c->cycles++;
    return true;
}
