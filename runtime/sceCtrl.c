/**
 * runtime/sceCtrl.c
 * Subsistema completo de I/O do sceCtrl (Controlador PSP)
 *
 * ARQUITETURA:
 * O GoW:CoO não chama sceCtrl via syscall para ler input — ele acessa
 * diretamente a região de memória do driver em 0x00330000 (sem relocation).
 * Com a máscara & 0x01FFFFFF do MEM_PTR, isso vira mem[0x00330000].
 *
 * O driver real do PSP mantém um ring-buffer de amostras nessa região.
 * O jogo lê o ponteiro em (0x00330000 - 0x6780) + 0x10 para achar o buffer.
 *
 * LAYOUT DA REGIÃO DO DRIVER (base = 0x003298C0 = 0x00330000 - 0x6740):
 *   +0x00: flags internos do driver
 *   +0x08: ponteiro para o buffer de amostras atual  <- jogo lê isso
 *   +0x10: ponteiro para o buffer de amostras atual  <- func_0880A988 lê (s1 - 26560 + 0x10 via a2)
 *   +0x14: tamanho do bloco - slot (usado no cálculo de a1 = a1 - s0)
 *
 * LAYOUT DE CADA AMOSTRA (SceCtrlData):
 *   +0x00: timestamp (microsegundos)
 *   +0x04: botões pressionados (bitmask)
 *   +0x06: analog_x (0-255, centro=128)
 *   +0x07: analog_y (0-255, centro=128)
 *
 * BOTÕES (bitmask de 32 bits):
 *   SELECT   = 0x000001    START    = 0x000008
 *   UP       = 0x000010    RIGHT    = 0x000020
 *   DOWN     = 0x000040    LEFT     = 0x000080
 *   LTRIGGER = 0x000100    RTRIGGER = 0x000200
 *   TRIANGLE = 0x001000    CIRCLE   = 0x002000
 *   CROSS    = 0x004000    SQUARE   = 0x008000
 *   HOME     = 0x010000    HOLD     = 0x020000
 *   NOTE     = 0x800000
 */

#include "cpu.h"
#include "memory.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ── Constantes do layout de memória ──────────────────────────────────────── */

/* Endereço base da região interna do driver sceCtrl na RAM do PSP.
 * Calculado a partir do LUI s1, 0x0033 -> s1 = 0x00330000
 * e do offset imediato -26560 (0xFFFF9880 = -26496 dec = -0x6780)
 * 0x00330000 - 0x6780 = 0x003298C0 -> com máscara & 0x01FFFFFF = 0x003298C0 */
#define CTRL_DRIVER_BASE    0x003298C0u

/* Offset dentro do driver onde fica o ponteiro para o buffer de amostras.
 * func_0880A988 faz: a2 = MEM_R32(mem, s1 - 26560)  <- endereço do buffer
 *                    a0 = MEM_R32(mem, a2 + 16)       <- dado dentro do buffer */
#define CTRL_BUF_PTR_OFF    0x00u   /* offset de 'a2' = ptr do buffer de amostras */

/* Onde o buffer de amostras em si vai ficar na RAM emulada.
 * Escolhemos uma região segura no topo da SPRAM emulada, longe do código. */
#define CTRL_SAMPLE_BUF     0x003299C0u  /* logo após o bloco do driver */

/* Número de amostras no ring-buffer (o PSP usa 64 por padrão) */
#define CTRL_RING_SIZE      64

/* Tamanho de cada SceCtrlData em bytes */
#define CTRL_SAMPLE_SIZE    8u

/* ── Estado global do subsistema ──────────────────────────────────────────── */
typedef struct {
    uint32_t timestamp;   /* +0x00 microsegundos */
    uint32_t buttons;     /* +0x04 bitmask de botões */
    uint8_t  lx;          /* +0x08 eixo X analógico (128 = centro) */
    uint8_t  ly;          /* +0x09 eixo Y analógico (128 = centro) */
    uint8_t  _pad[2];     /* +0x0A padding */
} SceCtrlData;

static struct {
    uint32_t      buttons_held;   /* Botões atualmente pressionados */
    uint8_t       lx, ly;         /* Analógico */
    uint32_t      frame;          /* Contador de frames */
    int           initialized;
} ctrl_state = {
    .buttons_held = 0,
    .lx = 128,
    .ly = 128,
    .frame = 0,
    .initialized = 0
};

/* ── Inicialização: popula a memória do driver ────────────────────────────── */

/**
 * ctrl_mem_init() - DEVE ser chamado no loader.c ANTES do loop principal.
 *
 * Preenche a região 0x003298C0 da RAM emulada com uma estrutura válida
 * para que func_0880A988 encontre um ponteiro não-nulo e saia do loop.
 *
 * Chamada em loader.c:
 *   extern void ctrl_mem_init(uint8_t *mem);
 *   ctrl_mem_init(mem);
 */
