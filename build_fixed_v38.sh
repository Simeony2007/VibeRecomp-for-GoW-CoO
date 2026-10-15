#!/bin/bash
# =============================================================================
# Script de Compilação Inteligente com Cache AOT (build_fixed_v26.sh)
# Emulador Híbrido MIPS Allegrex & GLES 3.2 (God of War - RK3326/R36S)
# Support for Multi-Module Linking, Safe Memory Isolation, and Debug Mode (main_v18.c)
# =============================================================================

# Cria o diretório de saída caso não exista
mkdir -p ./output

# Configurações de cores para o terminal
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}======================================================================${NC}"
echo -e "${YELLOW}[Build] Iniciando processo de compilação inteligente modularizada (V35)...${NC}"
echo -e "${BLUE}======================================================================${NC}"

# Detecta modo de depuração (Pure Interpreter)
INTERPRETER_MODE=false
FORCE_FLAG=""
for arg in "$@"; do
    if [ "$arg" == "--interpreter" ] || [ "$arg" == "-i" ]; then
        INTERPRETER_MODE=true
        FORCE_FLAG="-DFORCE_INTERPRETER"
    fi
done

# 1. Identifica o arquivo principal (main) disponível no diretório
# PRIORIDADE: main_v35.c -> main_v34.c -> main_v32.c -> main_v30.c -> main_v28.c -> main_v27.c -> main_v26.c -> main_v25.c -> main_v24.c -> main_v23.c -> main_v22.c -> main.c
MAIN_FILE=""
if [ -f "main_v35.c" ]; then
    MAIN_FILE="main_v35.c"
elif [ -f "main_v34.c" ]; then
    MAIN_FILE="main_v34.c"
elif [ -f "main_v33.c" ]; then
    MAIN_FILE="main_v33.c"
elif [ -f "main_v32.c" ]; then
    MAIN_FILE="main_v32.c"
elif [ -f "main_v30.c" ]; then
    MAIN_FILE="main_v30.c"
elif [ -f "main_v28.c" ]; then
    MAIN_FILE="main_v28.c"
elif [ -f "main_v27.c" ]; then
    MAIN_FILE="main_v27.c"
elif [ -f "main_v26.c" ]; then
    MAIN_FILE="main_v26.c"
elif [ -f "main_v25.c" ]; then
    MAIN_FILE="main_v25.c"
elif [ -f "main_v24.c" ]; then
    MAIN_FILE="main_v24.c"
elif [ -f "main_v23.c" ]; then
    MAIN_FILE="main_v23.c"
elif [ -f "main_v22.c" ]; then
    MAIN_FILE="main_v22.c"
elif [ -f "main_v20.c" ]; then
    MAIN_FILE="main_v20.c"
elif [ -f "main_v19.c" ]; then
    MAIN_FILE="main_v19.c"
elif [ -f "main_v18.c" ]; then
    MAIN_FILE="main_v18.c"
elif [ -f "main.c" ]; then
    MAIN_FILE="main.c"
fi
    # Busca heurística por qualquer arquivo que contenha 'int main'
    # Exclui patches isolados para evitar falsos positivos de linkagem

if [ -z "$MAIN_FILE" ] || [ ! -f "$MAIN_FILE" ]; then
    echo -e "${RED}[Build Error] Nenhum arquivo principal unificado de emulador (*.c) foi encontrado!${NC}"
    echo -e "${RED}[Build Error] Certifique-se de que o arquivo 'main_v28.c' completo está na pasta.${NC}"
    exit 1
fi

echo -e "${GREEN}[Build] Arquivo principal identificado para vinculação: '${MAIN_FILE}'${NC}"

# 2. Compilação inteligente do gow_core_aot.c com cache
AOT_OBJECTS=()
if [ "$INTERPRETER_MODE" == "true" ]; then
    echo -e "${YELLOW}[Build Info] Modo Pure Interpreter Ativo. Ignorando compilação do núcleo AOT...${NC}"
else
    if [ -f "gow_core_aot.c" ]; then
        echo -e "${YELLOW}[AOT] gow_core_aot.c detectado!${NC}"

        AOT_PART_DIR="./output/gow_core_aot_parts"
        AOT_PARTS=("$AOT_PART_DIR"/gow_core_aot_part_*.c)
        if [ -f "${AOT_PARTS[0]}" ]; then
            echo -e "${YELLOW}[AOT] Compilando shards AOT em unidades menores...${NC}"
            for AOT_SOURCE in "${AOT_PARTS[@]}"; do
                AOT_NAME="$(basename "$AOT_SOURCE" .c)"
                AOT_OBJECT="./output/${AOT_NAME}.o"
                if [ ! -f "$AOT_OBJECT" ] || [ "$AOT_SOURCE" -nt "$AOT_OBJECT" ] || \
                    [ "mips_cpu.h" -nt "$AOT_OBJECT" ] || [ "mips_dispatcher.h" -nt "$AOT_OBJECT" ]; then
                    echo -e "${YELLOW}[AOT] Compilando $AOT_NAME...${NC}"
                    gcc -c -O0 -w -fno-strict-aliasing -I. "$AOT_SOURCE" -o "$AOT_OBJECT"
                    if [ $? -ne 0 ]; then
                        echo -e "${RED}[AOT ERROR] Falha ao compilar $AOT_SOURCE.${NC}"
                        exit 1
                    fi
                fi
                AOT_OBJECTS+=("$AOT_OBJECT")
            done

            AOT_OBJECT="./output/gow_core_aot_registry.o"
            if [ ! -f "$AOT_OBJECT" ] || [ "gow_core_aot.c" -nt "$AOT_OBJECT" ] || \
                [ "mips_cpu.h" -nt "$AOT_OBJECT" ] || [ "mips_dispatcher.h" -nt "$AOT_OBJECT" ]; then
                echo -e "${YELLOW}[AOT] Compilando registro dos blocos...${NC}"
                gcc -c -O0 -w -fno-strict-aliasing gow_core_aot.c -o "$AOT_OBJECT"
                if [ $? -ne 0 ]; then
                    echo -e "${RED}[AOT ERROR] Falha ao compilar gow_core_aot.c.${NC}"
                    exit 1
                fi
            fi
            AOT_OBJECTS+=("$AOT_OBJECT")
        else
            echo -e "${YELLOW}[AOT] Shards ausentes; compilando o AOT monolítico legado...${NC}"
            AOT_OBJECT="./output/gow_core_aot.o"
            if [ ! -f "$AOT_OBJECT" ] || [ "gow_core_aot.c" -nt "$AOT_OBJECT" ] || \
                [ "mips_cpu.h" -nt "$AOT_OBJECT" ] || [ "mips_dispatcher.h" -nt "$AOT_OBJECT" ]; then
                gcc -c -O0 -w -fno-strict-aliasing gow_core_aot.c -o "$AOT_OBJECT"
                if [ $? -ne 0 ]; then
                    echo -e "${RED}[AOT ERROR] Falha crítica na compilação do gow_core_aot.c.${NC}"
                    exit 1
                fi
            fi
            AOT_OBJECTS+=("$AOT_OBJECT")
        fi
    else
        echo -e "${YELLOW}[Build Info] gow_core_aot.c não encontrado. Operando em modo 100% Interpretador Fallback.${NC}"
    fi
