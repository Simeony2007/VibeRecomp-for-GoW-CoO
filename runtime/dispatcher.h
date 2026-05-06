#ifndef DISPATCHER_H
#define DISPATCHER_H

#include "cpu.h"
#include <stdint.h>

void dispatcher(MIPS_CPU *cpu, uint8_t *mem, uint32_t target);

#endif