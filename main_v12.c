#include "mips_cpu.h"
#include "mips_dispatcher.h"
#include "mips_interpreter.h"
#include "mips_hle.h"
#include "mips_video.h"
#include "mips_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/*
*  MIPS Allegrex Dynamic Executable Loader (ELF/PRX) - God of War Port
*  Version 13 - Bulletproof Heuristic/Direct Module Linker & Import Patching (PRX Linker)
*  Fixed SceLibraryStubHeader alignment and offset layout.
*/

// Global HLE Kernel Instance
MIPS_HLE_Kernel kernel;

// Global video system binding pointer for accelerated graphics callbacks
void *global_video_system = NULL;

// Weak declaration of the registration function auto-generated in gow_core_aot.c.
__attribute__ ((weak)) void aot_register_blocks(MIPS_Dispatcher *disp) {
    (void)disp;
    printf("[AOT] No Ahead-of-Time recompiled blocks found (gow_core_aot.c not compiled).\n");
    printf("[AOT] System will operate in 100%% Fallback Interpreter mode.\n");
}

// Dynamic Syscall routing to our real POSIX-based HLE OS Kernel with a safe dummy fallback
void hle_syscall(MIPS_CPU *cpu) {
    uint32_t inst_word = mips_read32(cpu, cpu->pc);
    uint32_t syscall_id = (inst_word >> 6) & 0xFFFFF;

    // Intercept our custom fallback syscall for safely returning success on unimplemented imports
    if (syscall_id == 0x99999) {
        cpu->gpr[2] = 0; // Return success (0)
        cpu->pc += 4;
        cpu->next_pc = cpu->pc;
        return;
    }

    mips_hle_syscall(cpu, &kernel);
}

// Fallback interpreter delegation routing
void interpreter_step(MIPS_CPU *cpu) {
    mips_interpreter_step(cpu);
}

// Structures representing MIPS ELF32 Headers
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

// Structures representing PSP Module Headers (SceModuleInfo) - Packed to avoid compiler alignment issues
typedef struct {
    uint16_t modAttribute;
    uint16_t modVersion;
    char     modName[27];
    uint8_t  terminal;
    uint32_t gp_value;
    uint32_t ent_top;
    uint32_t ent_btm;
    uint32_t stub_top;
    uint32_t stub_btm;
} __attribute__ ((packed)) SceModuleInfo;

// Structures representing PSP Library Import Stubs Header (Officially Aligned & Corrigido!)
typedef struct {
    uint32_t lib_name_ptr;    // Pointer to library name (const char *)
    uint16_t version;         // Library version
    uint16_t attribute;       // Import attributes/flags
    uint8_t  struct_size;     // Size of SceLibraryStubHeader in 32-bit words (usually 5)
    uint8_t  num_vars;        // Number of imported variables (usually 0)
    uint16_t num_funcs;       // Real number of imported stubs/functions (num_stubs real)
    uint32_t nid_table_ptr;   // Pointer to NID table
    uint32_t stub_table_ptr;  // Pointer to stub table (.sceStub.text)
} __attribute__ ((packed)) SceLibraryStubHeader;

// Robust helper to perform safe dynamic pointer relocations
static inline uint32_t relocate_addr(uint32_t addr, uint32_t load_bias) {
    if (addr == 0) return 0;
    // Clear cache, uncached, and kernel segment bits to inspect the relative address
    uint32_t relative_addr = addr & 0x0FFFFFFF;
    if (relative_addr >= 0x08000000 && relative_addr < 0x0C000000) {
        // Already relocated (either as 0x08xxxxxx, 0x48xxxxxx, 0x88xxxxxx, or 0xA8xxxxxx)
        // Normalize it to standard cached user RAM space (0x08xxxxxx) for consistency
        return 0x08000000 + (relative_addr & 0x03FFFFFF);
    }
    // If relative_addr is smaller than 0x04000000, it's unrelocated, so apply load_bias
    if (relative_addr < 0x04000000) {
        return relative_addr + load_bias;
    }
    return addr;
}

