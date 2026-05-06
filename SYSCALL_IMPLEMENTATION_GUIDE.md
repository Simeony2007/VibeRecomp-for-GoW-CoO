# Guia de Implementação de Syscalls para PSP Recomp

## Status Atual
✅ **Análise Completa**: 546 syscalls únicos identificados  
✅ **Priorização**: Top 9 syscalls = 65.2% das chamadas  
✅ **Stub Implementado**: Todas as syscalls críticas retornam sucesso (0)  
⏳ **Próximo Passo**: Implementações funcionais baseadas em hardware alvo

---

## Camadas de Prioridade

### 🔴 CRÍTICA - Top 3 (14.5% das chamadas)
Implementação **OBRIGATÓRIA** para funcionamento básico.

| Código | Frequência | Tipo | Ação Recomendada |
|--------|-----------|------|------------------|
| **0x00D59** | 131x | Memory/Reg | ✅ Stub (retorna 0) - OK por agora |
| **0x00D4F** | 108x | Memory/Reg | ✅ Stub (retorna 0) - OK por agora |
| **0x00D48** | 103x | Memory/Reg | ✅ Stub (retorna 0) - OK por agora |

**Análise**: Estes códigos sequenciais sugerem uma pipeline de 3 estágios. Padrão típico:
```
Stage 1 (0x00D48) → Stage 2 (0x00D4F) → Stage 3 (0x00D59)
```

Possibilidades:
- Fence de memória / sincronização
- Operações de cache (L1/L2)
- Requisições de acesso a memória compartilhada

---

### 🟡 ALTA PRIORIDADE - Gráficos (81 calls = 3.4%)

| Código | Frequência | Nome |
|--------|-----------|------|
| **0x09F24** | 25x | Graphics Stage 3 (MAIS CRÍTICA) |
| **0x09F25** | 20x | Graphics Stage 4 |
| **0x09F23** | 18x | Graphics Stage 2 |
| **0x09F22** | 18x | Graphics Stage 1 |

**Implementação Sugerida** (com SDL2/OpenGL ES):

```c
case 0x09F22:  // Setup
    // Inicializa render target
    gpu_setup();
    cpu->v0 = 0;
    return;

case 0x09F23:  // Load vertices
    // Carrega dados de vértices da memória
    uint32_t vaddr = cpu->a0;
    uint32_t count = cpu->a1;
    gpu_load_vertices(mem, vaddr, count);
    cpu->v0 = 0;
    return;

case 0x09F24:  // Render
    // Executa comando de renderização
    gpu_render();
    cpu->v0 = 0;
    return;

case 0x09F25:  // Finalize
    // Apresenta frame buffer
    gpu_present();
    cpu->v0 = 0;
    return;
```

---

### 🟠 MÉDIA PRIORIDADE - Computação (41 calls = 1.74%)

| Código | Frequência | Tipo |
|--------|-----------|------|
| **0x02EC6** | 41x | Compute/Math (OUTLIER) |

**Análise**: Um único código com 41 chamadas é atípico. Possibilidades:
- Rotina de FFT para áudio
- SIMD (vetorização)
- Operação matemática pesada (sin, cos, sqrt em batch)

**Implementação Sugerida**:
```c
case 0x02EC6:  // Math/Compute Batch
    // a0 = endereço dos dados entrada
    // a1 = quantidade de elementos
    // a2 = tipo de operação (bitmap de flags)
    
    uint32_t input_addr = cpu->a0;
    uint32_t count = cpu->a1;
    uint32_t op_flags = cpu->a2;
    
    // Processar em batch (implementar conforme necessário)
    cpu->v0 = compute_batch(mem, input_addr, count, op_flags);
    return;
```

---

### ⚪ BAIXA PRIORIDADE - Casos Especiais (460 single-occurrence = 19.5%)

Estes syscalls aparecem apenas **1 vez** no código gerado. Provavelmente:
- Caminhos de erro/exceção
- Inicialização/cleanup
- Funções de debug

**Estratégia**: Deixar como stubs que retornam sucesso por enquanto.

---

## Implementação Prática

### Estrutura Base (Já Implementada)

