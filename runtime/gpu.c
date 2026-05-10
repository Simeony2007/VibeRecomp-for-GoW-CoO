// runtime/gpu.c
#include "cpu.h"
#include "memory.h"
#include <stdio.h>
#include <SDL2/SDL.h> // A biblioteca gráfica

SDL_Window *window = NULL;
SDL_Renderer *renderer = NULL;
SDL_Texture *texture = NULL;

void hle_sceDisplaySetFrameBuf(MIPS_CPU *cpu, uint8_t *mem) {
    uint32_t topaddr = cpu->a0; // Endereço da VRAM (0x04000000)

    // Cria a Janela na primeira vez que a função for chamada
    if (window == NULL) {
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            printf("[GPU] Erro no SDL: %s\n", SDL_GetError());
            cpu->v0 = 0;
            return;
        }
        window = SDL_CreateWindow("PSP Recomp - God of War", 
                                  SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 
                                  480, 272, SDL_WINDOW_SHOWN);
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        // O PSP desenha em 480x272, mas o buffer tem 512 de largura. Formato 32-bits.
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, 
                                    SDL_TEXTUREACCESS_STREAMING, 512, 272);
        printf("[GPU] Janela SDL2 inicializada com sucesso!\n");
    }

    if (topaddr != 0) {
        // Usa a nossa MMU virtual para achar a VRAM
        uint32_t *pixels = (uint32_t*)&mem[psp_translate_addr(topaddr)];
        
        // Joga os pixels da memória RAM emulada para a Placa de Vídeo do seu PC
        SDL_UpdateTexture(texture, NULL, pixels, 512 * 4);
        SDL_RenderClear(renderer);
        
        SDL_Rect srcRect = {0, 0, 480, 272};
        SDL_Rect dstRect = {0, 0, 480, 272};
        SDL_RenderCopy(renderer, texture, &srcRect, &dstRect);
        SDL_RenderPresent(renderer);
    }

    // Processa a janela para ela não travar/congelar no Windows/Linux
    SDL_Event e;
    while(SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) exit(0);
    }

    cpu->v0 = 0;
}