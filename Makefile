# ─────────────────────────────────────────────────────────────────────────────
# Makefile — PSP Recomp: God of War Chains of Olympus → ARM64
#
# COMO USAR:
#
#   Passo 1 — Instalar o compilador cross (no PC Linux/Windows WSL):
#       sudo apt install gcc-aarch64-linux-gnu
#
#   Passo 2 — Rodar o pipeline Python (gera os arquivos C):
#       python parse_elf.py EBOOT.BIN
#       python disasm_to_c.py EBOOT.BIN
#
#   Passo 3 — Compilar:
#       make          → compila para ARM64 (R36S)
#       make native   → compila para x86_64 (testar no PC)
#       make clean    → limpa os .o e o binário
#
#   Passo 4 — Copiar para o R36S:
#       scp gow_recomp user@<IP_DO_R36S>:/home/user/
# ─────────────────────────────────────────────────────────────────────────────

# ── Configuração do compilador ─────────────────────────────────────────────
CC_ARM64  := aarch64-linux-gnu-gcc
CC_NATIVE := gcc

TARGET    := gow_recomp
BUILD_DIR := build

# ── Flags de compilação ────────────────────────────────────────────────────
CFLAGS_COMMON := \
    -std=c11 \
    -O2 \
    -Wall \
    -Wno-unused-label \
    -Wno-unused-variable \
    -I. \
    -Iruntime \
    -Iout

# Flags específicas ARM64 para o R36S (Rockchip RK3326, Cortex-A35)
CFLAGS_ARM64 := $(CFLAGS_COMMON) \
    -march=armv8-a \
    -mtune=cortex-a35 \
    -fomit-frame-pointer \
    -ffunction-sections \
    -fdata-sections

LDFLAGS_ARM64 := -Wl,--gc-sections -lm

# ── Fontes ─────────────────────────────────────────────────────────────────
RUNTIME_SRCS := \
    runtime/memory.c \
    runtime/syscalls.c \
    runtime/loader.c

OUT_SRCS     := $(wildcard out/funcs/*.c) out/func_table.c

ALL_SRCS     := $(RUNTIME_SRCS) $(OUT_SRCS)

# Objetos ARM64
OBJS_ARM64   := $(patsubst %.c,$(BUILD_DIR)/arm64/%.o,$(ALL_SRCS))

# Objetos native
OBJS_NATIVE  := $(patsubst %.c,$(BUILD_DIR)/native/%.o,$(ALL_SRCS))

# ── Targets ────────────────────────────────────────────────────────────────
.PHONY: all native clean dirs check_tools

all: check_tools dirs $(TARGET)_arm64

native: dirs $(TARGET)_native

# ── Verifica se as ferramentas estão instaladas ────────────────────────────
check_tools:
	@command -v $(CC_ARM64) >/dev/null 2>&1 || { \
		echo ""; \
		echo "ERRO: $(CC_ARM64) não encontrado!"; \
		echo "Instale com:  sudo apt install gcc-aarch64-linux-gnu"; \
		echo ""; \
		exit 1; \
	}
	@echo "[OK] Compilador ARM64 encontrado: $(CC_ARM64)"
	@test -d out/funcs || { \
		echo ""; \
		echo "ERRO: Pasta out/funcs/ não existe!"; \
		echo "Rode primeiro:"; \
		echo "  python parse_elf.py EBOOT.BIN"; \
		echo "  python disasm_to_c.py EBOOT.BIN"; \
		echo ""; \
		exit 1; \
	}
	@echo "[OK] Arquivos gerados encontrados"

# ── Cria diretórios de build ───────────────────────────────────────────────
dirs:
	@mkdir -p $(BUILD_DIR)/arm64/runtime
	@mkdir -p $(BUILD_DIR)/arm64/out/funcs
	@mkdir -p $(BUILD_DIR)/native/runtime
	@mkdir -p $(BUILD_DIR)/native/out/funcs

# ── Compilação ARM64 ───────────────────────────────────────────────────────
$(BUILD_DIR)/arm64/%.o: %.c
	@echo "  [ARM64] $<"
	@$(CC_ARM64) $(CFLAGS_ARM64) -c $< -o $@

$(TARGET)_arm64: $(OBJS_ARM64)
	@echo "[LINK ARM64] $(TARGET)_arm64"
	@$(CC_ARM64) $(CFLAGS_ARM64) $(OBJS_ARM64) -o $@ $(LDFLAGS_ARM64)
	@echo ""
	@echo "=== Build ARM64 concluído: $(TARGET)_arm64 ==="
	@echo "Copie para o R36S com:"
	@echo "  scp $(TARGET)_arm64 user@<IP_R36S>:/home/user/$(TARGET)"

# ── Compilação nativa (x86_64 para teste no PC) ────────────────────────────
$(BUILD_DIR)/native/%.o: %.c
	@echo "  [native] $<"
	@$(CC_NATIVE) $(CFLAGS_COMMON) -c $< -o $@

$(TARGET)_native: $(OBJS_NATIVE)
	@echo "[LINK native] $(TARGET)_native"
	@$(CC_NATIVE) $(CFLAGS_COMMON) $(OBJS_NATIVE) -o $@ -lm
	@echo "=== Build nativo concluído: $(TARGET)_native ==="

# ── Limpeza ────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR) $(TARGET)_arm64 $(TARGET)_native
	@echo "Limpo."

# ── Estatísticas ───────────────────────────────────────────────────────────
stats:
	@echo "Funções geradas: $$(ls out/funcs/*.c 2>/dev/null | wc -l)"
	@echo "Linhas de C geradas: $$(cat out/funcs/*.c 2>/dev/null | wc -l)"
	@echo "Tamanho total: $$(du -sh out/funcs/ 2>/dev/null | cut -f1)"