void ctrl_mem_init(uint8_t *mem) {
    /* 1. Zera toda a região do driver */
    uint32_t driver_off = CTRL_DRIVER_BASE & 0x01FFFFFFu;
    memset(&mem[driver_off], 0, 256);

    /* 2. Escreve o ponteiro do buffer de amostras no offset que o jogo lê.
     *    func_0880A988: a2 = MEM_R32(mem, s1 + -26560)  [= mem[CTRL_DRIVER_BASE]]
     *    Então mem[CTRL_DRIVER_BASE + 0] deve conter o endereço do buffer. */
    uint32_t buf_vaddr = CTRL_SAMPLE_BUF;
    MEM_W32(mem, CTRL_DRIVER_BASE + 0x00, buf_vaddr);

    /* 3. O jogo depois faz: a0 = MEM_R32(mem, a2 + 16)
     *    Então o buffer em si, no offset +16, deve ter um valor não-nulo
     *    (ou o jogo pularia). Vamos colocar um segundo ponteiro apontando
     *    para a própria amostra. */
    uint32_t sample_off = CTRL_SAMPLE_BUF & 0x01FFFFFFu;
    memset(&mem[sample_off], 0, CTRL_RING_SIZE * CTRL_SAMPLE_SIZE + 64);

    /* Offset +16 do buffer aponta para a primeira amostra */
    MEM_W32(mem, CTRL_SAMPLE_BUF + 0x10, CTRL_SAMPLE_BUF + 0x20);

    /* 4. Preenche a primeira amostra com estado neutro (nenhum botão, analógico centrado) */
    uint32_t s0 = (CTRL_SAMPLE_BUF + 0x20) & 0x01FFFFFFu;
    MEM_W32(mem, s0 + 0x00, 1000u);  /* timestamp inicial */
    MEM_W32(mem, s0 + 0x04, 0u);     /* nenhum botão */
    mem[(s0 + 0x08) & 0x01FFFFFFu] = 128; /* lx = centro */
    mem[(s0 + 0x09) & 0x01FFFFFFu] = 128; /* ly = centro */

    ctrl_state.initialized = 1;
    printf("[CTRL] Subsistema sceCtrl inicializado. Driver em 0x%08X, buffer em 0x%08X\n",
           CTRL_DRIVER_BASE, CTRL_SAMPLE_BUF);

    /* ── Ring buffer de slots de frame (double buffering do engine) ──────────── */
    // 0x003299B0: índice do slot atual (lido em 0x08811850, offset -26064)
    // 0x003299BC: contador de frames   (lido em 0x08811864, offset -26052)
    // 0x003299A8: array de slots — slot[0] e slot[1] são ponteiros para frame descriptors

    // Dois frame descriptors numa região segura
    #define FRAME_DESC_0  0x00329A00u
    #define FRAME_DESC_1  0x00329A40u

    // Inicializa os dois descriptors com flags válidas
    // O campo +0x04 precisa ter o bit 0x02 setado (flag "buffer pronto")
    // O campo +0x00 precisa ser não-zero (flag "ativo")
    MEM_W32(mem, FRAME_DESC_0 + 0x00, 0x00000003u); // flags: ativo + pronto
    MEM_W32(mem, FRAME_DESC_0 + 0x04, 0x00000002u); // bit 0x02 = buffer válido
    MEM_W32(mem, FRAME_DESC_0 + 0x08, 0x00000000u); // bit 0x01 = não é o tipo alternativo

    MEM_W32(mem, FRAME_DESC_1 + 0x00, 0x00000003u);
    MEM_W32(mem, FRAME_DESC_1 + 0x04, 0x00000002u);
    MEM_W32(mem, FRAME_DESC_1 + 0x08, 0x00000000u);

    // Array de slots: slot[0] → FRAME_DESC_0, slot[1] → FRAME_DESC_1
    MEM_W32(mem, 0x003299A8u + 0x00, FRAME_DESC_0); // slot[0]
    MEM_W32(mem, 0x003299A8u + 0x04, FRAME_DESC_1); // slot[1]

    // Índice atual do slot (0 = primeiro slot)
    MEM_W32(mem, 0x003299B0u, 0x00000000u); // offset -26064: slot atual = 0

    // Contador de frames (começa em 0)
    MEM_W32(mem, 0x003299BCu, 0x00000000u); // offset -26052: frame count = 0

    printf("[CTRL] Ring buffer de frames inicializado. Slots: 0x%08X, 0x%08X\n",
        FRAME_DESC_0, FRAME_DESC_1);
}

/* ── Atualização do estado (chame a cada frame ou tick) ──────────────────── */

