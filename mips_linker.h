#ifndef MIPS_LINKER_V5_H
#define MIPS_LINKER_V5_H

#include "mips_reloc.h"
#include <stdint.h>

extern PatchedFallbackStub fallback_stubs[MAX_PATCHED_FALLBACKS];
extern int fallback_stubs_count;
extern uint32_t relocated_gp;

int load_eboot(MIPS_CPU *cpu, const char *filepath, uint32_t *entry_point);

#endif // MIPS_LINKER_V5_H
