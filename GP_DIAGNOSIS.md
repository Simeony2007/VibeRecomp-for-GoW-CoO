# 🔴 Diagnóstico: Por que o PC pula para 0xAFB2001C

## O Problema Completo

### Sintomas (do LOG)
```
[TRACE] Executando bloco: 0x0881E444 | RA: 0x0881E47C
[TRAP FATAL] O Kratos tentou pular para o lixo (PC = 0xAFB2001C)!

Registradores:
  t9 = 0x00001234    ← Endereço inválido (provavelmente lido de memória)
  gp = 0x08B533B0    ← Global Pointer
```

### Raiz Causada Identificada

1. **O bloco 0x0881E444 contém algo como:**
   ```mips
   lw t9, offset(gp)   # Lê um endereço de função de memória
   jalr t9             # Pula para esse endereço
   ```

2. **O problema:** Quando `lw t9, offset(gp)` é executado, ele calcula o endereço como `gp + offset`.
   - Se `gp` estiver incorreto, o código lerá lixo em vez de um endereço de função válido.
   - O valor lido foi `0x00001234` (endereço inválido/lixo).
   - O `jalr t9` pula para `0x00001234`, que depois de normalização fica `0xAFB2001C` ou similar.

3. **A causa raiz: GP desalinhado**
   ```
   elf_meta.json diz:   gp_value = 0x08800000  (base de código)
   Código original força: gp = 0x08B533B0      (verdadeiro GP)
   
   Diferença: 0x353B0 = 3,501,488 bytes!
   ```

   Com a correção do loader que **remove o bypass**, o código agora usa:
   ```c
   global_gp = read_json_field("elf_meta.json", "\"gp_value\"");
   // Resultado: global_gp = 0x08800000  ← ERRADO!
   ```

   Então **TODOS os acessos `lw/sw` via `gp+offset` estão 3.5 MB desalinhados**.

---

## Por que o JSON está errado?

O arquivo `elf_meta.json` é gerado por `parse_elf.py`. Analisando o código:

**Possível erro em parse_elf.py:**
```python
# Provavelmente está fazendo:
gp_value = load_base  # ❌ ERRADO! Confundindo GP com base de código
```

**O que deveria fazer:**
```python
# Procurar o símbolo _gp ou __gnu_local_gp na tabela de símbolos ELF
# ou usar a seção .reginfo se disponível
```

---

## A Solução (3 passos)

### 1️⃣ Extrair o GP Correto

Executar o script `extract_gp.py`:
```bash
python3 extract_gp.py
```

Isso vai:
- Procurar pelo símbolo `_gp` ou `__gnu_local_gp` na tabela de símbolos do ELF
- Atualizar `elf_meta.json` com o valor correto
- Imprimir o GP antes/depois

### 2️⃣ Ou corrigir `parse_elf.py` manualmente

Procurar em `parse_elf.py` a linha que seta `gp_value` e mudá-la para extrair o símbolo real.

### 3️⃣ Compilar e testar novamente

```bash
make clean
make -B
./main > LOG.txt 2>&1
```

---

## Por que remover o bypass foi errado?

O bypass original:
```c
if (global_gp == 0 || global_gp == 0x08800000) {
    global_gp = 0x08B533B0;  // Forçar o GP correto (hotfix)
}
```

Esse era um **hotfix que mascarava o verdadeiro problema**: o `parse_elf.py` está extraindo o GP errado.

**Agora que removemos o bypass:**
- Se o JSON estiver errado, o código usa o valor errado e falha.
- Isso é melhor! Forçam a gente a consertar a raiz do problema.
- Assim, sabemos com certeza que o problema é o JSON, não o loader.

---

## Próximos Passos

1. **Executar `extract_gp.py`** para obter o GP real
2. **Verificar se `elf_meta.json` foi atualizado** com o valor correto
3. **Recompilar e testar**
4. **Se ainda falhar com GP correto:** o problema é mais profundo (relocação PRX, seção de dados danificada, etc.)

---

## Resumo

| Componente | Valor Errado | Valor Correto | Status |
|-----------|--------------|---------------|--------|
| **load_base** | 0x00000000 | 0x08800000 | ✅ Corrigido |
| **Segmento 1** | mem[0] | mem[0x08800000 - 0x08800000] | ✅ Corrigido |
| **Segmento 2** | mem[0x34B3C0] | mem[0x0034B3C0] | ✅ Corrigido |
| **GP (Global Pointer)** | 0x08800000 ❌ | 0x08B533B0 ❌ | 🔴 **PRECISA CORRIGIR** |
| **PC inválido** | jalr de 0x00001234 | - | 🔴 **Resultado do GP errado** |
