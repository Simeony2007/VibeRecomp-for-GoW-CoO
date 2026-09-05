# 🎮 MIPS Allegrex AOT Emulator & GLES 3.2 Core

## Port de Alta Performance para God of War (RK3326/R36S)

Este projeto consiste em um **emulador híbrido de PSP altamente otimizado** e customizado para rodar o jogo *God of War* em plataformas de baixo custo baseadas em ARM (como os consoles portáteis R36S e placas RK3326) executando sistemas baseados em Linux. 

O motor de emulação combina **recompilação estática Ahead-of-Time (AOT)** para traduzir blocos de instruções MIPS da CPU Allegrex diretamente em código de alta performance, mantendo um **interpretador universal** robusto como fallback de segurança. A renderização gráfica é feita via mapeamento direto de geometria e comandos do GE (Graphics Engine) para shaders modernos de **OpenGL ES 3.20**, enquanto o áudio estéreo é processado em tempo real por um mixer multi-canal baseado em **SDL2**.

---

## 🤖 100% Desenvolvido por Inteligência Artificial (AI-Built)

Este projeto possui uma característica histórica de engenharia: **todo o seu código-fonte, arquitetura de memória, correções de baixo nível, scripts de build inteligente por cache e otimização de linker HLE foram projetados, desenvolvidos e depurados de forma 100% autônoma por Inteligência Artificial (Gemini Notebook / AI collaboration).** 

Desde o mapeamento milimétrico do barramento físico da RAM de 64MB do PSP, passando pela correção de regras estritas de aliasing do compilador (*Strict Aliasing*), até a virtualização de relocamento do Linker HLE para binários ELF relocáveis, o código foi gerado e revisado iterativamente por inteligência artificial para extrair a máxima performance diretamente no hardware Mali GPU do RK3326.

---

## 🚨 Requisitos Obrigatórios e Cruciais

Para o funcionamento do emulador, a arquitetura de High-Level Emulation (HLE) exige estritamente a presença dos arquivos originais do jogo dispostos no diretório local. **O emulador não acompanha nenhuma propriedade intelectual ou arquivo proprietário da Sony ou da Ready at Dawn.**

### 1. Jogo Original Requerido (Dump da UMD)
Você deve possuir a mídia física original do jogo e realizar o dump completo da imagem. Os arquivos de recursos devem ser organizados na estrutura de pastas da UMD para que o sistema de arquivos virtual (VFS) do emulador consiga ler os dados de jogo:
*   A pasta **`umd0/`** deve estar na raiz do executável.
*   O arquivo de dados principal do jogo **`GODOFWAR.WAD`** deve estar localizado em:
    `./umd0/PSP_GAME/USRDIR/GODOFWAR.WAD`
*   O arquivo de identificação **`UMD_DATA.BIN`** deve estar em:
    `./umd0/UMD_DATA.BIN`

### 2. EBOOT.BIN Descriptografado (Decrypted ELF)
O PSP real possui chaves de criptografia proprietárias por hardware (gerenciadas pelo coprocessador de segurança KIRK). Como o nosso emulador roda puramente em nível de usuário (High-Level Emulation), ele **não contém e não emula as chaves físicas de criptografia da Sony**. 

Portanto, o executável principal do jogo **`EBOOT.BIN` deve ser fornecido em formato ELF totalmente descriptografado e descompactado**.
*   **Como descriptografar o EBOOT.BIN:**
    1. Abra o emulador PPSSPP no seu computador.
    2. Vá em **Configurações -> Ferramentas -> Ferramentas do Desenvolvedor** (Developer Tools).
    3. Ative a caixa **"Dump Decrypted EBOOTs"** (Descriptografar EBOOTs).
    4. Execute o jogo *God of War* no PPSSPP por 2 segundos e feche-o.
    5. O PPSSPP gerará um arquivo ELF puro e descriptografado na pasta: `memstick/PSP/SYSTEM/DUMP/`.
    6. Copie esse arquivo de dump, renomeie-o para **`EBOOT.BIN`** e coloque-o diretamente na pasta raiz do emulador.

---

## 📐 Estrutura Arquitetural do Emulador

```
/GOW/ (Diretório Raiz)
├── EBOOT.BIN                 <-- Executável do jogo (MIPS ELF descriptografado obrigatório)
├── gow_core_aot.c            <-- Tradução estática compilada AOT das instruções do jogo
├── main_v12.c                <-- Loader dinâmico, Linker HLE e loop principal da CPU
├── mips_cpu.h                <-- Tradução de barramento e mapa físico de RAM de 64MB
├── mips_dispatcher.h         <-- Tabela de lookup JIT/AOT direta e ultra-rápida (O(1))
├── mips_interpreter.c        <-- Interpretador universal de fallback da CPU MIPS
├── mips_hle.c                <-- Kernel HLE de chamadas de sistema (I/O, Threads, Sema)
├── mips_video.c              <-- Renderer gráfico acelerado (Mapeamento Mali GPU OpenGL ES 3.20)
├── mips_audio.c              <-- Driver de áudio SDL2 com relógio de sincronização contínuo
├── build_fixed_v15.sh        <-- Script de compilação automatizado por caching inteligente
└── umd0/                     <-- Pasta contendo a extração da UMD do jogo original
    ├── UMD_DATA.BIN
    └── PSP_GAME/
        ├── SYSDIR/
        └── USRDIR/
            └── GODOFWAR.WAD  <-- Arquivo de dados de cena e modelos original
```

---

## 🛠️ Como Compilar e Executar

O projeto possui um compilador em cache inteligente para contornar o congelamento de memória durante a compilação do arquivo gigante `gow_core_aot.c` (que possui milhões de linhas). 

1.  Dê permissão de execução e compile o projeto executando o script versão 15 no seu terminal Linux (ou WSL):
    ```bash
    chmod +x build_fixed_v15.sh
    ./build_fixed_v15.sh
    ```
    *O compilador criará de forma ultra-rápida um arquivo de cache estático `./output/gow_core_aot.o` sem otimizações demoradas de CPU, mantendo o restante do núcleo otimizado com `-O2` para máxima performance.*

2.  Rode o emulador diretamente do executável gerado:
    ```bash
    ./output/main
    ```

Ao iniciar, o emulador executará o teste isolado da GPU renderizando um triângulo de diagnóstico em OpenGL ES 3.2, e logo em seguida ativará o **Linker HLE**, decodificando e vinculando todas as chamadas de sistema da API oficial do God of War e inicializando a execução contínua do jogo com suporte de áudio e vídeo sincronizados!
