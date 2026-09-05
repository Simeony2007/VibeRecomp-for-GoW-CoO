#include "mips_interpreter.h"
#include <stdio.h>
#include <stdlib.h>

// Extern routing function to our HLE Kernel
extern void hle_syscall(MIPS_CPU *cpu);

void mips_interpreter_step(MIPS_CPU *cpu) {
    uint32_t pc = cpu->pc;
    uint32_t inst = mips_read32(cpu, pc);

    // Resolve branch delay slot paradigm
    cpu->pc = cpu->next_pc;
    cpu->next_pc = cpu->pc + 4;

    uint32_t opcode = inst >> 26;
    uint32_t rs = (inst >> 21) & 0x1F;
    uint32_t rt = (inst >> 16) & 0x1F;
    uint32_t rd = (inst >> 11) & 0x1F;
    uint32_t shamt = (inst >> 6) & 0x1F;
    uint32_t funct = inst & 0x3F;

    int16_t simm = (int16_t)(inst & 0xFFFF);
    uint16_t imm = inst & 0xFFFF;
    uint32_t target = inst & 0x03FFFFFF;

    bool branch_taken = false;
    uint32_t branch_target = 0;

    cpu->cycles++;

    switch (opcode) {
        case 0x00: { // SPECIAL / R-Type
            switch (funct) {
                case 0x00: // SLL
                    if (rd != 0) cpu->gpr[rd] = cpu->gpr[rt] << shamt;
                    break;
                case 0x02: // SRL
                    if (rd != 0) cpu->gpr[rd] = cpu->gpr[rt] >> shamt;
                    break;
                case 0x03: // SRA
                    if (rd != 0) cpu->gpr[rd] = (int32_t)cpu->gpr[rt] >> shamt;
                    break;
                case 0x04: // SLLV
                    if (rd != 0) cpu->gpr[rd] = cpu->gpr[rt] << (cpu->gpr[rs] & 0x1F);
                    break;
                case 0x06: // SRLV
                    if (rd != 0) cpu->gpr[rd] = cpu->gpr[rt] >> (cpu->gpr[rs] & 0x1F);
                    break;
                case 0x07: // SRAV
                    if (rd != 0) cpu->gpr[rd] = (int32_t)cpu->gpr[rt] >> (cpu->gpr[rs] & 0x1F);
                    break;
                case 0x08: // JR
                    branch_taken = true;
                    branch_target = cpu->gpr[rs];
                    break;
                case 0x09: // JALR
                    if (rd != 0) cpu->gpr[rd] = pc + 8;
                    branch_taken = true;
                    branch_target = cpu->gpr[rs];
                    break;
                case 0x0C: // SYSCALL
                    cpu->pc = pc; // Restores exact PC of the syscall instruction for correct HLE routing
                    hle_syscall(cpu);
                    break;
                case 0x0D: // BREAK
                    cpu->exit_requested = true;
                    break;
                case 0x20: // ADD
                case 0x21: // ADDU
                    if (rd != 0) cpu->gpr[rd] = cpu->gpr[rs] + cpu->gpr[rt];
                    break;
                case 0x22: // SUB
                case 0x23: // SUBU
                    if (rd != 0) cpu->gpr[rd] = cpu->gpr[rs] - cpu->gpr[rt];
                    break;
                case 0x24: // AND
                    if (rd != 0) cpu->gpr[rd] = cpu->gpr[rs] & cpu->gpr[rt];
                    break;
                case 0x25: // OR
                    if (rd != 0) cpu->gpr[rd] = cpu->gpr[rs] | cpu->gpr[rt];
                    break;
                case 0x26: // XOR
                    if (rd != 0) cpu->gpr[rd] = cpu->gpr[rs] ^ cpu->gpr[rt];
                    break;
                case 0x27: // NOR
                    if (rd != 0) cpu->gpr[rd] = ~(cpu->gpr[rs] | cpu->gpr[rt]);
                    break;
                case 0x2A: // SLT
                    if (rd != 0) cpu->gpr[rd] = ((int32_t)cpu->gpr[rs] < (int32_t)cpu->gpr[rt]) ? 1 : 0;
                    break;
                case 0x2B: // SLTU
                    if (rd != 0) cpu->gpr[rd] = (cpu->gpr[rs] < cpu->gpr[rt]) ? 1 : 0;
                    break;
                default:
                    printf("[Interpreter Warning] Unsupported R-Type funct 0x%02X at PC 0x%08X (Inst: 0x%08X)\n", funct, pc, inst);
                    break;
            }
            break;
        }
        case 0x01: { // REGIMM
            if (rt == 0) { // BLTZ
                if ((int32_t)cpu->gpr[rs] < 0) {
                    branch_taken = true;
                    branch_target = pc + 4 + (simm * 4);
                }
            } else if (rt == 1) { // BGEZ
                if ((int32_t)cpu->gpr[rs] >= 0) {
                    branch_taken = true;
                    branch_target = pc + 4 + (simm * 4);
                }
            }
            break;
        }
        case 0x02: // J
            branch_taken = true;
            branch_target = (pc & 0xF0000000) | (target * 4);
            break;
        case 0x03: // JAL
            cpu->gpr[31] = pc + 8; // Link register r31
            branch_taken = true;
            branch_target = (pc & 0xF0000000) | (target * 4);
            break;
        case 0x04: // BEQ
            if (cpu->gpr[rs] == cpu->gpr[rt]) {
                branch_taken = true;
                branch_target = pc + 4 + (simm * 4);
            }
            break;
        case 0x05: // BNE
            if (cpu->gpr[rs] != cpu->gpr[rt]) {
                branch_taken = true;
                branch_target = pc + 4 + (simm * 4);
            }
            break;
        case 0x06: // BLEZ
            if ((int32_t)cpu->gpr[rs] <= 0) {
                branch_taken = true;
                branch_target = pc + 4 + (simm * 4);
            }
            break;
        case 0x07: // BGTZ
            if ((int32_t)cpu->gpr[rs] > 0) {
                branch_taken = true;
                branch_target = pc + 4 + (simm * 4);
            }
            break;
        case 0x08: // ADDI
        case 0x09: // ADDIU
            if (rt != 0) {
                cpu->gpr[rt] = cpu->gpr[rs] + simm;
            }
            break;
        case 0x0A: // SLTI
            if (rt != 0) {
                cpu->gpr[rt] = ((int32_t)cpu->gpr[rs] < simm) ? 1 : 0;
            }
            break;
        case 0x0B: // SLTIU
            if (rt != 0) {
                cpu->gpr[rt] = (cpu->gpr[rs] < (uint32_t)simm) ? 1 : 0;
            }
            break;
        case 0x0C: // ANDI
            if (rt != 0) {
                cpu->gpr[rt] = cpu->gpr[rs] & imm;
            }
            break;
        case 0x0D: // ORI
            if (rt != 0) {
                cpu->gpr[rt] = cpu->gpr[rs] | imm;
            }
            break;
        case 0x0E: // XORI
            if (rt != 0) {
                cpu->gpr[rt] = cpu->gpr[rs] ^ imm;
            }
            break;
        case 0x0F: // LUI
            if (rt != 0) {
                cpu->gpr[rt] = imm << 16;
            }
            break;
        case 0x20: // LB
            if (rt != 0) cpu->gpr[rt] = (int32_t)(int8_t)mips_read8(cpu, cpu->gpr[rs] + simm);
            break;
        case 0x21: // LH
            if (rt != 0) cpu->gpr[rt] = (int32_t)(int16_t)mips_read16(cpu, cpu->gpr[rs] + simm);
            break;
        case 0x23: // LW
            if (rt != 0) cpu->gpr[rt] = mips_read32(cpu, cpu->gpr[rs] + simm);
            break;
        case 0x24: // LBU
            if (rt != 0) cpu->gpr[rt] = mips_read8(cpu, cpu->gpr[rs] + simm);
            break;
        case 0x25: // LHU
            if (rt != 0) cpu->gpr[rt] = mips_read16(cpu, cpu->gpr[rs] + simm);
            break;
        case 0x28: // SB
            mips_write8(cpu, cpu->gpr[rs] + simm, (uint8_t)cpu->gpr[rt]);
            break;
        case 0x29: // SH
            mips_write16(cpu, cpu->gpr[rs] + simm, (uint16_t)cpu->gpr[rt]);
            break;
        case 0x2B: // SW
            mips_write32(cpu, cpu->gpr[rs] + simm, cpu->gpr[rt]);
            break;
        case 0x1C: { // SPECIAL2 (Allegrex custom: clz, clo, halt)
            if (inst == 0x70000000) { // HALT
                cpu->exit_requested = true;
                return;
            }
            switch (funct) {
                case 0x20: // CLZ
                    if (rd != 0) {
                        uint32_t val = cpu->gpr[rs];
                        cpu->gpr[rd] = val ? __builtin_clz(val) : 32;
                    }
                    break;
                case 0x21: // CLO
                    if (rd != 0) {
                        uint32_t val = ~cpu->gpr[rs];
                        cpu->gpr[rd] = val ? __builtin_clz(val) : 32;
                    }
                    break;
                default:
                    printf("[Interpreter Warning] Unsupported SPECIAL2 funct 0x%02X\n", funct);
                    break;
            }
            break;
        }
        default:
            printf("[Interpreter Warning] Unsupported opcode 0x%02X at PC 0x%08X\n", opcode, pc);
            break;
    }

    // Process branch delay slot
    if (branch_taken) {
        cpu->next_pc = branch_target;
    }
}
