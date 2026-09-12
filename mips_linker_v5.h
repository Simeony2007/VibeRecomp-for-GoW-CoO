#ifndef MIPS_LINKER_V5_H
#define MIPS_LINKER_V5_H

#include "mips_cpu.h"
#include <stdint.h>

#define MAX_PATCHED_FALLBACKS 1024

typedef struct {
    char lib_name[32];
    uint32_t nid;
    uint32_t syscall_code;
} PatchedFallbackStub;

extern PatchedFallbackStub fallback_stubs[MAX_PATCHED_FALLBACKS];
extern int fallback_stubs_count;
extern uint32_t relocated_gp;

// PSP Module Info Structure (SceModuleInfo)
typedef struct {
    uint16_t modAttribute;  // Offset 0: Attributes (0x0000 = User, 0x1000 = Kernel)
    uint16_t modVersion;    // Offset 2: Version (Major/Minor bytes)
    char     modName[27];   // Offset 4: Module Name (e.g., "God of War")
    uint8_t  terminal;      // Offset 31: Null-terminator or padding
    uint32_t gp_value;      // Offset 32: Global Pointer ($gp) value
    uint32_t ent_top;       // Offset 36: Export table start (usually 0 for games)
    uint32_t ent_btm;       // Offset 40: Export table end (usually 0)
    uint32_t stub_top;      // Offset 44: Import table start (highly critical!)
    uint32_t stub_btm;      // Offset 48: Import table end
} __attribute__ ((packed)) SceModuleInfo;

// PSP Library Stub Header Structure (SceLibraryStubHeader)
typedef struct {
    uint32_t lib_name_ptr;    // Offset 0: Pointer to library name string
    uint16_t version;         // Offset 4: Version
    uint16_t attribute;       // Offset 6: Attributes
    uint8_t  struct_size;     // Offset 8: Struct size in 32-bit words (usually 5)
    uint8_t  num_vars;        // Offset 9: Number of variables (usually 0)
    uint16_t num_funcs;       // Offset 10: Real count of imported functions/stubs
    uint32_t nid_table_ptr;   // Offset 12: Pointer to NID (hash) table
    uint32_t stub_table_ptr;  // Offset 16: Pointer to stub (.sceStub.text) table
} __attribute__ ((packed)) SceLibraryStubHeader;

int load_eboot(MIPS_CPU *cpu, const char *filepath, uint32_t *entry_point);

#endif // MIPS_LINKER_V5_H
