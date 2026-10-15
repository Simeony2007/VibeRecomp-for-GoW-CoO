/*
 * ===========================================================================
 * AUTO-GENERATED MIPS AOT TRANSLATION CODE
 * Source File: EBOOT.BIN
 * Target: R36S (ARM64, Cortex-A35)
 * ===========================================================================
 */

#include "mips_cpu.h"
#include "mips_dispatcher.h"
#include <stdint.h>
#include <stdbool.h>

#include <stdio.h>

// External fallbacks and syscall support
extern void interpreter_step(MIPS_CPU *cpu);
extern void hle_syscall(MIPS_CPU *cpu);


// --- REGISTRATION FUNCTION ---
// Call this function after initializing the dispatcher to bind all AOT blocks.
extern void aot_register_blocks_000(MIPS_Dispatcher *disp);
extern void aot_register_blocks_001(MIPS_Dispatcher *disp);
extern void aot_register_blocks_002(MIPS_Dispatcher *disp);
extern void aot_register_blocks_003(MIPS_Dispatcher *disp);
extern void aot_register_blocks_004(MIPS_Dispatcher *disp);
extern void aot_register_blocks_005(MIPS_Dispatcher *disp);
extern void aot_register_blocks_006(MIPS_Dispatcher *disp);
extern void aot_register_blocks_007(MIPS_Dispatcher *disp);
extern void aot_register_blocks_008(MIPS_Dispatcher *disp);
extern void aot_register_blocks_009(MIPS_Dispatcher *disp);
extern void aot_register_blocks_010(MIPS_Dispatcher *disp);
extern void aot_register_blocks_011(MIPS_Dispatcher *disp);
extern void aot_register_blocks_012(MIPS_Dispatcher *disp);
extern void aot_register_blocks_013(MIPS_Dispatcher *disp);
extern void aot_register_blocks_014(MIPS_Dispatcher *disp);
extern void aot_register_blocks_015(MIPS_Dispatcher *disp);
extern void aot_register_blocks_016(MIPS_Dispatcher *disp);
extern void aot_register_blocks_017(MIPS_Dispatcher *disp);
extern void aot_register_blocks_018(MIPS_Dispatcher *disp);
extern void aot_register_blocks_019(MIPS_Dispatcher *disp);
extern void aot_register_blocks_020(MIPS_Dispatcher *disp);
extern void aot_register_blocks_021(MIPS_Dispatcher *disp);
extern void aot_register_blocks_022(MIPS_Dispatcher *disp);
extern void aot_register_blocks_023(MIPS_Dispatcher *disp);
extern void aot_register_blocks_024(MIPS_Dispatcher *disp);
extern void aot_register_blocks_025(MIPS_Dispatcher *disp);
extern void aot_register_blocks_026(MIPS_Dispatcher *disp);
extern void aot_register_blocks_027(MIPS_Dispatcher *disp);
extern void aot_register_blocks_028(MIPS_Dispatcher *disp);
extern void aot_register_blocks_029(MIPS_Dispatcher *disp);
extern void aot_register_blocks_030(MIPS_Dispatcher *disp);
extern void aot_register_blocks_031(MIPS_Dispatcher *disp);
extern void aot_register_blocks_032(MIPS_Dispatcher *disp);
extern void aot_register_blocks_033(MIPS_Dispatcher *disp);
extern void aot_register_blocks_034(MIPS_Dispatcher *disp);
extern void aot_register_blocks_035(MIPS_Dispatcher *disp);
extern void aot_register_blocks_036(MIPS_Dispatcher *disp);
extern void aot_register_blocks_037(MIPS_Dispatcher *disp);
extern void aot_register_blocks_038(MIPS_Dispatcher *disp);
extern void aot_register_blocks_039(MIPS_Dispatcher *disp);
extern void aot_register_blocks_040(MIPS_Dispatcher *disp);
extern void aot_register_blocks_041(MIPS_Dispatcher *disp);
extern void aot_register_blocks_042(MIPS_Dispatcher *disp);
extern void aot_register_blocks_043(MIPS_Dispatcher *disp);
extern void aot_register_blocks_044(MIPS_Dispatcher *disp);
extern void aot_register_blocks_045(MIPS_Dispatcher *disp);
extern void aot_register_blocks_046(MIPS_Dispatcher *disp);
extern void aot_register_blocks_047(MIPS_Dispatcher *disp);
extern void aot_register_blocks_048(MIPS_Dispatcher *disp);
extern void aot_register_blocks_049(MIPS_Dispatcher *disp);
extern void aot_register_blocks_050(MIPS_Dispatcher *disp);
extern void aot_register_blocks_051(MIPS_Dispatcher *disp);
extern void aot_register_blocks_052(MIPS_Dispatcher *disp);
extern void aot_register_blocks_053(MIPS_Dispatcher *disp);
extern void aot_register_blocks_054(MIPS_Dispatcher *disp);
extern void aot_register_blocks_055(MIPS_Dispatcher *disp);
extern void aot_register_blocks_056(MIPS_Dispatcher *disp);
extern void aot_register_blocks_057(MIPS_Dispatcher *disp);
extern void aot_register_blocks_058(MIPS_Dispatcher *disp);
extern void aot_register_blocks_059(MIPS_Dispatcher *disp);
extern void aot_register_blocks_060(MIPS_Dispatcher *disp);
extern void aot_register_blocks_061(MIPS_Dispatcher *disp);
extern void aot_register_blocks_062(MIPS_Dispatcher *disp);
extern void aot_register_blocks_063(MIPS_Dispatcher *disp);
extern void aot_register_blocks_064(MIPS_Dispatcher *disp);
extern void aot_register_blocks_065(MIPS_Dispatcher *disp);
extern void aot_register_blocks_066(MIPS_Dispatcher *disp);
extern void aot_register_blocks_067(MIPS_Dispatcher *disp);
extern void aot_register_blocks_068(MIPS_Dispatcher *disp);
extern void aot_register_blocks_069(MIPS_Dispatcher *disp);
void aot_register_blocks(MIPS_Dispatcher *disp) {
    aot_register_blocks_000(disp);
    aot_register_blocks_001(disp);
    aot_register_blocks_002(disp);
    aot_register_blocks_003(disp);
    aot_register_blocks_004(disp);
    aot_register_blocks_005(disp);
    aot_register_blocks_006(disp);
    aot_register_blocks_007(disp);
    aot_register_blocks_008(disp);
    aot_register_blocks_009(disp);
    aot_register_blocks_010(disp);
    aot_register_blocks_011(disp);
    aot_register_blocks_012(disp);
    aot_register_blocks_013(disp);
    aot_register_blocks_014(disp);
    aot_register_blocks_015(disp);
    aot_register_blocks_016(disp);
    aot_register_blocks_017(disp);
    aot_register_blocks_018(disp);
    aot_register_blocks_019(disp);
    aot_register_blocks_020(disp);
    aot_register_blocks_021(disp);
    aot_register_blocks_022(disp);
    aot_register_blocks_023(disp);
    aot_register_blocks_024(disp);
    aot_register_blocks_025(disp);
    aot_register_blocks_026(disp);
    aot_register_blocks_027(disp);
    aot_register_blocks_028(disp);
    aot_register_blocks_029(disp);
    aot_register_blocks_030(disp);
    aot_register_blocks_031(disp);
    aot_register_blocks_032(disp);
    aot_register_blocks_033(disp);
    aot_register_blocks_034(disp);
    aot_register_blocks_035(disp);
    aot_register_blocks_036(disp);
    aot_register_blocks_037(disp);
    aot_register_blocks_038(disp);
    aot_register_blocks_039(disp);
    aot_register_blocks_040(disp);
    aot_register_blocks_041(disp);
    aot_register_blocks_042(disp);
    aot_register_blocks_043(disp);
    aot_register_blocks_044(disp);
    aot_register_blocks_045(disp);
    aot_register_blocks_046(disp);
    aot_register_blocks_047(disp);
    aot_register_blocks_048(disp);
    aot_register_blocks_049(disp);
    aot_register_blocks_050(disp);
    aot_register_blocks_051(disp);
    aot_register_blocks_052(disp);
    aot_register_blocks_053(disp);
    aot_register_blocks_054(disp);
    aot_register_blocks_055(disp);
    aot_register_blocks_056(disp);
    aot_register_blocks_057(disp);
    aot_register_blocks_058(disp);
    aot_register_blocks_059(disp);
    aot_register_blocks_060(disp);
    aot_register_blocks_061(disp);
    aot_register_blocks_062(disp);
    aot_register_blocks_063(disp);
    aot_register_blocks_064(disp);
    aot_register_blocks_065(disp);
    aot_register_blocks_066(disp);
    aot_register_blocks_067(disp);
    aot_register_blocks_068(disp);
    aot_register_blocks_069(disp);
}