```c
void psp_syscall(MIPS_CPU *cpu, uint8_t *mem, uint32_t code) {
    switch (code) {
        case 0x00D59:  // Top 1
            cpu->v0 = 0;
            return;
        // ... outros casos
        default:
            cpu->v0 = 0;  // Sucesso padrão
            break;
    }
}
```

### Macros Úteis para Passar Argumentos

```c
// Leitura de argumentos do contexto da CPU
#define ARG0  (cpu->a0)   // Primeiro argumento
#define ARG1  (cpu->a1)   // Segundo argumento
#define ARG2  (cpu->a2)   // Terceiro argumento
#define ARG3  (cpu->a3)   // Quarto argumento

// Retorno
#define RETURN(x)  do { cpu->v0 = (x); return; } while(0)

// Leitura de memória
#define MEM_READ32(addr)  MEM_R32(mem, addr)
#define MEM_READ16(addr)  MEM_R16(mem, addr)
#define MEM_READ8(addr)   MEM_R8(mem, addr)
```

### Exemplo: Syscall de Display (Comum em PSP)

```c
case 0x6001:  // sceDisplaySetFrameBuf
{
    // a0 = struct SceDisplayFrameBuf*
    // a1 = sync_type
    uint32_t fbuf_addr = ARG0;
    uint32_t sync_type = ARG1;
    
    // Parsear struct na memória
    struct {
        uint32_t topaddr;    // offset 0
        uint32_t bufferwidth; // offset 4
        uint32_t pixelformat; // offset 8
    } fbuf;
    
    fbuf.topaddr = MEM_READ32(fbuf_addr + 0);
    fbuf.bufferwidth = MEM_READ32(fbuf_addr + 4);
    fbuf.pixelformat = MEM_READ32(fbuf_addr + 8);
    
    fprintf(stderr, "[FB] addr=0x%08X w=%d fmt=%d\n",
            fbuf.topaddr, fbuf.bufferwidth, fbuf.pixelformat);
    
    RETURN(0);
}
```

---

## Padrão de Testagem

### Checklist para Cada Syscall Implementada

- [ ] Syscall é chamada com argumentos válidos
- [ ] Arquivo de LOG mostra a chamada (`[SYSCALL] code=0x...`)
- [ ] CPU->v0 retorna valor esperado
- [ ] Não há crash ou seg fault
- [ ] Performance aceitável (< 1ms por call em média)

---

## Syscalls Candidatos para Próxima Fase

Baseado na frequência, considere implementar em ordem:

1. **0x09F24** - Graphics (25 calls) → Essencial para renderização
2. **0x09F25** - Graphics (20 calls) → Essencial para display
3. **0x02EC6** - Compute (41 calls) → Pode afetar FPS
4. **0x09F23, 0x09F22** - Graphics (18 calls cada) → Suporte de pipeline

---

## Recursos Adicionais

### Debugging
Para ver cada syscall sendo executado:
```bash
./gow_recomp_arm64 2>&1 | grep SYSCALL | head -100
```

### Profiling
Para identificar syscalls lentos:
```bash
time ./gow_recomp_arm64
```

### Referências PSP
- UoFW Documentation: https://uofw.github.io/upspd/docs/software/PSP_OS/
- PSP Toolchain: https://github.com/pspdev/psptoolchain
- Insomniac PSP Dev Kit: Documentação proprietária (se disponível)

---

## Status de Implementação

| Descrição | Status | Data |
|-----------|--------|------|
| Análise de syscalls | ✅ Completo | 2026-05-06 |
| Stubs implementados | ✅ Completo | 2026-05-06 |
| Top 3 Memory syscalls | ✅ Stub | 2026-05-06 |
| Graphics pipeline | ⏳ Stub | - |
| Compute batch | ⏳ Stub | - |
| I/O e threads | ⏳ Não iniciado | - |

---

## Próximos Passos

1. **Compilar e testar** com os stubs atuais
2. **Executar** o recompilador e capturar logs
3. **Identificar** quais syscalls precisam ser implementadas primeiro
4. **Implementar** syscalls de renderização (9F22-9F25) para ter saída visual
5. **Otimizar** performance conforme necessário