/**
 * ctrl_update() - Atualiza o buffer de amostras na RAM emulada.
 *
 * Por enquanto mantém o controlador em estado neutro (sem input).
 * Para adicionar input real (SDL, evdev, etc.), modifique ctrl_state
 * antes de chamar esta função.
 */
void ctrl_update(uint8_t *mem) {
    if (!ctrl_state.initialized) return;

    ctrl_state.frame++;

    uint32_t s0 = (CTRL_SAMPLE_BUF + 0x20) & 0x01FFFFFFu;
    MEM_W32(mem, s0 + 0x00, ctrl_state.frame * 16667u); /* ~60fps em micros */
    MEM_W32(mem, s0 + 0x04, ctrl_state.buttons_held);
    mem[(s0 + 0x08) & 0x01FFFFFFu] = ctrl_state.lx;
    mem[(s0 + 0x09) & 0x01FFFFFFu] = ctrl_state.ly;
}

/* ── Syscalls HLE do sceCtrl ─────────────────────────────────────────────── */

/**
 * sceCtrlSetSamplingCycle - nid_0x6A2774F3 (0x08B01C0C)
 * Define o ciclo de amostragem do controlador (em microssegundos).
 * a0 = ciclo (0 = VSync, 1-333 = valor em ms)
 * Retorna 0 em sucesso.
 */
void hle_sceCtrlSetSamplingCycle(MIPS_CPU *cpu, uint8_t *mem) {
    printf("[HLE CTRL] sceCtrlSetSamplingCycle | ciclo=%u\n", cpu->a0);
    cpu->v0 = 0;
}

/**
 * sceCtrlSetSamplingMode - nid_0xA7144800 (0x08B01C14)
 * Define o modo de amostragem.
 * a0 = mode (0 = digital, 1 = analógico)
 * Retorna 0 em sucesso.
 */
void hle_sceCtrlSetSamplingMode(MIPS_CPU *cpu, uint8_t *mem) {
    printf("[HLE CTRL] sceCtrlSetSamplingMode | mode=%u\n", cpu->a0);
    cpu->v0 = 0;
}

/**
 * sceCtrlReadBufferPositive - nid_0x1F4011E6 (0x08B01BFC)
 * Lê amostras do controlador (bloqueia até ter dados).
 * a0 = ponteiro para SceCtrlData na RAM do PSP
 * a1 = número de amostras a ler
 * Retorna número de amostras lidas.
 *
 * NOTA: No PSP real, esta função bloqueia a thread até a próxima amostragem.
 * Aqui simplesmente escrevemos o estado atual e retornamos imediatamente.
 */
void hle_sceCtrlReadBufferPositive(MIPS_CPU *cpu, uint8_t *mem) {
    uint32_t out_ptr = cpu->a0;
    uint32_t count   = cpu->a1;

    ctrl_update(mem);

    if (out_ptr != 0 && count > 0) {
        uint32_t off = out_ptr & 0x01FFFFFFu;
        MEM_W32(mem, off + 0x00, ctrl_state.frame * 16667u);
        MEM_W32(mem, off + 0x04, ctrl_state.buttons_held);
        mem[(off + 0x08) & 0x01FFFFFFu] = ctrl_state.lx;
        mem[(off + 0x09) & 0x01FFFFFFu] = ctrl_state.ly;
    }

    cpu->v0 = (count > 0) ? 1 : 0;
}

/**
 * sceCtrlPeekBufferPositive - nid_0x3A622550 (0x08B01C04)
 * Lê amostras sem bloquear a thread.
 * Mesma assinatura que sceCtrlReadBufferPositive.
 */
void hle_sceCtrlPeekBufferPositive(MIPS_CPU *cpu, uint8_t *mem) {
    /* Comportamento idêntico ao Read no nosso contexto */
    hle_sceCtrlReadBufferPositive(cpu, mem);
}

/**
 * sceCtrlGetSamplingCycle - nid_0x3A622550 companheiro
 * Retorna o ciclo atual de amostragem.
 */
void hle_sceCtrlGetSamplingCycle(MIPS_CPU *cpu, uint8_t *mem) {
    if (cpu->a0 != 0) {
        MEM_W32(mem, cpu->a0 & 0x01FFFFFFu, 0u); /* ciclo = VSync */
    }
    cpu->v0 = 0;
}

/**
 * sceCtrlGetSamplingMode
 * Retorna o modo de amostragem atual.
 */
void hle_sceCtrlGetSamplingMode(MIPS_CPU *cpu, uint8_t *mem) {
    if (cpu->a0 != 0) {
        MEM_W32(mem, cpu->a0 & 0x01FFFFFFu, 1u); /* modo analógico */
    }
    cpu->v0 = 0;
}
