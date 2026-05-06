#include "cpu.h"
#include <stdio.h>

/* Tabela de nomes para o log ficar bonito */
static const char *syscall_name(uint32_t code) {
    switch(code) {
        case 0x22001: return "sceGeListEnQueue";
        case 0x0E004: return "sceDisplayWaitVblankStart";
        case 0x0E000: return "sceDisplaySetMode";
        case 0x0E001: return "sceDisplaySetFrameBuf";
        case 0x0D006: return "sceCtrlPeekBufferPositive";
        
        /* As misteriosas mais chamadas do Kratos */
        case 0x00D59: return "GOW_Unknown_Core_Op1";
        case 0x00D4F: return "GOW_Unknown_Core_Op2";
        case 0x00D48: return "GOW_Unknown_Core_Op3";
        case 0x21400: return "GOW_Giant_Loop_Op1";
        case 0x21800: return "GOW_Giant_Loop_Op2";
        
        default: return "Desconhecida";
    }
}

void psp_syscall(MIPS_CPU *cpu, uint8_t *mem, uint32_t code) {
    // Loga a syscall pra gente ver o jogo rodando no terminal!
    fprintf(stderr, "[SYSCALL] code=0x%05X (%s) a0=0x%08X a1=0x%08X\n",
            code, syscall_name(code), cpu->a0, cpu->a1);

    switch (code) {
        /* ═══════════════════════════════════════════════════════════════
         * GRÁFICOS & TELA (Os códigos reais que você achou!)
         * ═══════════════════════════════════════════════════════════════ */
        case 0x22001: // sceGeListEnQueue
            // O jogo está mandando polígonos pra GPU. 
            // Retornamos um ID fictício da lista (ex: 1) para ele achar que deu certo.
            cpu->v0 = 1; 
            return;

        case 0x0E004: // sceDisplayWaitVblankStart
            // O jogo quer travar a 60 FPS. Retorna 0 (sucesso).
            cpu->v0 = 0;
            return;

        case 0x0E000: // sceDisplaySetMode
        case 0x0E001: // sceDisplaySetFrameBuf
            // O jogo configurou a tela.
            cpu->v0 = 0;
            return;

        /* ═══════════════════════════════════════════════════════════════
         * CONTROLES
         * ═══════════════════════════════════════════════════════════════ */
        case 0x0D006: // sceCtrlPeekBufferPositive
            // Como ainda não ligamos o SDL para ler teclado, 
            // vamos dizer que nenhum botão está apertado.
            // O Kratos vai ficar parado, mas o jogo não vai travar.
            cpu->v0 = 0;
            return;

        /* ═══════════════════════════════════════════════════════════════
         * AS OUTRAS MAIS CHAMADAS DO GOW (Retorno padrão = sucesso)
         * ═══════════════════════════════════════════════════════════════ */
        case 0x00D59:
        case 0x00D4F:
        case 0x00D48:
        case 0x21400:
        case 0x21800:
            cpu->v0 = 0;
            return;

        default:
            // Qualquer outra syscall genérica retorna 0 para evitar travamentos
            cpu->v0 = 0;
            break;
    }
}