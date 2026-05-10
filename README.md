# PSP Recomp Engine - God of War: Chains of Olympus → ARM64/R36S

**Objetivo:** Port funcional de *God of War: Chains of Olympus* (PSP) para ARM64, com foco no handheld **R36S**. O projeto usa HLE (High-Level Emulation) para rodar o EBOOT.BIN original recompilando blocos MIPS em C e interceptando syscalls do PSP.

---

## 📋 Checklist de Progresso

### ✅ Fase 1: Infraestrutura Base (CONCLUÍDA - 100%)
- [x] Leitor ELF/PRX: Script Python (`parse_elf.py`) decodifica EBOOT.BIN e extrai metadados para `elf_meta.json`.
- [x] CPU Virtual MIPS: Dispatcher C capaz de executar blocos MIPS recompilados com gerenciamento de registradores.
- [x] Escalonador de Threads: Suporta múltiplas threads, troca de contexto e EventFlags.
- [x] Sistema de Memória MMU: Mapeamento virtual PSP (0x08800000 → índice RAM com tradução de VRAM/kernel mirror).
- [x] Roteador HLE: Interceptação de syscalls e redirecionamento para stubs C.

### 🟠 Fase 2: Correções de Arquitetura Críticas (EM ANDAMENTO - 70%)
- [x] Tradução correta de endereços ELF → RAM com `load_base` do JSON.
- [x] Normalização de endereços espelhados/kernel mirror com máscara `0x1FFFFFFF`.
- [x] Dispatcher falha imediatamente em PC inválido (sem fallback 4096 bytes).
- [x] Remoção do bypass de EventFlag que mentia sucesso para `evid == 0`.
- [x] **Identificado problema raiz do PC inválido:** GP em `elf_meta.json` está sendo 0x08800000 quando deveria ser 0x08B533B0 ou outro valor correto.
  - O `parse_elf.py` está confundindo `gp_value` com `load_base`.
  - Quando GP está errado, acessos `lw t9, offset(gp)` leem lixo da memória.
  - Isso causa saltos para endereços inválidos (0xAFB2001C).
- [ ] **PRÓXIMO:** Usar `extract_gp.py` para extrair o GP real do símbolo ELF e atualizar JSON.

### 🔴 Fase 3: HLE Syscalls Essenciais (PENDENTE - 0%)
- [ ] **I/O de Arquivo:**
  - `sceIoOpen`, `sceIoRead`, `sceIoClose` para carregar `.arc` / `.wad`.
  - `sceIoDopen`, `sceIoDread` para navegação de pastas.
- [ ] **Alocação de Memória:**
  - `sceKernelAllocPartitionMemory` para blocos dinâmicos.
  - `sceKernelFreePartitionMemory` para liberação.
- [ ] **Timers:**
  - `sceKernelUSleep` para esperas não-bloqueantes.
  - `sceKernelGetSystemTime` para relógio do jogo.
- [ ] **Mutex/Semáforo:**
  - `sceKernelCreateMutex`, `sceKernelLockMutex`, `sceKernelUnlockMutex`.

### 🔴 Fase 4: Gráficos e Áudio (PENDENTE - 0%)
- [ ] **GE (Graphics Engine):**
  - `sceGeListEnQueue`, `sceGeListSync` com suporte real de listas de comando.
  - Renderização via SDL2 ou Vulkan.
- [ ] **Atrac3/Áudio:**
  - `sceAudioChReserve` para reserva de canal de áudio.
  - Decode de Atrac3 e playback via SDL2 Audio.
- [ ] **Display:**
  - Framebuffer duplo com suporte a 480×272 PSP.

### 🔴 Fase 5: Integração ARM64 / R36S (PENDENTE - 0%)
- [ ] Compilação cruzada para ARM64.
- [ ] Otimizações de performance para handheld (cache, SIMD).
- [ ] Integração com sistema de arquivos do R36S.
- [ ] Suporte a controle/input nativo do dispositivo.

---

## 🔬 Status Técnico Atual

**O que funciona:**
- ✅ Carregamento de ELF com mapeamento correto de segmentos (agora usando `load_base` do JSON).
- ✅ Execução de blocos MIPS com dispatcher funcional.
- ✅ Escalonamento de threads e gerenciamento de contexto.
- ✅ HLE básico (stubs de I/O, GPU, syscalls).

**Problema em investigação:**
- 🔴 **PC corrompido (0xAFB2001C):** Código ainda pula para endereço inválido. Possíveis causas:
  - GP relocalizado incorretamente (valor forçado `0x08B533B0` pode estar errado).
  - Leitura de ponteiro de função corrompida via `gp+offset`.
  - Segmentos ainda não alinhados corretamente na RAM.

**Últimas correções (Ciclo atual):**
- Loader agora lê `load_base` do JSON e o adiciona ao `p_vaddr` do ELF.
- Dispatcher normaliza PC com `CLEAN_ADDR()` antes de lookup.
- EventFlag bypass removido; agora segue lógica real do scheduler.

---

## 📝 Próximas Ações Prioritárias

### 1️⃣ Debug imediato do PC corrompido
   - Usar GDB para inspeccionar memória ao redor de `gp`.
   - Verificar se segmentos estão em lugar correto após `mem_load_segment()`.

### 2️⃣ Validar GP real
   - Extrair símbolo `_gp` ou `__gnu_local_gp` do arquivo `.sym`.
   - Comparar com valor forçado `0x08B533B0`.

### 3️⃣ Trace de leitura de ponteiro
   - Adicionar log em `psp_lwl()` / `psp_lwr()` quando `gp+offset` é acessado.

### 4️⃣ Testes de EventFlag
   - Verificar se remoção do bypass quebrou scheduler.
   - Adicionar logs em `scheduler_wait_evflag()`.

---

## 📂 Estrutura do Projeto

```
RecompProject/
├── runtime/
│   ├── loader.c          ← Carregador de ELF (com relocalização load_base)
│   ├── dispatcher.c      ← Dispatcher de blocos MIPS (normaliza PC)
│   ├── memory.c/h        ← Sistema MMU (CLEAN_ADDR, psp_translate_addr)
│   ├── cpu.h             ← Estrutura MIPS_CPU
│   ├── scheduler.c/h     ← Escalonador de threads
│   ├── syscalls.c        ← Stubs de HLE (sem bypass de EventFlag)
│   └── ...
├── out/                  ← Blocos MIPS recompilados
├── GOW_DATA/             ← Dados do jogo (assets)
├── parse_elf.py          ← Script de extração ELF
└── README.md             ← Este arquivo
```

---

## Aviso Legal

Este projeto é apenas para fins educacionais e de preservação.
Requer que você possua uma cópia legítima de *God of War: Chains of Olympus* (PSP).