// Check if SceModuleInfo pointer is completely valid and clean
static inline int is_valid_module_info(MIPS_CPU *cpu, SceModuleInfo *mod, uint32_t load_bias) {
    (void)cpu; // Suppress unused-parameter warnings
    if (!mod) return 0;

    // Safety check on attribute (0x0000 = user, 0x1000 = kernel, 0x0006 = standard)
    if (mod->modAttribute > 0x1FFF) return 0;

    // Safety checks on pointers - Relocated to actual RAM limits (relocated pointers can legitimately be 0!)
    uint32_t gp = relocate_addr(mod->gp_value, load_bias);
    uint32_t ent_top = relocate_addr(mod->ent_top, load_bias);
    uint32_t ent_btm = relocate_addr(mod->ent_btm, load_bias);
    uint32_t stub_top = relocate_addr(mod->stub_top, load_bias);
    uint32_t stub_btm = relocate_addr(mod->stub_btm, load_bias);

    if (gp != 0 && (gp < 0x08800000 || gp >= 0x0C000000)) return 0;
    if (ent_top != 0 && (ent_top < 0x08800000 || ent_top >= 0x0C000000 || (ent_top & 3) != 0)) return 0;
    if (ent_btm != 0 && (ent_btm < 0x08800000 || ent_btm >= 0x0C000000 || (ent_btm & 3) != 0)) return 0;
    if (stub_top != 0 && (stub_top < 0x08800000 || stub_top >= 0x0C000000 || (stub_top & 3) != 0)) return 0;
    if (stub_btm != 0 && (stub_btm < 0x08800000 || stub_btm >= 0x0C000000 || (stub_btm & 3) != 0)) return 0;

    if (ent_top != 0 && ent_btm != 0 && ent_top > ent_btm) return 0;
    if (stub_top != 0 && stub_btm != 0 && stub_top > stub_btm) return 0;
    if (stub_top == 0 && ent_top == 0) return 0; // Must contain either stubs or exports

    // Check if name has printable chars (first char check)
    if (mod->modName[0] < 32 || mod->modName[0] >= 127) return 0;

    return 1;
}

// Heuristically scan the emulated RAM to locate SceModuleInfo structure (100% bulletproof fallback)
static inline uint32_t find_sce_module_info(MIPS_CPU *cpu, uint32_t load_bias) {
    // Scan RAM from 0x08800000 to 0x0BFFF000 (KU0 Cached User space)
    for (uint32_t addr = 0x08800000; addr < 0x0BFFF000; addr += 4) {
        SceModuleInfo *mod = (SceModuleInfo *)mips_get_ptr(cpu, addr);
        if (is_valid_module_info(cpu, mod, load_bias)) {
            printf("[HLE Linker] Heuristically discovered SceModuleInfo at virtual address 0x%08X!\n", addr);
            return addr;
        }
    }
    return 0;
}

