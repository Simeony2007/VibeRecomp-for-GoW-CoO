# PSP Recomp Engine - God of War: Chains of Olympus

Um emulador de recompilação dinâmica (HLE) projetado especificamente para analisar e executar o binário original de *God of War: Chains of Olympus* (PSP) em arquiteturas modernas.

## 📌 O que já temos pronto (O que foi feito)
- **Leitor ELF/PRX Nativo:** Um extrator em Python (`parse_elf.py`) que decodifica o `EBOOT.BIN` descriptado, mapeia segmentos de memória e resolve nativamente a tabela de importação (`sceModuleInfo`).
- **Roteador HLE Dinâmico:** Geração automatizada de rotas (`hle_router.h`) que intercepta chamadas MIPS e as redireciona para funções C no host.
- **CPU Virtual:** Um loop de execução (Dispatcher) capaz de interpretar instruções MIPS e gerenciar o estado dos registradores.
- **Escalonador de Threads (Scheduler):** Um gerenciador de paralelismo real que cria threads independentes, aloca Stacks isoladas e realiza Troca de Contexto (Context Switch) baseada no ciclo de vida do jogo.

## 🚀 O que estamos fazendo (Foco Atual)
- **Engenharia Reversa do Boot:** Isolando e resolvendo dependências iniciais do motor da Santa Monica.
- **Sincronização HLE:** Implementando respostas simuladas para Syscalls do Kernel (Semáforos, Delays, Callbacks) para impedir Deadlocks e Polling Loops infinitos.
- **Gerenciamento do Ciclo de Vida de Threads:** Blindando o motor contra saltos em ponteiros nulos (`$ra` vazios) após a morte natural de uma Thread do jogo.

## 🎯 O que vamos fazer (Próximos Passos)
1. **Sistema de I/O Virtual:** Implementar `sceIoOpen`, `sceIoRead` e `sceIoClose` para o jogo conseguir ler os arquivos `.arc` e `.wad` do disco.
2. **Alocação de Memória RAM:** Codificar a syscall `sceKernelAllocPartitionMemory` para entregar blocos dinâmicos de memória para o motor 3D do Kratos.
3. **Ponte de Áudio e Vídeo:** Interceptar chamadas da GE (Graphics Engine) e do Atrac3 para renderizar gráficos e som no sistema host via bibliotecas como SDL2.

## Aviso legal

Este projeto é apenas para fins educacionais e de preservação.
Requer que você possua uma cópia legítima do jogo.

TODOS OS CÓDIGOS SÃO GERADOS POR IA, EU APENAS ORGANIZO E COMANDO O PROCESSO DE CRIAÇÃO E DEBUGGING.
