# PSP Recomp — God of War: Chains of Olympus → R36S (DESCRIÇÃO DE PROJETO DESATUALIZADA)
## Guia passo a passo para iniciantes

---

## O que é isso?

Este projeto converte o código MIPS do PSP em código C portável,
que pode ser compilado nativamente para o ARM64 do R36S —
sem precisar de emulador.

```
EBOOT.BIN (MIPS)  →  [pipeline Python]  →  C  →  [gcc ARM64]  →  binário nativo R36S
```

---

## Estrutura de arquivos

```
psp_recomp/
├── parse_elf.py        ← Passo 1: lê o ELF e gera elf_meta.json
├── disasm_to_c.py      ← Passo 2: traduz MIPS → C
├── Makefile            ← Passo 3: compila para ARM64
├── runtime/
│   ├── cpu.h           ← Define a struct da CPU MIPS
│   ├── memory.h/.c     ← Gerencia a memória emulada
│   ├── syscalls.c      ← Stubs das syscalls do PSP (implemente aqui)
│   └── loader.c        ← Carrega o ELF e inicia a execução
└── out/                ← Gerado automaticamente pelo pipeline Python
    ├── funcs/
    │   ├── func_08900000.c   ← Uma função MIPS por arquivo
    │   ├── func_08900200.c
    │   └── ...
    ├── func_table.c
    └── func_table.h
```

---

## Pré-requisitos

### No PC (Windows)
1. Instale o **Python 3.10+**: https://www.python.org/downloads/
2. Instale o **WSL2** (Windows Subsystem for Linux):
   - Abra o PowerShell como Administrador e rode:
     ```
     wsl --install
     ```
   - Reinicie o PC após a instalação

### No WSL2 / PC Linux
```bash
# Instala o compilador cross para ARM64
sudo apt update
sudo apt install gcc-aarch64-linux-gnu make python3
```

---

## Passo a passo

### 1. Copie o EBOOT.BIN descriptado para esta pasta

O EBOOT.BIN precisa estar **descriptado** (sem criptografia da Sony).
Use o PRXDecrypter se necessário.

Coloque o arquivo aqui:
```
psp_recomp/EBOOT.BIN
```

### 2. Abra um terminal nesta pasta

No Windows, clique com botão direito na pasta → "Abrir no Terminal"
Ou use o WSL2:
```bash
cd /mnt/c/Users/SEU_USUARIO/psp_recomp
```

### 3. Rode o parser ELF

```bash
python parse_elf.py EBOOT.BIN
```

Saída esperada:
```
============================================================
  Arquivo : EBOOT.BIN
  Tipo    : ET_SCE_EXEC (PRX executável Sony)
  Entry   : 0x08900000          ← ANOTE ESTE ENDEREÇO
  Segmentos (2):
    ELFSegment(vaddr=0x08900000, memsz=0x1A0000, flags=RX)
    ELFSegment(vaddr=0x08AA0000, memsz=0x80000,  flags=RW)
============================================================
[parse_elf] Metadados exportados: elf_meta.json
```

**IMPORTANTE:** Anote o endereço do **Entry** (ex: `0x08900000`).

### 4. Rode o tradutor MIPS → C

```bash
python disasm_to_c.py EBOOT.BIN
```

Isso vai gerar centenas (ou milhares) de arquivos .c na pasta `out/funcs/`.
Pode levar alguns minutos dependendo do tamanho do jogo.

Saída esperada:
```
[disasm] Passe 1: descobrindo funções...
[disasm] 1247 funções encontradas.
[disasm] Traduzindo funções: 1200/1247...
[disasm] 1247 funções traduzidas.
[disasm] Arquivos gerados em: out/
```

### 5. Configure o entry point no loader

Abra o arquivo `runtime/loader.c` em um editor de texto.

Procure esta linha perto do final:
```c
/* TODO: substitua pelo endereço real do entry */
/* func_ENTRY_ADDR(&cpu, mem); */
```

Substitua pelo endereço que você anotou no passo 3.
Exemplo (se o Entry foi `0x08900000`):
```c
func_08900000(&cpu, mem);
```

Salve o arquivo.

### 6. Compile para ARM64

No terminal WSL2 ou Linux:
```bash
make
```

Se tudo der certo:
```
=== Build ARM64 concluído: gow_recomp_arm64 ===
Copie para o R36S com:
  scp gow_recomp_arm64 user@<IP_R36S>:/home/user/gow_recomp
```

### 7. Copie para o R36S e teste

Conecte o R36S na mesma rede Wi-Fi.
No terminal Linux:
```bash
scp gow_recomp_arm64 user@192.168.X.X:/home/user/gow_recomp
```

No R36S (via SSH ou terminal):
```bash
chmod +x gow_recomp
./gow_recomp EBOOT.BIN
```

---

## O que esperar neste estágio

Na primeira execução, você vai ver muitos logs de syscalls não implementadas:
```
[SYSCALL] code=0x06000 (sceDisplaySetMode) a0=0x00000001 ...
[SYSCALL] code=0x05002 (sceCtrlSetSamplingMode) a0=0x00000001 ...
```

Isso é **esperado e normal**. Significa que a CPU está executando corretamente,
mas as funções do hardware do PSP ainda precisam ser reimplementadas.

O próximo passo é implementar as syscalls em `runtime/syscalls.c`
usando SDL2 para vídeo/áudio/input no R36S.

---

## Problemas comuns

### "Magic ELF inválido"
O EBOOT.BIN ainda está criptografado. Use o PRXDecrypter:
https://github.com/John-K/prxdecrypter

### "Arquivo elf_meta.json não encontrado"
Rode o passo 3 antes do passo 4.

### "gcc-aarch64-linux-gnu: command not found"
```bash
sudo apt install gcc-aarch64-linux-gnu
```

### O binário trava ou dá segfault
Provável endereço de entry incorreto no loader.c.
Verifique o endereço exato no elf_meta.json:
```bash
cat elf_meta.json | grep entry
```

---

## Próximos passos (roadmap)

- [ ] Implementar `sceDisplaySetFrameBuf` → renderização via SDL2/DRM
- [ ] Implementar `sceCtrlReadBufferPositive` → input via gamepad Linux
- [ ] Implementar `sceAudioOutputBlocking` → áudio via ALSA/SDL2
- [ ] Implementar o dispatcher de JR indireto (funções virtuais)
- [ ] Mapear comandos GE (GPU) do PSP → OpenGL ES 2.0

---

## Aviso legal

Este projeto é apenas para fins educacionais e de preservação.
Requer que você possua uma cópia legítima do jogo.

TODOS OS CÓDIGOS SÃO GERADOS POR IA, EU APENAS ORGANIZO E COMANDO O PROCESSO DE CRIAÇÃO E DEBUGGING.
