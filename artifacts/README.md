# Artefatos de diagnostico

- `runs/`: traces, logs do console e logs de comparacao do PPSSPP.
- `reports/`: relatorios gerados a partir do EBOOT e de outras ferramentas.

Os fontes do core, scripts e testes permanecem em seus caminhos atuais para
preservar os comandos de build e os caminhos usados pelo jogo.

Novas execucoes podem gravar os resultados em `artifacts/runs/`, por exemplo:

```bash
MIPS_TRACE_FILE=artifacts/runs/Run76.trace ./output/main \
  > artifacts/runs/Run76.console.log 2>&1
```
