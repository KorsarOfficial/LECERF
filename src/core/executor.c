#include "core/cpu.h"
#include "core/bus.h"
#include "core/decoder.h"

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

/* Executor: given decoded insn, mutate CPU/bus state. Returns false on fault. */
bool execute(cpu_t* c, bus_t* bus, const insn_t* i) {
    /* Advance PC to next by default; branches override. */
    u32 next_pc = c->r[REG_PC] + i->size;

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

        case OP_UDF:
            c->halted = true;
            return false;

        case OP_SVC:
            c->halted = true;
            return true;

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
            for (int k = 0; k < 16; ++k) {
                if (i->reg_list & (1u << k)) {
                    u32 v = 0;
                    if (!bus_read(bus, sp, 4, &v)) { c->halted = true; return false; }
                    if (k == 15) {
                        next_pc = v & ~1u; /* POP PC — interworking */
                    } else {
                        c->r[k] = v;
                    }
                    sp += 4;
                }
            }
            c->r[REG_SP] = sp;
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

        /* === Thumb-2 branch with link === */
        case OP_T32_BL: {
            /* LR = PC_of_next_insn | 1 (Thumb bit). */
            c->r[REG_LR] = (c->r[REG_PC] + 4) | 1u;
            next_pc = (c->r[REG_PC] + 4 + i->imm) & ~1u;
            break;
        }

        default:
            c->halted = true;
            return false;
    }

    c->r[REG_PC] = next_pc;
    c->cycles++;
    return true;
}
