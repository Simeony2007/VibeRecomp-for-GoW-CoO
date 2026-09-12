#ifndef MIPS_RELOC_V3_H
#define MIPS_RELOC_V3_H

#include "mips_cpu.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef MAX_PENDING_HI16
#define MAX_PENDING_HI16 64
#endif

#ifndef MAX_PATCHED_FALLBACKS
#define MAX_PATCHED_FALLBACKS 256
#endif

// Guard against duplicate PSP module types across translation units
#ifndef PSP_MODULE_TYPES_DEFINED
#define PSP_MODULE_TYPES_DEFINED

typedef struct {
    uint32_t stub_addr;
    uint32_t nid;
    uint32_t target_vaddr;
    char     lib_name[32];
    uint32_t syscall_code;
} PatchedFallbackStub;

// PSP SceModuleInfo structure - strict 52-byte layout matching PSP OS
typedef struct {
    uint16_t modAttribute;
    uint8_t  modVersion[2];
    char     modName[27];
    char     terminal;
    uint32_t gp_value;
    uint32_t ent_top;
    uint32_t ent_btm;
    uint32_t stub_top;
    uint32_t stub_btm;
} __attribute__((packed)) SceModuleInfo;

typedef struct {
    uint32_t lib_name_ptr;
    uint16_t version;
    uint16_t attribute;
    uint8_t  struct_size;
    uint8_t  num_variables;
    uint16_t num_funcs;
    uint32_t nid_table_ptr;
    uint32_t stub_table_ptr;
} __attribute__((packed)) SceLibraryStubHeader;

#endif // PSP_MODULE_TYPES_DEFINED

// Guard against duplicate ELF32 types across translation units
#ifndef ELF32_TYPES_DEFINED
#define ELF32_TYPES_DEFINED

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} Elf32_Shdr;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
} Elf32_Rel;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
    int32_t  r_addend;
} Elf32_Rela;

typedef struct {
    uint32_t ri_gprmask;
    uint32_t ri_cprmask[4];
    uint32_t ri_gp_value;
} Elf32_RegInfo;

#endif // ELF32_TYPES_DEFINED

// Function prototypes
bool safe_read_cstring(MIPS_CPU *cpu, uint32_t vaddr, char *out, size_t out_size);
int mips_apply_relocations(MIPS_CPU *cpu, uint8_t *elf_data, uint32_t load_bias, uint32_t *entry_point);

#endif // MIPS_RELOC_V3_H
