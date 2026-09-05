#!/bin/bash
# Script de compilação inteligente com Caching e Otimização Seletiva (Versão v15)
# Ativa o scanner heurístico de SceModuleInfo corrigido e resolve todos os imports do God of War.

echo "Compilando o emulador God of War RK3326/R36S GLES 3.2 (Versão v15)..."
echo "=========================================================================="

# 1. AUTO-FIX: Renomeia qualquer arquivo .txt baixado do Notebook de volta para C/H se necessário
for f in *.txt; do
    if [ -f "$f" ]; then
        basename="${f%.txt}"
        if [ ! -f "$basename" ]; then
            echo "[Auto-Fix] Renomeando $f -> $basename"
            mv "$f" "$basename"
        fi
    fi
done

# Garante que a pasta de output existe
mkdir -p ./output

# 2. Seleciona a melhor Main disponível (prioriza main_v12.c)
MAIN_SRC="main_v2.c"
if [ -f "main_v12.c" ]; then
    MAIN_SRC="main_v12.c"
elif [ -f "main_v11.c" ]; then
    MAIN_SRC="main_v11.c"
elif [ -f "main_v10.c" ]; then
    MAIN_SRC="main_v10.c"
elif [ -f "main_v9.c" ]; then
    MAIN_SRC="main_v9.c"
elif [ -f "main_v8.c" ]; then
    MAIN_SRC="main_v8.c"
elif [ -f "main_v7.c" ]; then
    MAIN_SRC="main_v7.c"
elif [ -f "main_v6.c" ]; then
    MAIN_SRC="main_v6.c"
elif [ -f "main_v5.c" ]; then
    MAIN_SRC="main_v5.c"
elif [ -f "main_v4.c" ]; then
    MAIN_SRC="main_v4.c"
elif [ -f "main_v3.c" ]; then
    MAIN_SRC="main_v3.c"
fi
echo "[Build] Usando arquivo principal: $MAIN_SRC"

# 3. Compilação inteligente do gow_core_aot.c (se existir)
AOT_OBJ=""
if [ -f "gow_core_aot.c" ]; then
    echo "[AOT] gow_core_aot.c detectado!"
    
    # Verifica se o objeto precisa ser recompilado (se não existe ou se o .c é mais recente)
    if [ ! -f "./output/gow_core_aot.o" ] || [ "gow_core_aot.c" -nt "./output/gow_core_aot.o" ]; then
        echo "[AOT] Compilando arquivo gigante gow_core_aot.c (isso pode levar de 30 a 60 segundos na primeira vez)..."
        gcc -c -O0 -w -fno-strict-aliasing gow_core_aot.c -o ./output/gow_core_aot.o
        if [ $? -ne 0 ]; then
            echo "[AOT ERROR] Falha crítica na compilação do gow_core_aot.c."
            exit 1
        fi
        echo "[AOT] gow_core_aot.o gerado com sucesso!"
    else
        echo "[AOT] Usando gow_core_aot.o pré-compilado existente (Compilação ultra-rápida ativa!)."
    fi
    AOT_OBJ="./output/gow_core_aot.o"
fi

# 4. Compila o restante do núcleo com -O2 (otimização máxima para performance) e linka com o objeto AOT
echo "[Build] Compilando e linkando o núcleo do emulador com otimização -O2..."
gcc -Wall -Wextra -O2 -fno-strict-aliasing \
    $MAIN_SRC \
    mips_cpu.c \
    mips_dispatcher.c \
    mips_interpreter.c \
    mips_hle.c \
    mips_ge.c \
    mips_video.c \
    mips_audio.c \
    $AOT_OBJ \
    -lSDL2 -lGLESv2 -lm \
    -o ./output/main

if [ $? -eq 0 ]; then
    echo "====================================================="
    echo "Compilação bem-sucedida! Executável gerado em: ./output/main"
    echo "====================================================="
else
    echo "Erro na compilação. Verifique os logs acima."
fi
