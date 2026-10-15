#ifndef MIPS_INTERPRETER_H
#define MIPS_INTERPRETER_H

#include "mips_cpu.h"

// Decodes and executes a single instruction at the current PC
void mips_interpreter_step(MIPS_CPU *cpu);

#endif // MIPS_INTERPRETER_H
