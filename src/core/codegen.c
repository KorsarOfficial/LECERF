#include "core/codegen.h"
#include <stddef.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/mman.h>
#endif

/* x86 emit. rdi=cpu, [rdi + R_OFF + i*4] = r[i]. */
#define R_OFF ((int)offsetof(cpu_t, r))
#define PC_OFF (R_OFF + 15 * 4)

static void emit_b(codegen_t* cg, u8 b) {
    if (cg->used < cg->capacity) cg->buffer[cg->used++] = b;
}
static void emit_w16(codegen_t* cg, u16 v) { emit_b(cg, (u8)v); emit_b(cg, (u8)(v >> 8)); }
static void emit_w32(codegen_t* cg, u32 v) {
    emit_b(cg, (u8)v); emit_b(cg, (u8)(v >> 8));
    emit_b(cg, (u8)(v >> 16)); emit_b(cg, (u8)(v >> 24));
}

/* mov eax, [rdi + R_OFF + r*4] */
static void ld_eax(codegen_t* cg, u8 r) {
    emit_b(cg, 0x8B); emit_b(cg, 0x87);
    emit_w32(cg, (u32)(R_OFF + r * 4));
}
/* mov ecx, [rdi + R_OFF + r*4] */
static void ld_ecx(codegen_t* cg, u8 r) {
    emit_b(cg, 0x8B); emit_b(cg, 0x8F);
    emit_w32(cg, (u32)(R_OFF + r * 4));
}
/* mov [rdi + R_OFF + r*4], eax */
static void st_eax(codegen_t* cg, u8 r) {
    emit_b(cg, 0x89); emit_b(cg, 0x87);
    emit_w32(cg, (u32)(R_OFF + r * 4));
}
/* pc <- imm */
static void st_pc(codegen_t* cg, u32 pc) {
    emit_b(cg, 0xC7); emit_b(cg, 0x87);
    emit_w32(cg, (u32)PC_OFF); emit_w32(cg, pc);
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
static void ret_true(codegen_t* cg) {
    emit_b(cg, 0xB0); emit_b(cg, 0x01);
    emit_b(cg, 0xC3);
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
            return true;
        default:
            return false;
    }
}

static void emit_op(codegen_t* cg, const insn_t* i) {
    switch (i->op) {
        case OP_NOP: case OP_T32_NOP: break;

        case OP_MOV_IMM: case OP_T32_MOV_IMM: case OP_T32_MOVW:
            mov_eax_imm(cg, i->imm); st_eax(cg, i->rd); break;

        case OP_MOV_REG:
            ld_eax(cg, i->rm); st_eax(cg, i->rd); break;

        case OP_ADD_REG:
            ld_eax(cg, i->rn); ld_ecx(cg, i->rm); op_add_ec(cg); st_eax(cg, i->rd); break;

        case OP_SUB_REG:
            ld_eax(cg, i->rn); ld_ecx(cg, i->rm); op_sub_ec(cg); st_eax(cg, i->rd); break;

        case OP_ADD_IMM3: case OP_ADD_IMM8:
        case OP_T32_ADD_IMM: case OP_T32_ADDW:
            ld_eax(cg, i->rn); add_imm(cg, i->imm); st_eax(cg, i->rd); break;

        case OP_SUB_IMM3: case OP_SUB_IMM8:
        case OP_T32_SUB_IMM: case OP_T32_SUBW:
            ld_eax(cg, i->rn); sub_imm(cg, i->imm); st_eax(cg, i->rd); break;

        case OP_AND_REG:
            ld_eax(cg, i->rn); ld_ecx(cg, i->rm); op_and_ec(cg); st_eax(cg, i->rd); break;
        case OP_T32_AND_IMM:
            ld_eax(cg, i->rn);
            emit_b(cg, 0x25); emit_w32(cg, i->imm);
            st_eax(cg, i->rd); break;

        case OP_ORR_REG:
            ld_eax(cg, i->rn); ld_ecx(cg, i->rm); op_or_ec(cg); st_eax(cg, i->rd); break;
        case OP_T32_ORR_IMM:
            ld_eax(cg, i->rn);
            emit_b(cg, 0x0D); emit_w32(cg, i->imm);
            st_eax(cg, i->rd); break;

        case OP_EOR_REG:
            ld_eax(cg, i->rn); ld_ecx(cg, i->rm); op_xor_ec(cg); st_eax(cg, i->rd); break;
        case OP_T32_EOR_IMM:
            ld_eax(cg, i->rn);
            emit_b(cg, 0x35); emit_w32(cg, i->imm);
            st_eax(cg, i->rd); break;

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

cg_thunk_t codegen_emit(codegen_t* cg, const insn_t* ins, u8 n) {
    for (u8 k = 0; k < n; ++k) if (!codegen_supports(ins[k].op)) return NULL;
    if (cg->used + (u32)n * 64 > cg->capacity) return NULL;
    u8* start = cg->buffer + cg->used;
    for (u8 k = 0; k < n; ++k) emit_op(cg, &ins[k]);
    u32 last = ins[n - 1].pc + ins[n - 1].size;
    st_pc(cg, last);
    ret_true(cg);
    return (cg_thunk_t)start;
}