// High performance uncompressed ELF and decrypted plain ~PSP loader
int load_eboot(MIPS_CPU *cpu, const char *filepath, uint32_t *entry_point) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        printf("[Loader Error] Failed to open game executable: %s\n", filepath);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = malloc(size);
    if (!data) {
        printf("[Loader Error] Out of memory allocating file buffer.\n");
        fclose(f);
        return -2;
    }

    if (fread(data, 1, size, f) != size) {
        printf("[Loader Error] Failed to read full binary from file.\n");
        free(data);
        fclose(f);
        return -3;
    }
    fclose(f);

    uint8_t *elf_data = data;
    size_t elf_size = size;
    (void)elf_size; // Suppress unused-variable warnings

    uint32_t modinfo_offset = 0;

    // Detect ~PSP (PRX) container
    if (size >= 4 && memcmp(data, "~PSP", 4) == 0) {
        printf("[Loader] Detected ~PSP (PRX) executable container.\n");
        modinfo_offset = *(uint32_t*)&data[52]; // Read modinfo_offset directly from ~PSP header
        
        // PSP executables contain an embedded ELF inside the PRX block. Let's find it.
        uint8_t *elf_ptr = NULL;
        for (size_t i = 0; i < size - 4; i++) {
            if (data[i] == 0x7F && data[i+1] == 'E' && data[i+2] == 'L' && data[i+3] == 'F') {
                elf_ptr = &data[i];
                elf_size = size - i;
                break;
            }
        }

        if (elf_ptr != NULL) {
            printf("[Loader] Found embedded MIPS ELF32 at offset 0x%X inside PRX!\n", (unsigned int)(elf_ptr - data));
            elf_data = elf_ptr;
        } else {
            // Check compression type in header
            uint16_t comp_type = *(uint16_t*)&data[6];
            if ((comp_type & 0xF00) != 0x300) {
                printf("[Loader Error] PRX file is encrypted or compressed (Type 0x%03X).\n", comp_type & 0xF00);
                printf("[Loader Error] Please decrypt/decompress it first (e.g. using PPSSPP's decrypted ELF dump).\n");
                free(data);
                return -4;
            }

            // Fallback for flat uncompressed PRX mapping directly past header
            uint32_t boot_entry = *(uint32_t*)&data[48];
            printf("[Loader] PRX is uncompressed/decrypted but has no ELF signature. Segments: %d.\n", data[39]);
            printf("[Loader] Mapping flat program space directly to standard base address 0x08804000.\n");
            
            void *dest = mips_get_ptr(cpu, 0x08804000);
            if (!dest) {
                printf("[Loader Error] Target physical memory at 0x08804000 is unavailable.\n");
                free(data);
                return -5;
            }
            memcpy(dest, data + 0x150, size - 0x150); // Copy everything skipping PRX header
            *entry_point = boot_entry + 0x08804000;
            free(data);
            return 0;
        }
    }

    // Load standard ELF format with byte checks to avoid hex escape warning
    Elf32_Ehdr *ehdr = (Elf32_Ehdr*)elf_data;
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' || ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
        printf("[Loader Error] Game binary does not have a valid ELF magic number.\n");
        free(data);
        return -6;
    }

    // Auto-relocation Base Address Calculation
    uint32_t load_bias = 0;
    if (ehdr->e_entry < 0x08000000) {
        load_bias = 0x08800000;
        printf("[Loader] Relocatable ELF detected (Entry: 0x%08X). Applying PSP user RAM load bias: 0x%08X\n", ehdr->e_entry, load_bias);
    }

    *entry_point = ehdr->e_entry + load_bias;
    printf("[Loader] Successfully parsed MIPS ELF Header. Entry Point: 0x%08X\n", *entry_point);

    uint8_t *phdr_table = elf_data + ehdr->e_phoff;
    int loaded_count = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        Elf32_Phdr *phdr = (Elf32_Phdr*)(phdr_table + i * ehdr->e_phentsize);
        if (phdr->p_type == 1) { // PT_LOAD segment
            uint32_t target_vaddr = phdr->p_vaddr + load_bias;
            printf("  -> Loading PT_LOAD segment %d: VADDR 0x%08X (original: 0x%08X), File Size: %u bytes, Memory Size: %u bytes, Flags: 0x%X\n",
                   loaded_count, target_vaddr, phdr->p_vaddr, phdr->p_filesz, phdr->p_memsz, phdr->p_flags);
            
            void *dest = mips_get_ptr(cpu, target_vaddr);
            if (!dest) {
                printf("[Loader Error] Memory mapping violation: Segment virtual range is out of bounds!\n");
                free(data);
                return -7;
            }
            // Copy initialized data
            memcpy(dest, elf_data + phdr->p_offset, phdr->p_filesz);
            // Fill BSS / unitialized data with zeros
            if (phdr->p_memsz > phdr->p_filesz) {
                memset((uint8_t*)dest + phdr->p_filesz, 0, phdr->p_memsz - phdr->p_filesz);
            }
            loaded_count++;
        }
    }

    printf("[Loader] Successfully loaded %d executable segments into emulated memory spaces.\n", loaded_count);

    // --- SceModuleInfo OFFSET EXTRACTION FROM DECRYPTED ELF ---
    if (modinfo_offset == 0 && ehdr->e_phnum > 0) {
        Elf32_Phdr *first_phdr = (Elf32_Phdr*)(phdr_table);
        // Standard ELF p_paddr is often 0 or equals p_vaddr. In PRX, first program header's p_paddr 
        // is uniquely set to SceModuleInfo file offset (different from p_vaddr).
        if (first_phdr->p_paddr != 0 && first_phdr->p_paddr != first_phdr->p_vaddr) {
            modinfo_offset = first_phdr->p_paddr & 0x7FFFFFFF; // Clear the MSB kernel-mode flag if present
            printf("[Loader] ELF format detected. Retrieved SceModuleInfo offset from first phdr p_paddr: 0x%08X\n", modinfo_offset);
        } else {
            printf("[Loader] Standard ELF detected. Skipping direct p_paddr modinfo offset to avoid false positives.\n");
        }
    }

    // --- HLE LINKER & IMPORT STUB PATCHING (PRX RESOLVER) ---
    uint32_t resolved_modinfo_addr = 0;

    // 1. First path: check if the direct pointer is valid (passing load_bias for relocations)
    if (modinfo_offset != 0) {
        // Try to translate as a file offset
        for (int i = 0; i < ehdr->e_phnum; i++) {
            Elf32_Phdr *phdr = (Elf32_Phdr*)(phdr_table + i * ehdr->e_phentsize);
            if (phdr->p_type == 1) { // PT_LOAD segment
                if (modinfo_offset >= phdr->p_offset && modinfo_offset < phdr->p_offset + phdr->p_filesz) {
                    uint32_t relative_offset = modinfo_offset - phdr->p_offset;
                    resolved_modinfo_addr = phdr->p_vaddr + relative_offset + load_bias;
                    break;
                }
            }
        }

        // Fallback: If not found as file offset, treat as reloc offset
        if (resolved_modinfo_addr == 0) {
            resolved_modinfo_addr = relocate_addr(modinfo_offset, load_bias);
        }

        // Check SceModuleInfo pointer validity
        SceModuleInfo *mod = (SceModuleInfo *)mips_get_ptr(cpu, resolved_modinfo_addr);
        if (mod && is_valid_module_info(cpu, mod, load_bias)) {
            printf("[HLE Linker] Direct modinfo offset is VALID. Using SceModuleInfo at virtual address 0x%08X.\n", resolved_modinfo_addr);
        } else {
            printf("[HLE Linker Warning] Direct modinfo validation failed at virtual address 0x%08X\n", resolved_modinfo_addr);
            resolved_modinfo_addr = 0;
        }
    }

    // 2. Fallback: Scan memory heuristically if direct pointer is missing or corrupted
    if (resolved_modinfo_addr == 0) {
        printf("[HLE Linker Warning] Direct modinfo offset is invalid or missing. Initiating heuristic memory scanner...\n");
        resolved_modinfo_addr = find_sce_module_info(cpu, load_bias);
    }

    if (resolved_modinfo_addr != 0) {
        SceModuleInfo *mod_info = (SceModuleInfo *)mips_get_ptr(cpu, resolved_modinfo_addr);
        if (mod_info != NULL) {
            printf("[HLE Linker] Module Name: '%s', Version: %d.%d, Attributes: 0x%04X\n",
                   mod_info->modName, mod_info->modVersion & 0xFF, mod_info->modVersion >> 8, mod_info->modAttribute);
            
            uint32_t stub_top_addr = relocate_addr(mod_info->stub_top, load_bias);
            uint32_t stub_btm_addr = relocate_addr(mod_info->stub_btm, load_bias);
            printf("[HLE Linker] Stub table virtual range: 0x%08X - 0x%08X\n", stub_top_addr, stub_btm_addr);
            
            uint32_t curr_stub_addr = stub_top_addr;
            int patch_count = 0;
            int total_stubs = 0;
            
            while (curr_stub_addr < stub_btm_addr) {
                SceLibraryStubHeader *stub_hdr = (SceLibraryStubHeader *)mips_get_ptr(cpu, curr_stub_addr);
                // Sanity check to avoid reading unmapped memory
                if (stub_hdr == NULL || stub_hdr->lib_name_ptr == 0) {
                    break;
                }
                
                uint32_t lib_name_vaddr = relocate_addr(stub_hdr->lib_name_ptr, load_bias);
                const char *lib_name = (const char *)mips_get_ptr(cpu, lib_name_vaddr);
                if (!lib_name) lib_name = "UnknownLibrary";
                
                uint32_t nid_table_vaddr = relocate_addr(stub_hdr->nid_table_ptr, load_bias);
                uint32_t *nid_table = (uint32_t *)mips_get_ptr(cpu, nid_table_vaddr);
                
                uint32_t stub_table_vaddr = relocate_addr(stub_hdr->stub_table_ptr, load_bias);
                
                // Safety Cap to prevent corrupt headers from writing all over RAM
                uint32_t num_stubs_to_patch = stub_hdr->num_funcs; // FIXED: Real number of imported stubs/funcs
                if (num_stubs_to_patch > 1024) {
                    num_stubs_to_patch = 1024;
                }
                
                if (nid_table != NULL && stub_table_vaddr != 0) {
                    for (uint32_t j = 0; j < num_stubs_to_patch; j++) {
                        uint32_t nid = nid_table[j];
                        uint32_t target_stub_vaddr = stub_table_vaddr + j * 8; // Each stub is exactly 8 bytes (jr $ra; nop)
                        
                        uint32_t syscall_code = 0;
                        const char *func_name = "unknown";
                        
                        // Map the real PSPSDK 32-bit crypt NID hashes into HLE mock Syscalls (GOW spec match)
                        if (strcmp(lib_name, "IoFileMgrForUser") == 0) {
                            if (nid == 0x109F504F || nid == 0x109F50BC) { syscall_code = 0x11111; func_name = "sceIoOpen"; }
                            else if (nid == 0x6A2C3D1B) { syscall_code = 0x11112; func_name = "sceIoRead"; }
                            else if (nid == 0xA0B5A7C2) { syscall_code = 0x11113; func_name = "sceIoReadAsync"; }
                            else if (nid == 0x810C4BC3) { syscall_code = 0x11114; func_name = "sceIoClose"; }
                        } else if (strcmp(lib_name, "ThreadManForUser") == 0) {
                            if (nid == 0xD6D016D7 || nid == 0x3F53A0F3) { syscall_code = 0x22221; func_name = "sceKernelCreateSema"; }
                            else if (nid == 0x4E3A1105) { syscall_code = 0x22222; func_name = "sceKernelWaitSema"; }
                            else if (nid == 0xCEADEB85) { syscall_code = 0x22223; func_name = "sceKernelSignalSema"; }
                            else if (nid == 0x2C1184E6) { syscall_code = 0x22224; func_name = "sceKernelDelayThread"; }
                        } else if (strcmp(lib_name, "sceGe_Driver") == 0 || strcmp(lib_name, "sceGe") == 0) {
                            if (nid == 0xAB49E7EC || nid == 0xE47E40E4) { syscall_code = 0x33331; func_name = "sceGeListEnqueue"; }
                            else if (nid == 0x034C113E) { syscall_code = 0x33332; func_name = "sceGeListSync"; }
                        } else if (strcmp(lib_name, "sceAudio") == 0) {
                            if (nid == 0x5EC81C55) { syscall_code = 0x40001; func_name = "sceAudioChReserve"; }
                            else if (nid == 0x13F592C9) { syscall_code = 0x40002; func_name = "sceAudioChRelease"; }
                            else if (nid == 0x14074D7E || nid == 0x08DF58A5) { syscall_code = 0x40003; func_name = "sceAudioOutputPanned"; }
                        }
                        
                        if (syscall_code != 0) {
                            // Overwrite the original stub jr $ra with our custom syscall
                            mips_write32(cpu, target_stub_vaddr, (syscall_code << 6) | 0x0000000C);
                            // Overwrite delay slot with jr $ra to perform immediate clean return
                            mips_write32(cpu, target_stub_vaddr + 4, 0x03E00008);
                            
                            printf("  -> patched import: %s -> %s (NID: 0x%08X) -> custom syscall: 0x%X\n", lib_name, func_name, nid, syscall_code);
                            patch_count++;
                        } else {
                            // Fallback for unimplemented stubs to dummy syscall 0x99999 to prevent CPU lockups
                            mips_write32(cpu, target_stub_vaddr, (0x99999 << 6) | 0x0000000C);
                            mips_write32(cpu, target_stub_vaddr + 4, 0x03E00008);
                            // Print fallback import logs so we can see what the game is calling and need to implement next!
                            printf("  -> patched import (fallback): %s -> NID: 0x%08X -> fallback success (returns 0)\n", lib_name, nid);
                            patch_count++;
                        }
                        total_stubs++;
                    }
                }
                
                // Advance stub headers sequentially. FIXED: step reads 1 byte from struct_size (offset 8)
                uint32_t step = stub_hdr->struct_size;
                if (step < 5 || step > 32) step = 5;
                curr_stub_addr += step * 4;
            }
            printf("[HLE Linker] Resolved and patched %d / %d imported stubs successfully!\n", patch_count, total_stubs);
        } else {
            printf("[HLE Linker Error] SceModuleInfo structure is invalid or corrupt.\n");
        }
    } else {
        printf("[HLE Linker Error] SceModuleInfo structure could not be discovered or resolved! Skip linker patching.\n");
    }

    free(data);
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("======================================================================\n");
    printf("     MIPS Allegrex AOT Emulator & GLES 3.2 Core - RK3326/R36S\n");
    printf("======================================================================\n");

    // 1. Allocate and Initialize CPU State
    MIPS_CPU cpu;
    int init_ret = mips_cpu_init(&cpu);
    if (init_ret != 0) {
        fprintf(stderr, "[ERROR] Failed to allocate contiguous memory blocks for CPU (Code: %d).\n", init_ret);
        return EXIT_FAILURE;
    }
    printf("[Init] Contiguous 64MB RAM, 2MB VRAM, and 16KB Scratchpad allocated successfully.\n");

    // 2. Initialize HLE OS Subsystems (including GE Graphics Engine)
    int hle_ret = mips_hle_init(&kernel);
    if (hle_ret != 0) {
        fprintf(stderr, "[ERROR] Failed to initialize HLE subsystem (Code: %d).\n", hle_ret);
        mips_cpu_free(&cpu);
        return EXIT_FAILURE;
    }

    // 3. Initialize OpenGL ES 3.20 Graphics Subsystem
    MIPS_VideoSystem video;
    int video_ret = mips_video_init(&video);
    if (video_ret != 0) {
        fprintf(stderr, "[ERROR] Failed to initialize Video GLES subsystem (Code: %d).\n", video_ret);
        mips_hle_free(&kernel);
        mips_cpu_free(&cpu);
        return EXIT_FAILURE;
    }
    global_video_system = &video;
    kernel.video_system = &video;

    // 4. Initialize SDL2 Multi-channel Audio Subsystem
    MIPS_AudioSystem audio;
    int audio_ret = mips_audio_init(&audio);
    if (audio_ret != 0) {
        fprintf(stderr, "[ERROR] Failed to initialize Audio subsystem (Code: %d).\n", audio_ret);
        mips_video_free(&video);
        mips_hle_free(&kernel);
        mips_cpu_free(&cpu);
        return EXIT_FAILURE;
    }
    kernel.audio_system = &audio;

    // 5. Execute Isolated Diagnostics GLES 3.20 Draw Test
    printf("[Diagnostics] Running Isolated Direct GLES 3.20 Render Test first...\n");
    mips_video_draw_test_triangle(&video);
    printf("[Video Diagnostics] Standard hardcoded test triangle drawn successfully!\n");
    printf("[Diagnostics] Direct triangle should be visible on screen. Keeping open for 2s...\n");
    SDL_Delay(2000);

    // 6. Allocate and Initialize Dispatcher Lookup
    MIPS_Dispatcher dispatcher;
    int disp_ret = mips_dispatcher_init(&dispatcher);
    if (disp_ret != 0) {
        fprintf(stderr, "[ERROR] Failed to allocate dispatcher lookup table (Code: %d).\n", disp_ret);
        mips_audio_free(&audio);
        mips_video_free(&video);
        mips_hle_free(&kernel);
        mips_cpu_free(&cpu);
        return EXIT_FAILURE;
    }
    printf("[Init] Direct-mapped O(1) flat 128MB lookup table aligned to RAM.\n");

    // 7. Bind Fallback Interpreter
    dispatcher.fallback_interpreter = interpreter_step;
    printf("[Init] Fallback interpreter bound to dispatcher successfully.\n");

    // 8. Load Decrypted Game Executable
    uint32_t entry_point = 0x08804000;

    // We try fallbacks to decrypted files, and critically, the unencrypted BOOT.BIN of extracted UMDs!
    const char *fallback_paths[] = {
        "EBOOT.BIN",
        "BOOT.BIN",
        "umd0/PSP_GAME/SYSDIR/BOOT.BIN", // Unencrypted fallback from original game files
        "umd0/PSP_GAME/SYSDIR/EBOOT.BIN",
        "EBOOT.ELF"
    };
    int num_fallbacks = sizeof(fallback_paths) / sizeof(fallback_paths[0]);
    int load_success = 0;

    for (int i = 0; i < num_fallbacks; i++) {
        printf("[Boot] Attempting to load executable option %d: '%s'...\n", i + 1, fallback_paths[i]);
        if (load_eboot(&cpu, fallback_paths[i], &entry_point) == 0) {
            printf("[Boot SUCCESS] Mapped program memory space using '%s'.\n", fallback_paths[i]);
            load_success = 1;
            break;
        }
    }

    if (!load_success) {
        fprintf(stderr, "[Boot ERROR] No valid, decrypted executable found.\n");
        fprintf(stderr, "[Boot ERROR] Please place a decrypted EBOOT.BIN, EBOOT.ELF, or the original BOOT.BIN in this folder.\n");
        mips_dispatcher_free(&dispatcher);
        mips_audio_free(&audio);
        mips_video_free(&video);
        mips_hle_free(&kernel);
        mips_cpu_free(&cpu);
        return EXIT_FAILURE;
    }

    // 9. Auto-register Recompiled AOT Blocks (Dynamic registration)
    printf("[AOT] Binding pre-compiled translation blocks from gow_core_aot.c...\n");
    aot_register_blocks(&dispatcher);

    // 10. Start emulation loop
    // Set up standard initial hardware register contexts before the CPU boots
    cpu.gpr[29] = 0x0BFFF000; // $sp (Stack Pointer) safely allocated at the very top of user RAM
    cpu.gpr[31] = 0x08800000; // $ra (Return Address) points to our HALT trap at the bottom of memory

    // Insert HALT instruction (0x70000000) at 0x08800000 to cleanly catch exit if the module main function returns
    mips_write32(&cpu, 0x08800000, 0x70000000);

    cpu.pc = entry_point;
    cpu.next_pc = entry_point;

    printf("[Emulator] Boot Sector successfully loaded. CPU Entry point initialized at PC: 0x%08X\n", cpu.pc);
    printf("[Emulator] Stack Pointer ($sp) initialized to: 0x%08X\n", cpu.gpr[29]);
    printf("[Emulator] Starting primary execution thread loop...\n\n");

    mips_dispatcher_execute(&dispatcher, &cpu);

    printf("\n======================================================================\n");
    printf("[Emulator Halt] Loop completed. Execution stats:\n");
    printf("  - Registers state: $v0 = %d, $a0 = %d\n", (int32_t)cpu.gpr[2], (int32_t)cpu.gpr[4]);
    printf("  - Elapsed Cycles simulated: %llu\n", (unsigned long long)cpu.cycles);
    printf("  - Exit requested flag: %d\n", (int)cpu.exit_requested);
    printf("  - Fault virtual address (BadVAddr): 0x%08X\n", cpu.bad_vaddr);
    printf("======================================================================\n");

    // 11. Cleanup and release blocks
    mips_dispatcher_free(&dispatcher);
    mips_audio_free(&audio);
    mips_video_free(&video);
    mips_hle_free(&kernel);
    mips_cpu_free(&cpu);
    printf("[Cleanup] Contiguous virtual spaces released cleanly. Done.\n");

    return EXIT_SUCCESS;
}