fi

# Detect linker file
LINKER_FILE="mips_linker.c"
if [ -f "mips_linker_v4.c" ]; then
    LINKER_FILE="mips_linker_v4.c"
fi

# 3. Compilação e Vinculação Unificada com Otimização -O2 para os módulos dinâmicos
if [ "$INTERPRETER_MODE" == "true" ]; then
    echo -e "${YELLOW}[Build] Compilando e vinculando o núcleo no modo de depuração INTERPRETADOR PURO...${NC}"
else
    echo -e "${YELLOW}[Build] Compilando e vinculando o restante do núcleo híbrido com otimização -O2...${NC}"
fi

    KIRK_DIR="./PPSSPP/ext/libkirk"
    KIRK_OBJECTS=()
    for KIRK_SOURCE in kirk_engine AES SHA1 bn ec amctrl; do
        KIRK_OBJECT="./output/prx_${KIRK_SOURCE}.o"
        gcc -std=c11 -I"$KIRK_DIR" -c "$KIRK_DIR/$KIRK_SOURCE.c" -o "$KIRK_OBJECT"
        if [ $? -ne 0 ]; then
            echo -e "${RED}[Build Error] Falha ao compilar libkirk/$KIRK_SOURCE.c.${NC}"
            exit 1
        fi
        KIRK_OBJECTS+=("$KIRK_OBJECT")
    done

    g++ -std=c++17 -I./PPSSPP -I"$KIRK_DIR" -c \
        PPSSPP/Core/ELF/PrxDecrypter.cpp -o ./output/prx_decrypter.o
    if [ $? -ne 0 ]; then
        echo -e "${RED}[Build Error] Falha ao compilar o decriptador PRX do PPSSPP.${NC}"
        exit 1
    fi

    g++ -std=c++17 -I. -I./PPSSPP -I"$KIRK_DIR" -c \
        mips_prx_runtime.cpp -o ./output/mips_prx_runtime.o
    if [ $? -ne 0 ]; then
        echo -e "${RED}[Build Error] Falha ao compilar a ponte PRX/gzip do core.${NC}"
        exit 1
    fi

    gcc -Wall -Wextra -O2 -fno-strict-aliasing $FORCE_FLAG \
    "$MAIN_FILE" \
    $LINKER_FILE \
    mips_cpu.c \
    mips_dispatcher.c \
    mips_interpreter.c \
    mips_hle.c \
    mips_sas.c \
    mips_ge.c \
    mips_video.c \
    mips_audio.c \
    mips_reloc.c \
    mips_prx.c \
    sceKernelThread.c \
    sceSysMem.c \
    "${KIRK_OBJECTS[@]}" \
    ./output/prx_decrypter.o \
    ./output/mips_prx_runtime.o \
    "${AOT_OBJECTS[@]}" \
    -lSDL2 -lGLESv2 -lm -lpthread -lstdc++ -lz \
    -o ./output/main

if [ $? -eq 0 ]; then
    echo -e "${GREEN}======================================================================${NC}"
    if [ "$INTERPRETER_MODE" == "true" ]; then
        echo -e "${GREEN}[BUILD SUCCESS] Emulador compilado com sucesso no modo INTERPRETADOR PURO!${NC}"
        echo -e "${GREEN}[BUILD SUCCESS] Execute com './output/main' para obter o Heartbeat e diagnosticar o local de travamento.${NC}"
    else
        echo -e "${GREEN}[BUILD SUCCESS] Emulador híbrido unificado compilado e vinculado com sucesso!${NC}"
        echo -e "${GREEN}[BUILD SUCCESS] Executável gerado em: ./output/main${NC}"
    fi
    echo -e "${GREEN}======================================================================${NC}"
    exit 0
else
    echo -e "${RED}======================================================================${NC}"
    echo -e "${RED}[BUILD ERROR] Erro na compilação do núcleo. Verifique as mensagens acima.${NC}"
    echo -e "${RED}======================================================================${NC}"
    exit 1
fi
