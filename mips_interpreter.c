#include "mips_interpreter.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

// Extern routing function to our HLE Kernel
extern void hle_syscall(MIPS_CPU *cpu);

void mips_interpreter_step(MIPS_CPU *cpu) {
    cpu->gpr[0] = 0; // Guard-rail: Enforce MIPS zero-register hardwired invariant
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
                case 0x0A: // MOVZ (Move on Zero - PSP Allegrex)
                    if (rd != 0 && cpu->gpr[rt] == 0) {
                        cpu->gpr[rd] = cpu->gpr[rs];
                    }
                    break;
                case 0x0B: // MOVN (Move on Not Zero - PSP Allegrex)
                    if (rd != 0 && cpu->gpr[rt] != 0) {
                        cpu->gpr[rd] = cpu->gpr[rs];
                    }
                    break;
                case 0x0C: // SYSCALL
                    cpu->pc = pc; // Restores exact PC of the syscall instruction for correct HLE routing
                    hle_syscall(cpu);
                    break;
                case 0x0D: // BREAK
                    cpu->exit_requested = true;
                    break;
                case 0x10: // MFHI (Move From HI)
                    if (rd != 0) cpu->gpr[rd] = cpu->hi;
                    break;
                case 0x11: // MTHI (Move To HI)
                    cpu->hi = cpu->gpr[rs];
                    break;
                case 0x12: // MFLO (Move From LO)
                    if (rd != 0) cpu->gpr[rd] = cpu->lo;
                    break;
                case 0x13: // MTLO (Move To LO)
                    cpu->lo = cpu->gpr[rs];
                    break;
                case 0x18: { // MULT (Signed Multiply)
                    int64_t res = (int64_t)(int32_t)cpu->gpr[rs] * (int64_t)(int32_t)cpu->gpr[rt];
                    cpu->lo = (uint32_t)(res & 0xFFFFFFFF);
                    cpu->hi = (uint32_t)(res >> 32);
                    break;
                }
                case 0x19: { // MULTU (Unsigned Multiply)
                    uint64_t res = (uint64_t)cpu->gpr[rs] * (uint64_t)cpu->gpr[rt];
                    cpu->lo = (uint32_t)(res & 0xFFFFFFFF);
                    cpu->hi = (uint32_t)(res >> 32);
                    break;
                }
                case 0x1A: { // DIV (Signed Divide)
                    if (cpu->gpr[rt] != 0) {
                        cpu->lo = (uint32_t)((int32_t)cpu->gpr[rs] / (int32_t)cpu->gpr[rt]);
                        cpu->hi = (uint32_t)((int32_t)cpu->gpr[rs] % (int32_t)cpu->gpr[rt]);
                    }
                    break;
                }
                case 0x1B: { // DIVU (Unsigned Divide)
                    if (cpu->gpr[rt] != 0) {
                        cpu->lo = cpu->gpr[rs] / cpu->gpr[rt];
                        cpu->hi = cpu->gpr[rs] % cpu->gpr[rt];
                    }
                    break;
                }
                case 0x20: // ADD
                case 0x21: // ADDU
                    if (rd != 0) cpu->gpr[rd] = cpu->gpr[rs] + cpu->gpr[rt];
                    break;
                case 0x22: // SUB
                case 0x23: // SUBU
                    if (rd != 0) cpu->gpr[rd] = cpu->gpr[rs] - cpu->gpr[rt];
                    break;
                // NOVO: DADD/DADDU/DSUB/DSUBU (funct 0x2C-0x2F) - variantes de
                // 64 bits do MIPS III. A Allegrex (PSP) nao tem datapath de
                // 64 bits de verdade, entao tratamos igual ao ADD/ADDU/SUB/SUBU
                // de 32 bits - mesma abordagem usada por outros interpretadores
                // PSP pra manter compatibilidade com codigo que usa esses opcodes
                // (visto em loops de descompressao/checksum, ex: leitura de
                // arquivos .csz). Antes eram silenciosamente ignorados, o que
                // travava contadores/ponteiros de loop em vez de avancarem.
                case 0x2C: // DADD
                case 0x2D: // DADDU
                    if (rd != 0) cpu->gpr[rd] = cpu->gpr[rs] + cpu->gpr[rt];
                    break;
                case 0x2E: // DSUB
                case 0x2F: // DSUBU
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
            } else if (rt == 2) { // BLTZL (Branch on Less Than Zero Likely)
                if ((int32_t)cpu->gpr[rs] < 0) {
                    branch_taken = true;
                    branch_target = pc + 4 + (simm * 4);
                } else {
                    cpu->pc = pc + 8;
                    cpu->next_pc = pc + 12;
                }
            } else if (rt == 3) { // BGEZL (Branch on Greater Than or Equal to Zero Likely)
                if ((int32_t)cpu->gpr[rs] >= 0) {
                    branch_taken = true;
                    branch_target = pc + 4 + (simm * 4);
                } else {
                    cpu->pc = pc + 8;
                    cpu->next_pc = pc + 12;
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
                case 0x22: { // LWL (Load Word Left)
            uint32_t addr = cpu->gpr[rs] + simm;
            uint32_t aligned_addr = addr & ~3u;
            uint32_t word = mips_read32(cpu, aligned_addr);
            int shift = (addr & 3) * 8;
            // Formula padrao MIPS (big-endian vs little-endian ja tratada por addr&3):
            if (rt != 0) cpu->gpr[rt] = (cpu->gpr[rt] & ~(0xFFFFFFFFu << shift)) | (word << shift);
            break;
        }
        case 0x26: { // LWR (Load Word Right)
            uint32_t addr = cpu->gpr[rs] + simm;
            uint32_t aligned_addr = addr & ~3u;
            uint32_t word = mips_read32(cpu, aligned_addr);
            int shift = (addr & 3) * 8;
            if (rt != 0) cpu->gpr[rt] = (cpu->gpr[rt] & ~(0xFFFFFFFFu >> (24 - shift))) | (word >> (24 - shift));
            break;
        }
        case 0x2A: { // SWL (Store Word Left) - complemento exato do LWL acima.
            // Deduzido como o inverso matematico exato do LWL ja existente
            // (nao copiei uma formula generica de outro emulador de proposito -
            // assim garanto que SWL seguido de LWL no mesmo endereco devolve
            // exatamente os bits que foram gravados, consistente com a
            // convencao que este interpretador ja usa).
            uint32_t addr = cpu->gpr[rs] + simm;
            uint32_t aligned_addr = addr & ~3u;
            uint32_t mem = mips_read32(cpu, aligned_addr);
            int shift = (addr & 3) * 8; // 0, 8, 16 ou 24 - nunca 32, sem UB nos shifts abaixo
            uint32_t keep_mask = ~(0xFFFFFFFFu >> shift);
            uint32_t new_mem = (mem & keep_mask) | (cpu->gpr[rt] >> shift);
            mips_write32(cpu, aligned_addr, new_mem);
            break;
        }
        case 0x2E: { // SWR (Store Word Right) - complemento exato do LWR acima.
            uint32_t addr = cpu->gpr[rs] + simm;
            uint32_t aligned_addr = addr & ~3u;
            uint32_t mem = mips_read32(cpu, aligned_addr);
            int shift = (addr & 3) * 8;
            uint32_t preserve_mask = ~(0xFFFFFFFFu << (24 - shift));
            uint32_t new_mem = (mem & preserve_mask) | (cpu->gpr[rt] << (24 - shift));
            mips_write32(cpu, aligned_addr, new_mem);
            break;
        }
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
                case 0x24: // MFIC - Move From Interrupt Controller (Allegrex)
                    if (rt != 0) cpu->gpr[rt] = cpu->ic_state;
                    break;
                case 0x26: // MTIC - Move To Interrupt Controller (Allegrex)
                    cpu->ic_state = cpu->gpr[rt];
                    break;
                default:
                    printf("[Interpreter Warning] Unsupported SPECIAL2 funct 0x%02X (inst=0x%08X, rs=%d, rt=%d, rd=%d) at PC 0x%08X\n", funct, inst, rs, rt, rd, pc);
                    break;
            }
            break;
        }
        case 0x14: { // BEQL (Branch on Equal Likely)
            if (cpu->gpr[rs] == cpu->gpr[rt]) {
                branch_taken = true;
                branch_target = pc + 4 + (simm * 4);
            } else {
                cpu->pc = pc + 8;
                cpu->next_pc = pc + 12;
            }
            break;
        }
        case 0x15: { // BNEL (Branch on Not Equal Likely)
            if (cpu->gpr[rs] != cpu->gpr[rt]) {
                branch_taken = true;
                branch_target = pc + 4 + (simm * 4);
            } else {
                cpu->pc = pc + 8;
                cpu->next_pc = pc + 12;
            }
            break;
        }
        case 0x16: { // BLEZL (Branch on Less Than or Equal to Zero Likely)
            if ((int32_t)cpu->gpr[rs] <= 0) {
                branch_taken = true;
                branch_target = pc + 4 + (simm * 4);
            } else {
                cpu->pc = pc + 8;
                cpu->next_pc = pc + 12;
            }
            break;
        }
        case 0x17: { // BGTZL (Branch on Greater Than Zero Likely)
            if ((int32_t)cpu->gpr[rs] > 0) {
                branch_taken = true;
                branch_target = pc + 4 + (simm * 4);
            } else {
                cpu->pc = pc + 8;
                cpu->next_pc = pc + 12;
            }
            break;
        }

        case 0x11: { // COP1 (FPU Single/Compare/Branch)
            uint32_t sub_op = rs;
            if (sub_op == 0x00) { // MFC1
                if (rt != 0) cpu->gpr[rt] = *(uint32_t*)&cpu->fpr[rd];
            } else if (sub_op == 0x04) { // MTC1
                *(uint32_t*)&cpu->fpr[rd] = cpu->gpr[rt];
            } else if (sub_op == 0x02) { // CFC1
                if (rt != 0) cpu->gpr[rt] = cpu->fcr31;
            } else if (sub_op == 0x06) { // CTC1
                cpu->fcr31 = cpu->gpr[rt];
            } else if (sub_op == 0x08) { // BC1 branches
                uint32_t cc = (rt >> 2) & 7;
                uint32_t bit = (cc == 0) ? 23 : (24 + cc);
                bool cond = (cpu->fcr31 & (1 << bit)) != 0;
                uint32_t branch_type = rt & 3;
                if (branch_type == 0) { // BC1F
                    if (!cond) {
                        branch_taken = true;
                        branch_target = pc + 4 + (simm * 4);
                    }
                } else if (branch_type == 1) { // BC1T
                    if (cond) {
                        branch_taken = true;
                        branch_target = pc + 4 + (simm * 4);
                    }
                } else if (branch_type == 2) { // BC1FL
                    if (!cond) {
                        branch_taken = true;
                        branch_target = pc + 4 + (simm * 4);
                    } else {
                        cpu->pc = pc + 8;
                        cpu->next_pc = pc + 12;
                    }
                } else if (branch_type == 3) { // BC1TL
                    if (cond) {
                        branch_taken = true;
                        branch_target = pc + 4 + (simm * 4);
                    } else {
                        cpu->pc = pc + 8;
                        cpu->next_pc = pc + 12;
                    }
                }
            } else if (sub_op == 0x10) { // Format Single (.s)
                float fs_val = cpu->fpr[rd];
                float ft_val = cpu->fpr[rt];
                uint32_t fd = shamt;
                switch (funct) {
                    case 0x00: // ADD.S
                        cpu->fpr[fd] = fs_val + ft_val;
                        break;
                    case 0x01: // SUB.S
                        cpu->fpr[fd] = fs_val - ft_val;
                        break;
                    case 0x02: // MUL.S
                        cpu->fpr[fd] = fs_val * ft_val;
                        break;
                    case 0x03: // DIV.S
                        if (ft_val != 0.0f) cpu->fpr[fd] = fs_val / ft_val;
                        break;
                    case 0x04: // SQRT.S
                        cpu->fpr[fd] = sqrtf(fs_val);
                        break;
                    case 0x05: // ABS.S
                        cpu->fpr[fd] = fabsf(fs_val);
                        break;
                    case 0x06: // MOV.S
                        cpu->fpr[fd] = fs_val;
                        break;
                    case 0x07: // NEG.S
                        cpu->fpr[fd] = -fs_val;
                        break;
                    case 0x0C: // ROUND.W.S
                        *(int32_t*)&cpu->fpr[fd] = (int32_t)roundf(fs_val);
                        break;
                    case 0x0D: // TRUNC.W.S
                        *(int32_t*)&cpu->fpr[fd] = (int32_t)fs_val;
                        break;
                    case 0x0E: // CEIL.W.S
                        *(int32_t*)&cpu->fpr[fd] = (int32_t)ceilf(fs_val);
                        break;
                    case 0x0F: // FLOOR.W.S
                        *(int32_t*)&cpu->fpr[fd] = (int32_t)floorf(fs_val);
                        break;
                    case 0x20: // CVT.S.W (colocado aqui por engano - fmt=S+funct=0x20
                               // nao e uma instrucao real que compilador nenhum gera;
                               // o CVT.S.W de verdade agora e tratado em fmt=W/sub_op 0x14
                               // logo abaixo. Deixado aqui sem alteracao de comportamento
                               // pra nao mudar nada que ja funcionava.
                        cpu->fpr[fd] = (float)*(int32_t*)&cpu->fpr[rd];
                        break;
                    case 0x24: // CVT.W.S
                        *(int32_t*)&cpu->fpr[fd] = (int32_t)fs_val;
                        break;
                    default:
                        if (funct >= 0x30 && funct <= 0x3F) { // c.cond.s comparisons
                            bool cmp_cond = false;
                            switch (funct & 0xF) {
                                case 0x0: cmp_cond = false; break; // c.f.s
                                case 0x2: cmp_cond = (fs_val == ft_val); break; // c.eq.s
                                case 0x4: cmp_cond = (fs_val < ft_val); break; // c.lt.s (c.olt.s)
                                case 0x6: cmp_cond = (fs_val <= ft_val); break; // c.le.s (c.ole.s)
                                case 0xC: cmp_cond = (fs_val < ft_val); break; // c.lt.s (c.ult.s)
                                case 0xE: cmp_cond = (fs_val <= ft_val); break; // c.le.s (c.ule.s)
                                default: cmp_cond = (fs_val == ft_val); break;
                            }
                            uint32_t cc = (inst >> 8) & 7;
                            uint32_t bit = (cc == 0) ? 23 : (24 + cc);
                            cpu->fcr31 = (cpu->fcr31 & ~(1 << bit)) | (cmp_cond ? (1 << bit) : 0);
                        } else {
                            printf("[Interpreter Warning] Unsupported COP1 Single funct 0x%02X at PC 0x%08X\n", funct, pc);
                        }
                        break;
                }
            } else if (sub_op == 0x14) { // Format Word (.w) - operando fonte
                // e' um inteiro de 32 bits guardado nos bits de fpr[rd] (nao
                // um float). Antes essa combinacao (fmt=W) nao tinha
                // tratamento nenhum e caia no 'else' generico de baixo -
                // que e' exatamente o aviso "Unsupported COP1 sub_op 0x14"
                // que aparecia nos logs. O CVT.S.W (converter inteiro pra
                // float) que existia antes estava colocado por engano
                // dentro do bloco fmt=S (sub_op 0x10), onde a instrucao
                // correspondente nunca e' realmente gerada por compilador
                // nenhum (fmt=S + funct 0x20 seria "CVT.S.S", que nao
                // existe) - o real CVT.S.W sempre vem com fmt=W, aqui.
                int32_t fs_int = *(int32_t*)&cpu->fpr[rd];
                uint32_t fd = shamt;
                switch (funct) {
                    case 0x20: // CVT.S.W - converte o inteiro fs_int para float, guarda em fd
                        cpu->fpr[fd] = (float)fs_int;
                        break;
                    default:
                        printf("[Interpreter Warning] Unsupported COP1 Word funct 0x%02X at PC 0x%08X\n", funct, pc);
                        break;
                }
            } else {
                printf("[Interpreter Warning] Unsupported COP1 sub_op 0x%02X at PC 0x%08X\n", sub_op, pc);
            }
            break;
        }
                case 0x12: { // COP2 (VFPU register moves)
            if (rs == 0x00) { // MFC2
                if (rt != 0) cpu->gpr[rt] = *(uint32_t*)&cpu->vfpu[rd];
            } else if (rs == 0x04) { // MTC2
                *(uint32_t*)&cpu->vfpu[rd] = cpu->gpr[rt];
            } else if (rs == 0x02) { // CFC2
                if (rt != 0) {
                    if (rd < 4) cpu->gpr[rt] = cpu->vcr[rd];
                    else cpu->gpr[rt] = 0;
                }
            } else if (rs == 0x06) { // CTC2
                if (rd < 4) cpu->vcr[rd] = cpu->gpr[rt];
            }
            break;
        }
        case 0x18: { // VFPU vadd / vdiv / or other operations
            uint32_t vd = inst & 0x7F;
            uint32_t vs = (inst >> 8) & 0x7F;
            uint32_t vt = (inst >> 16) & 0x7F;
            uint32_t format = (inst >> 23) & 0x7;
            if (format == 0) { // vadd
                for (int i = 0; i < 4; i++) {
                    if (vd+i < 128 && vs+i < 128 && vt+i < 128) {
                        cpu->vfpu[vd + i] = cpu->vfpu[vs + i] + cpu->vfpu[vt + i];
                    }
                }
            } else if (format == 7) { // vdiv
                for (int i = 0; i < 4; i++) {
                    if (vd+i < 128 && vs+i < 128 && vt+i < 128 && cpu->vfpu[vt + i] != 0.0f) {
                        cpu->vfpu[vd + i] = cpu->vfpu[vs + i] / cpu->vfpu[vt + i];
                    }
                }
            }
            break;
        }
        case 0x19: { // VFPU vmul / vdot / vhdp
            uint32_t vd = inst & 0x7F;
            uint32_t vs = (inst >> 8) & 0x7F;
            uint32_t vt = (inst >> 16) & 0x7F;
            uint32_t format = (inst >> 23) & 0x7;
            if (format == 0) { // vmul.q
                for (int i = 0; i < 4; i++) {
                    if (vd+i < 128 && vs+i < 128 && vt+i < 128) {
                        cpu->vfpu[vd + i] = cpu->vfpu[vs + i] * cpu->vfpu[vt + i];
                    }
                }
            } else if (format == 1) { // vdot.q (Dot Product)
                float sum = 0.0f;
                for (int i = 0; i < 4; i++) {
                    if (vs+i < 128 && vt+i < 128) {
                        sum += cpu->vfpu[vs + i] * cpu->vfpu[vt + i];
                    }
                }
                if (vd < 128) {
                    cpu->vfpu[vd] = sum;
                }
            } else if (format == 4) { // vhdp.q (Homogeneous Dot Product)
                float sum = 0.0f;
                for (int i = 0; i < 4; i++) {
                    if (vs+i < 128) {
                        float vt_val = (i == 3) ? 1.0f : ((vt+i < 128) ? cpu->vfpu[vt + i] : 0.0f);
                        sum += cpu->vfpu[vs + i] * vt_val;
                    }
                }
                if (vd < 128) {
                    cpu->vfpu[vd] = sum;
                }
            }
            break;
        }
        case 0x1A: { // VFPU vsub
            uint32_t vd = inst & 0x7F;
            uint32_t vs = (inst >> 8) & 0x7F;
            uint32_t vt = (inst >> 16) & 0x7F;
            uint32_t format = (inst >> 23) & 0x7;
            if (format == 0) { // vsub
                for (int i = 0; i < 4; i++) {
                    if (vd+i < 128 && vs+i < 128 && vt+i < 128) {
                        cpu->vfpu[vd + i] = cpu->vfpu[vs + i] - cpu->vfpu[vt + i];
                    }
                }
            }
            break;
        }
        case 0x31: { // LWC1
            *(uint32_t*)&cpu->fpr[rt] = mips_read32(cpu, cpu->gpr[rs] + simm);
            break;
        }
        case 0x39: { // SWC1
            mips_write32(cpu, cpu->gpr[rs] + simm, *(uint32_t*)&cpu->fpr[rt]);
            break;
        }
        case 0x32: { // LWC2 (Load Word Coprocessor 2 - Scalar)
            uint32_t addr = cpu->gpr[rs] + simm;
            if (rt < 128) {
                *(uint32_t*)&cpu->vfpu[rt] = mips_read32(cpu, addr);
            }
            break;
        }
        case 0x36: { // LV.Q Allegrex official Vector Load
            uint32_t addr = cpu->gpr[rs] + simm;
            if (rt + 3 < 128) {
                *(uint32_t*)&cpu->vfpu[rt + 0] = mips_read32(cpu, addr + 0);
                *(uint32_t*)&cpu->vfpu[rt + 1] = mips_read32(cpu, addr + 4);
                *(uint32_t*)&cpu->vfpu[rt + 2] = mips_read32(cpu, addr + 8);
                *(uint32_t*)&cpu->vfpu[rt + 3] = mips_read32(cpu, addr + 12);
            }
            break;
        }
        case 0x3A: { // SWC2 (Store Word Coprocessor 2 - Scalar)
            uint32_t addr = cpu->gpr[rs] + simm;
            if (rt < 128) {
                mips_write32(cpu, addr, *(uint32_t*)&cpu->vfpu[rt]);
            }
            break;
        }
        case 0x3C: { // COP2 Matrix calculations (vmmul, vtfm, vhtfm, vmidt, vmzero)
            uint32_t vd = inst & 0x7F;
            uint32_t vs = (inst >> 8) & 0x7F;
            uint32_t vt = (inst >> 16) & 0x7F;
            uint32_t format = (inst >> 23) & 0x7;

            if (format == 0) { // vmmul (Matrix Multiplication - Pair, Triple, Quad)
                int dim = 2;
                uint32_t bit15 = (inst >> 15) & 1;
                uint32_t bit7 = (inst >> 7) & 1;
                if (bit15 == 0) dim = 2;
                else if (bit7 == 0) dim = 3;
                else dim = 4;

                float temp[16] = {0};
                for (int r = 0; r < dim; r++) {
                    for (int c = 0; c < dim; c++) {
                        float sum = 0.0f;
                        for (int k = 0; k < dim; k++) {
                            // VFPU matrices are column-major in storage
                            float val_s = cpu->vfpu[vs + k * 4 + r];
                            float val_t = cpu->vfpu[vt + c * 4 + k];
                            sum += val_s * val_t;
                        }
                        temp[c * 4 + r] = sum;
                    }
                }
                for (int i = 0; i < dim * 4; i++) {
                    if (vd + i < 128) cpu->vfpu[vd + i] = temp[i];
                }
            } else if (format >= 1 && format <= 3) { // vtfm / vhtfm (Vector-Matrix transforms - Pair, Triple, Quad)
                int dim = format + 1; // format 1 -> 2, format 2 -> 3, format 3 -> 4
                uint32_t is_vtfm = (inst >> 7) & 1;

                float temp_v[4] = {0};
                for (int i = 0; i < dim; i++) {
                    float sum = 0.0f;
                    for (int j = 0; j < dim; j++) {
                        float m_val = cpu->vfpu[vs + j * 4 + i];
                        float v_val = (j == (dim - 1) && !is_vtfm) ? 1.0f : cpu->vfpu[vt + j];
                        sum += m_val * v_val;
                    }
                    temp_v[i] = sum;
                }
                for (int i = 0; i < dim; i++) {
                    if (vd + i < 128) cpu->vfpu[vd + i] = temp_v[i];
                }
            } else if (format == 7) { // vmidt / vmzero (Identity / Zero Matrices - Pair, Triple, Quad)
                uint32_t op_type = (inst >> 16) & 0x7F;
                int dim = 2;
                uint32_t bit15 = (inst >> 15) & 1;
                uint32_t bit7 = (inst >> 7) & 1;
                if (bit15 == 0) dim = 2;
                else if (bit7 == 0) dim = 3;
                else dim = 4;

                if (op_type == 3) { // vmidt
                    for (int c = 0; c < dim; c++) {
                        for (int r = 0; r < dim; r++) {
                            if (vd + c * 4 + r < 128) {
                                cpu->vfpu[vd + c * 4 + r] = (r == c) ? 1.0f : 0.0f;
                            }
                        }
                    }
                } else if (op_type == 6) { // vmzero
                    for (int c = 0; c < dim; c++) {
                        for (int r = 0; r < dim; r++) {
                            if (vd + c * 4 + r < 128) {
                                cpu->vfpu[vd + c * 4 + r] = 0.0f;
                            }
                        }
                    }
                }
            }
            break;
        }
        case 0x3E: { // SV.Q Allegrex official Vector Store
            uint32_t addr = cpu->gpr[rs] + simm;
            if (rt + 3 < 128) {
                mips_write32(cpu, addr + 0, *(uint32_t*)&cpu->vfpu[rt + 0]);
                mips_write32(cpu, addr + 4, *(uint32_t*)&cpu->vfpu[rt + 1]);
                mips_write32(cpu, addr + 8, *(uint32_t*)&cpu->vfpu[rt + 2]);
                mips_write32(cpu, addr + 12, *(uint32_t*)&cpu->vfpu[rt + 3]);
            }
            break;
        }
        case 0x34: { // VFPU monadic / conversion operations (vabs, vneg, vzero, vone, vrcp, vrsq, vsin, vcos, vsqrt, vf2in, vi2f, vi2uc, vi2s)
            uint32_t vd = inst & 0x7F;
            uint32_t vs = (inst >> 8) & 0x7F;
            uint32_t bit7 = (inst >> 7) & 1;
            uint32_t bit15 = (inst >> 15) & 1;
            uint32_t dim = 1 + bit7 + bit15 * 2;
            
            if (rs == 0x10) { // vf2in (float to int)
                for (int i = 0; i < dim; i++) {
                    if (vd+i < 128 && vs+i < 128) {
                        float val = cpu->vfpu[vs + i];
                        *(int32_t*)&cpu->vfpu[vd + i] = (int32_t)roundf(val);
                    }
                }
            } else if (rs == 0x14) { // vi2f (int to float)
                for (int i = 0; i < dim; i++) {
                    if (vd+i < 128 && vs+i < 128) {
                        int32_t val = *(int32_t*)&cpu->vfpu[vs + i];
                        cpu->vfpu[vd + i] = (float)val;
                    }
                }
            } else if (rs == 0x01 && rt == 0x1C) { // vi2uc (int to unsigned char)
                for (int i = 0; i < dim; i++) {
                    if (vd+i < 128 && vs+i < 128) {
                        int32_t val = *(int32_t*)&cpu->vfpu[vs + i];
                        if (val < 0) val = 0;
                        if (val > 255) val = 255;
                        *(int32_t*)&cpu->vfpu[vd + i] = val;
                    }
                }
            } else if (rs == 0x01 && rt == 0x1F) { // vi2s (int to short)
                for (int i = 0; i < dim; i++) {
                    if (vd+i < 128 && vs+i < 128) {
                        int32_t val = *(int32_t*)&cpu->vfpu[vs + i];
                        if (val < -32768) val = -32768;
                        if (val > 32767) val = 32767;
                        *(int32_t*)&cpu->vfpu[vd + i] = val;
                    }
                }
            } else if (rs == 0x00) { // Unary operations (vabs, vneg, vzero, vone, vrcp, vrsq, vsin, vcos, vsqrt)
                switch (rt) {
                    case 1: // vabs
                        for (int i = 0; i < dim; i++) {
                            if (vd+i < 128 && vs+i < 128) cpu->vfpu[vd + i] = fabsf(cpu->vfpu[vs + i]);
                        }
                        break;
                    case 2: // vneg
                        for (int i = 0; i < dim; i++) {
                            if (vd+i < 128 && vs+i < 128) cpu->vfpu[vd + i] = -cpu->vfpu[vs + i];
                        }
                        break;
                    case 6: // vzero
                        for (int i = 0; i < dim; i++) {
                            if (vd+i < 128) cpu->vfpu[vd + i] = 0.0f;
                        }
                        break;
                    case 7: // vone
                        for (int i = 0; i < dim; i++) {
                            if (vd+i < 128) cpu->vfpu[vd + i] = 1.0f;
                        }
                        break;
                    case 16: // vrcp (reciprocal)
                        for (int i = 0; i < dim; i++) {
                            if (vd+i < 128 && vs+i < 128) {
                                float val = cpu->vfpu[vs + i];
                                cpu->vfpu[vd + i] = (val != 0.0f) ? (1.0f / val) : 0.0f;
                            }
                        }
                        break;
                    case 17: // vrsq (reciprocal square root)
                        for (int i = 0; i < dim; i++) {
                            if (vd+i < 128 && vs+i < 128) {
                                float val = cpu->vfpu[vs + i];
                                cpu->vfpu[vd + i] = (val > 0.0f) ? (1.0f / sqrtf(val)) : 0.0f;
                            }
                        }
                        break;
                    case 18: // vsin
                        for (int i = 0; i < dim; i++) {
                            if (vd+i < 128 && vs+i < 128) {
                                cpu->vfpu[vd + i] = sinf(cpu->vfpu[vs + i] * 1.57079632679f);
                            }
                        }
                        break;
                    case 19: // vcos
                        for (int i = 0; i < dim; i++) {
                            if (vd+i < 128 && vs+i < 128) {
                                cpu->vfpu[vd + i] = cosf(cpu->vfpu[vs + i] * 1.57079632679f);
                            }
                        }
                        break;
                    case 22: // vsqrt
                        for (int i = 0; i < dim; i++) {
                            if (vd+i < 128 && vs+i < 128) {
                                float val = cpu->vfpu[vs + i];
                                cpu->vfpu[vd + i] = (val >= 0.0f) ? sqrtf(val) : 0.0f;
                            }
                        }
                        break;
                    default:
                        printf("[Interpreter Warning] Unsupported COP2 monadic rt 0x%02X at PC 0x%08X\n", rt, pc);
                        break;
                }
            }
            break;
        }
                case 0x1F: { // SPECIAL3
            switch (funct) {
                case 0x00: { // EXT rt, rs, pos, size
                    uint32_t pos = shamt;
                    uint32_t size = rd + 1;
                    uint32_t mask = (size < 32) ? ((1u << size) - 1) : 0xFFFFFFFFu;
                    if (rt != 0) cpu->gpr[rt] = (cpu->gpr[rs] >> pos) & mask;
                    break;
                }
                case 0x04: { // INS rt, rs, pos, size  (msb = rd, campo vai de pos ate msb)
                    uint32_t pos = shamt;
                    uint32_t msb = rd;
                    uint32_t size = msb - pos + 1;
                    uint32_t mask = (size < 32) ? ((1u << size) - 1) : 0xFFFFFFFFu;
                    if (rt != 0) {
                        uint32_t inserted = (cpu->gpr[rs] & mask) << pos;
                        cpu->gpr[rt] = (cpu->gpr[rt] & ~(mask << pos)) | inserted;
                    }
                    break;
                }
                case 0x20: { // BSHFL (sub-opcode selecionado por shamt)
                    switch (shamt) {
                        case 0x02: // WSBH - word swap bytes within halfwords
                            if (rd != 0) {
                                uint32_t v = cpu->gpr[rt];
                                cpu->gpr[rd] = ((v & 0xFF00FF00u) >> 8) | ((v & 0x00FF00FFu) << 8);
                            }
                            break;
                        case 0x10: // SEB - sign-extend byte
                            if (rd != 0) cpu->gpr[rd] = (int32_t)(int8_t)(cpu->gpr[rt] & 0xFF);
                            break;
                        case 0x18: // SEH - sign-extend halfword
                            if (rd != 0) cpu->gpr[rd] = (int32_t)(int16_t)(cpu->gpr[rt] & 0xFFFF);
                            break;
                        default:
                            printf("[Interpreter Warning] Unsupported SPECIAL3 BSHFL shamt 0x%02X at PC 0x%08X\n", shamt, pc);
                            break;
                    }
                    break;
                }
                default:
                    printf("[Interpreter Warning] Unsupported SPECIAL3 funct 0x%02X (shamt=0x%02X) at PC 0x%08X\n", funct, shamt, pc);
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
