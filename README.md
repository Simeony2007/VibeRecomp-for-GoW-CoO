# Emulador PSP Allegrex para God of War: Chains of Olympus

## Sobre o projeto

Este projeto e um emulador experimental de PSP, escrito principalmente em C,
com suporte auxiliar em C++ para componentes de PRX. O objetivo principal e
executar **God of War: Chains of Olympus** de forma nativa em um dispositivo
ARM64 de baixo custo, especialmente o R36S/RK3326, usando recompilacao estatica
(AOT) quando houver blocos traduzidos e mantendo um interpretador Allegrex como
fallback e ferramenta de diagnostico.

O projeto esta em desenvolvimento e ainda nao e um emulador PSP completo. O
estado observado no momento e que o jogo carrega, inicializa subsistemas,
submete listas ao Graphics Engine e chega a desenhar primitivas, mas a imagem
continua incorreta: na execucao mais recente, a tela permanece preta durante os
fades e termina no mesmo padrao visual de antes, incluindo a regiao preta no
canto.

### Autoria do codigo

Conforme declarado pelo responsavel pelo projeto, **todo o codigo de
implementacao deste projeto foi produzido por inteligencia artificial; nao
houve escrita manual de codigo**. Isso se refere ao codigo proprio do emulador,
aos scripts e aos testes deste projeto. O diretorio `PPSSPP/` contem codigo de
terceiros/upstream usado como referencia e/ou dependencia; esse codigo nao e
apresentado como autoria original deste projeto.

## Arquitetura

O fluxo principal e:

1. `main_v35.c` inicia o processo, carrega o jogo e conecta os subsistemas.
2. O estado `MIPS_CPU` representa registradores e memoria do PSP.
3. A execucao de codigo Allegrex e encaminhada ao dispatcher AOT quando ha um
   bloco traduzido registrado; caso contrario, pode seguir pelo interpretador.
4. Syscalls e modulos importados sao atendidos por implementacoes HLE.
5. As chamadas GE alimentam uma maquina de estado de display lists.
6. O renderer decodifica vertices, texturas e estado de framebuffer e desenha
   por SDL2/OpenGL ES.
7. A camada de audio recebe/mistura buffers PCM atraves de SDL2.

| Area | Arquivos principais | Responsabilidade |
| --- | --- | --- |
| Inicializacao e carregamento | `main_v35.c`, `mips_reloc.c`, `mips_prx.c`, `mips_prx_runtime.cpp` | Inicializacao, leitura do jogo, relocacoes de modulos e suporte a PRX. |
| Estado e memoria da CPU | `mips_cpu.h`, `mips_cpu.c` | Registradores Allegrex, RAM/VRAM/scratchpad, traducao de enderecos e instrumentacao de memoria. |
| Interpretador | `mips_interpreter.c`, `mips_interpreter.h` | Execucao de uma instrucao Allegrex por passo; inclui GPR, COP1 e VFPU em diferentes niveis de cobertura. |
| Recompilacao estatica/AOT | `aot_recompiler_v5.py`, `gow_core_aot.c`, `mips_dispatcher.c`, `mips_dispatcher.h` | Geracao/registro de blocos traduzidos e despacho para AOT ou fallback interpretado. O AOT atual e especifico ao executavel de GOW. |
| HLE e sistema PSP | `mips_hle.c`, `mips_hle.h`, `sceKernelThread.c`, `sceKernelThread.h`, `sceSysMem.c`, `sceSysMem.h` | Syscalls, I/O, threads, sincronizacao, memoria e servicos PSP implementados em software. |
| Graphics Engine | `mips_ge.c`, `mips_ge.h` | Registradores GE, comandos e fluxo de display lists, vertices, matrizes, framebuffer, texturas e estado CLUT. |
| Renderizacao | `mips_video.c`, `mips_video.h`, `mips_video_pixel.h`, `mips_video_texture.h` | SDL2/OpenGL ES 3.2, conversao de pixels, leitura/decodificacao de texturas e rasterizacao de primitivas. |
| Audio e SAS | `mips_audio.c`, `mips_audio.h`, `mips_sas.c` | Saida SDL2 e suporte parcial a canais/mixagem e audio SAS. |
| Testes | `tests/python/` | Testes de regressao de CPU, memoria, VFPU, GE e pipeline de video. |
| Artefatos | `artifacts/runs/`, `artifacts/reports/` | Logs, traces e relatorios usados nas investigacoes. |
| Referencia externa | `PPSSPP/` | Codigo upstream PPSSPP consultado para comparar formatos e semantica; nao confundir com o core proprio do projeto. |

## Estado tecnico consolidado

O core ja possui interpretador Allegrex parcial, blocos AOT especificos ao
GOW com fallback interpretado, HLE de I/O/threads/memoria e um caminho GE que
desenha primitivas e texturas via OpenGL ES. Isso ainda nao e compatibilidade
completa: ha imports em fallback, cobertura parcial de COP1/VFPU, SAS sem
sintese PSP completa, estados GE incompletos e divergencia entre draws GLES e
framebuffer guest. O GOW ainda nao foi confirmado como visualmente jogavel, e
os runs de Ozone/2048/Cave History devem ser lidos conforme suas limitacoes
documentadas.

O inventario detalhado de syscalls implementadas versus simplificadas/stub,
comandos GE, cobertura da CPU, ponte AOT e evidencias dos runs esta em
[General progress.md](./General%20progress.md). Para separar fatos medidos de
hipoteses, consulte tambem [tools/STATUS.md](./tools/STATUS.md).

### Estado da CPU e memoria

`MIPS_CPU` guarda os 32 registradores GPR, PC, HI/LO, FPR, FCR31, os 128
registradores VFPU, controles COP0, estado de excecao/trace e ponteiros para
RAM, VRAM e scratchpad. A memoria PSP e mapeada em buffers contiguos no host,
com helpers para traduzir enderecos virtuais PSP e registrar acessos relevantes.

O interpretador e importante tanto como fallback para blocos que ainda nao
foram traduzidos quanto como caminho de diagnostico: instrumentacoes locais
colocadas em `mips_interpreter_step` nao necessariamente executam quando o
despacho vai diretamente para um bloco AOT.

### Build

O script principal e `build_fixed_v38.sh`. O modo padrao tenta construir o
emulador hibrido; `--interpreter` forca o modo interpretador puro para
diagnostico:

```bash
./build_fixed_v38.sh --interpreter
```

O script espera GCC/G++, SDL2, OpenGL ES e as dependencias de sistema usadas
pela compilacao. Ele grava o executavel em `output/main`. Um build feito em
WSL/x86-64 e um executavel para o host WSL; **nao e automaticamente o binario
ARM64 que deve ser copiado para o R36S**. Para executar no aparelho e necessario
compilar no proprio aparelho ou usar um cross-toolchain ARM64 apropriado e
validar as bibliotecas/ABI do destino.

### Controles de teclado

O renderer atual traduz teclado para `sceCtrlReadBufferPositive` e
`sceCtrlPeekBufferPositive`: setas controlam o direcional, Z/X/A/S mapeiam
Cross/Circle/Square/Triangle, Enter e Shift direito mapeiam Start/Select, Q/E
mapeiam L/R e o teclado numerico 4/6/8/2 controla o analogico. Ao perder o foco
da janela, as teclas pressionadas sao limpas.

O modo interpretador e recomendado para os traces que instrumentam
`mips_interpreter_step`; traces colocados somente ali nao demonstram que um
bloco executado via AOT tomou o mesmo caminho.

## Investigacao atual: logos de abertura do GOW

### Sintoma observado

O Run252 (interpretador puro, com a nova entrada de teclado) foi executado por
ate quatro minutos. A janela e as capturas mostram preto/cinza, sem as logos da
Sony ou da Ready at Dawn. O run nao prova que o jogo aceitou a tecla Z. O log
confirma que `frontend_assets.bin` foi aberto e que `splash_screens.bin`
(72.012 bytes) foi lido integralmente; isso nao prova que os dados tenham sido
decodificados nem que tenham chegado a um draw visivel.

As primeiras nove primitivas GE sao faixas `through` que cobrem a tela. O draw
#10 e um quad preto sem textura com vertices de 480x272, representados pelo
renderer como NDC de -480 a 480 e -272 a 272; o draw #11 e os seguintes incluem
geometria fora do intervalo visivel. `sceMpegInit` retorna sucesso, mas os
imports de `sceVideocodec` nao registram chamadas no resumo do run. O scanner
do asset nao encontra assinaturas PSMF/MPEG comuns; isso nao prova qual e o
formato proprietario dos dados.

Portanto, a proxima tarefa e rastrear o consumidor/formato dos assets de
abertura e a cadeia que deveria produzir os logos (GE 2D ou outro caminho),
nao ajustar gameplay 3D sem um frame visivel comparavel.

### O que foi corrigido e medido

1. **Opcode VFPU3 `0x1B`.** A tabela local do PPSSPP confirma que o opcode
   primario `0x1B` seleciona o grupo VFPU3; os sub-opcodes validos da tabela
   sao `vcmp`, `vmin`, `vmax`, `vscmp`, `vsge` e `vslt`. O interpretador agora
   executa essas seis operacoes, incluindo prefixos VFPU e condition codes de
   `vcmp`. `tests/python/test_vfpu3.py` cobre as seis operacoes. O Run192 e
   anterior a essa correcao; um novo run e necessario para verificar o efeito
   no jogo.
2. **Tabela Allegrex SPECIAL.** Comparando com as tabelas e semantica do
   PPSSPP presente em `PPSSPP/`, foram corrigidos no interpretador:
   `CLZ/CLO`, `MADD/MADDU`, `MAX/MIN`, `MSUB/MSUBU` e as formas codificadas de
   `ROTR/ROTRV`. Antes, alguns functs eram interpretados como operacoes
   diferentes ou nao eram tratados.
3. **Trace de instrucoes candidatas.** A variavel
   `MIPS_TRACE_ALLEGREX_SPECIAL=1` ativa linhas `[ALLEGREX SPECIAL]`
   deduplicadas por PC, com instrucao e operandos. Esse trace so cobre o caminho
   pelo interpretador.
4. **Regressao de CPU.** `tests/python/test_allegrex_special.py` verifica os
   resultados dos encodings afetados; o teste e compilado e executado por
   Python usando GCC.
5. **Run191 contra Run190.** O Run191 observou quatro PCs executando `CLZ`, e
   nao mostrou os outros functs candidatos no trecho. Nos mesmos primeiros
   draws texturizados, o renderer registrou CLUT8 swizzled como **128x256** no
   Run191; no Run190, o estado GE cru `TSIZE0=0x002020` era decodificado e
   registrado como **1x1**. A coincidencia temporal indica uma mudanca efetiva
   no estado da textura, mas a cadeia exata entre a instrucao corrigida e o
   comando GE ainda nao foi demonstrada.
6. **Coordenadas dos draws.** Apesar do tamanho de textura diferente, os
   primeiros quads texturizados reportam praticamente os mesmos limites no
   log, aproximadamente `x=373..396`, `y=-23..3`, sob o rotulo GLES NDC. Esses
   valores ficam muito fora do intervalo visivel NDC usual `[-1, 1]`. Isso e
   uma pista forte de que transformacao, viewport, offset ou interpretacao das
   coordenadas precisa ser auditada antes de concluir que a textura e a unica
   causa visual. A captura agora grava JSON com `VTYPE`, vertices, matrizes
   float24, viewport, offsets e estagios clip/NDC/pixel;
   `tools/check_ge_coordinates.py` recalcula os estagios do renderer. Ainda e
   preciso comparar com PPSSPP.
7. **Video/MPEG.** O Run191 registra `sceMpegInit` e consultas de memoria MPEG,
   e abre/le o arquivo `splash_screens.bin`. No trecho verificado nao foi
   encontrada uma chamada de decode MPEG/AVC. Isso nao prova que o splash nao
   seja video, nem que nenhuma chamada ocorreu fora do trecho analisado.
8. **Display lists e texturas.** Runs anteriores registraram uma display list
   com `B8002020`, formato CLUT8, modo swizzled, CLUT de 1024 bytes e UVs
   variados. O watch de memoria existente encontrou leituras no endereco
   observado, mas nao exclui escritores que contornem `mips_write*`, como
   copias diretas do HLE/carregamento.

### Resultados dos runs 193–196

1. **Run193 GOW foi hibrido/AOT, nao Interpreter puro.** O log confirma
   `[BUILD MODE] HYBRID_AOT_INTERPRETER` e correspondencia da assinatura AOT
   do GOW. Portanto, zero avisos `Unsupported opcode` nesse run nao validam
   diretamente os handlers novos do `mips_interpreter.c`. O jogo executou
   318 enqueues de display list, registrou 11 fallbacks de blend `10/10` e
   encerrou com `faults=0`. O visual gravado permaneceu preto.
2. **Os dumps guest do GOW estao vazios.** Os 20 buffers de display do Run193,
   cada um com 480x272 RGBA e stride 512, contem somente bytes zero. O dump do
   draw #11 registra vertices com X em clip/NDC aproximadamente 374–396, fora
   do intervalo GLES visivel. Isso e uma divergencia concreta a comparar com
   o PPSSPP, nao prova isoladamente qual etapa tem o erro.
3. **Ozone demonstra geometria visivel, mas nao framebuffer guest.** O Run195
   foi Interpreter puro, produziu milhares de vertices em primitivas GE e o
   video mostra, por volta de 225–240 s, uma cena tridimensional em tons de
   cinza, visivel mas distorcida/recortada. Os 20 dumps de framebuffer guest
   do mesmo run tambem contem apenas zeros. Isso confirma que o caminho que
   pinta a janela GLES pode produzir geometria sem escrever a imagem de volta
   ao framebuffer PSP; nao e uma comparacao pixel-a-pixel sincronizada.
4. **Cave History e 2048 nao chegaram a uma prova visual do renderer.** O
   Run194 tem tres capturas de display, e o Run196 tem duas; todas sao zero.
   Nenhum desses logs contem registro `[GE Renderer] GE draw` ou encerramento
   limpo. O Run196 fica em loop no PC `0x08990C24`, depois de enfileirar duas
   listas GE. A causa do loop ainda nao foi isolada.
5. **Chamadas fallback com maior contagem.** Run193 conta 638 chamadas ao NID
   `0x79D1C3FA` (`sceKernelDcacheWritebackAll` na tabela local do PPSSPP).
   Run195 conta 906 chamadas ao NID `0x3F7AD767` (`sceRtcGetCurrentTick`).
   Esses imports devem ser avaliados por efeitos esperados antes de atribuir
   causalidade visual.
6. **Próxima comparação:** executar o GOW em `--interpreter` para uma linha de
   base com as correções recentes e, em seguida, executar o mesmo GOW em AOT.
   Fazer 2048 primeiro na próxima rodada, mas só considerar esse jogo um teste
   do renderer se o log registrar draws GE. Encerrar cada processo pela janela
   para obter o resumo de fallback; `Ctrl+Z` suspende e não conclui o processo.

### Validacao mais recente

Os testes direcionados de diagnosticos, VFPU3, Allegrex, COP1, framebuffer e
GLES passaram:

```bash
python -m unittest tests.python.test_diagnostic_tools tests.python.test_vfpu3 tests.python.test_vfpu_vsub tests.python.test_allegrex_special tests.python.test_cop1_differential tests.python.test_video_framebuffer tests.python.test_video_texture_pipeline
python -m unittest tests.python.test_vram_to_png
```

O build local do modo interpretador tambem terminou com sucesso:

```bash
wsl bash -lc 'cd /mnt/c/Users/onysi/Downloads/GOW && ./build_fixed_v38.sh --interpreter'
```

O build mostrou warnings preexistentes/fora do escopo (incluindo avisos de
tipos e VFPU) mas nao terminou com erro. O sucesso de compilacao e dos testes
nao significa que o GOW ja renderiza corretamente nem que a build WSL seja a
build ARM64 do R36S.

Para gerar um resumo repetivel de um log e verificar a tabela VFPU3 contra a
referencia local:

```bash
python tools/summarize_log.py artifacts/runs/RunNNN.console.log
python tools/analyze_png_abort.py artifacts/runs/RunNNN-2048-Interpreter.console.log
python tools/audit_vfpu3.py
python tools/blend_factor_table.py
python tools/check_ge_coordinates.py artifacts/runs/RunNNN-draw11_draw11_state.json
python tools/imports_report.py artifacts/runs/RunNNN.console.log artifacts/runs/RunNNN-imports.csv
python tools/scan_splash.py umd0/PSP_GAME/USRDIR/data/splash_screens.bin
python tools/decode_ge_list.py artifacts/runs/RunNNN-ge-list.bin --base 0x084A0000
```

The script `blend_factor_table.py` outputs all 121 source/destination factor
pairs and marks PSP doubled-alpha factors and separate FIXA/FIXB constants
that cannot be represented exactly by one GLES `glBlendFunc` call.
`analyze_png_abort.py` validates `data/take.png` (PNG CRCs and decompression),
compares each logged `take.png` read against the matching source bytes using
the guest FNV-1a hash, and reports whether the run reached any GE draw.

Runs201–204 traced the abort through `png_create_read_struct_2` and `longjmp`
to zlib's `incorrect data check` error in `png_read_row`. Run204 is a pure
interpreter run: all 50 logged `take.png` reads match the local asset, and no
unsupported opcodes were reported. The expected IDAT Adler-32 is
`0xE5673AB0`, while the guest's calculated checksum at the failure is
`0x51B242D9`.

Runs206–208 narrowed the cause further. The first two 577-byte scanlines passed
byte-for-byte; scanline 3 first differs at byte 6 (`guest=0x01`, expected
`0x00`). The output byte is written by guest `inflate_fast` at `0x08936C90`.
The follow-up trace and comparison with PPSSPP identified incorrect
little-endian merge semantics in the interpreter's `LWL/LWR/SWL/SWR`
operations. These were corrected and covered at every byte alignment.
Run212 reports all three captured scanlines matching the host output, and
Run214 reaches at least 24,576 GE draws before exiting with status zero. The
2048 PNG issue is resolved for this test; it was an interpreter/memory issue,
not GE, 3D, or audio.

`compare_inflate_output.py` compares the bytes read by the guest Adler-32
routine with the host's decoded PNG scanlines. A trace for this tool should use
the exact guest addresses for the zlib Adler function and output buffer:

```bash
mkdir -p artifacts/runs
MIPS_TRACE_FILE=artifacts/runs/RunNNN-adler.trace \
MIPS_TRACE_WATCH_PC=0x0893AAE0 \
MIPS_TRACE_WATCH_EVERY=1 \
MIPS_TRACE_WATCH_SAMPLES=5 \
MIPS_TRACE_WATCH_TRACE_AFTER=4 \
MIPS_TRACE_WATCH_TRACE_STEPS=10000 \
MIPS_TRACE_WATCH_ONLY=1 \
MIPS_TRACE_WATCH_ADDR=0x09496220 \
MIPS_TRACE_WATCH_SIZE=0x241 \
timeout --kill-after=2s 240s ./output/main TesterGames/2048PSP/EBOOT.PBP \
  > artifacts/runs/RunNNN-adler.console.log 2>&1
python tools/compare_inflate_output.py \
  TesterGames/2048PSP/data/take.png artifacts/runs/RunNNN-adler.trace
```

This capture is intentionally bounded to the first scanlines reaching
`adler32`. It remains useful for regressions; the known mismatch in Runs206–208
was caused by the interpreter's unaligned word merge operations and was fixed
before Run212. A mismatch on a future run should be investigated as a new
regression rather than treated as an unresolved 2048 failure.

Set `MIPS_DISPLAY_DUMP_PREFIX` to capture the first 20 raw guest display
buffers submitted through `mips_video_set_framebuffer`. The `.bin` files
include complete rows with stride. Convert a dump to PNG with:

```bash
python tools/vram_to_png.py artifacts/runs/RunNNN_display01_0408C000_f3.bin \
  artifacts/runs/RunNNN-display01.png --width 480 --height 272 \
  --stride 512 --format 3
```

The output directory must already exist, and PNG conversion requires Pillow.
New runs also print a `[HLE Fallback Summary]` for each registered fallback;
the import report includes exact counts from these summaries. Historical logs
without those lines show fallback calls as unknown.

The current renderer presents the guest buffer through
`mips_video_begin_frame`, but GE primitives are drawn directly to GLES rather
than rasterized back into guest VRAM at `FBP/FBW`. The synthetic framebuffer
test verifies buffer upload/scanout only; it is not an end-to-end PSP GE
framebuffer write test.

GE display lists are now accumulated into one GLES frame and presented at
`sceDisplayWaitVblankStart()` / `sceDisplayWaitVblankStartCB()`, instead of
presenting every `sceGeListEnQueue()` call as a separate frame. This fixes a
confirmed frame-cadence bug and has a regression check for preserving earlier
draws across successive lists. Run215's screenshot now shows a visible
checkerboard region, but not a recognizable 2048 board. Run216 submitted many
GOW lists/draws, while the user saw only the untouched gray window. The old
logs do not count successful VBlank calls, so set `MIPS_TRACE_VBLANK=1` on the
next run to distinguish a missing presentation from off-screen/empty draws.
The trace prints the first eight waits, then every 60th wait.

### Proximos diagnosticos sugeridos

- Rastrear a leitura e os consumidores de `splash_screens.bin` e
  `frontend_assets.bin` ate o upload de textura/estado GE ou a chamada de
  decodificacao correspondente. Identificar qual fonte efetivamente contem as
  logos antes de escolher entre renderer 2D, formato de asset ou HLE de video.
- Capturar draw/texture/vertices/CLUT e backbuffer no primeiro frame que deveria
  mostrar uma logo; os frames 1-20 do Run252 documentam somente preto/cinza.
- Rastrear, no draw #11 da mesma display list, a decodificacao de vertices, as
  matrizes world/view/projection, os registradores de viewport scale/center e
  os offsets. Registrar os valores crus e as coordenadas intermediarias.
- Comparar os mesmos comandos, vertices e registradores com um frame equivalente
  do PPSSPP. Usar estado/draw como ancora, nao tempo de execucao.
- Verificar se a transformacao aplicada aos vertices 3D do GOW (incluindo
  viewport e divisao por W) esta coerente com o formato e a convencao de
  coordenadas do GE PSP antes de alterar o renderer.
- Manter a correcao de dimensoes CLUT8; nao substituir `TSIZE0` nem forcar a
  posicao dos quads para apenas fazer a imagem aparecer.
- Se for necessario, rastrear a origem do valor `TSIZE0` e os escritores da
  display list por caminhos que nao passem pelos helpers instrumentados.
- Investigar o caminho de video do splash separadamente apenas se surgirem
  evidencias adicionais de chamadas de decode ou dados PMF/PSS; o Run252 leu o
  asset, mas nao registrou chamadas de `sceVideocodec`.

### Capturas opcionais do draw e de I/O

Para capturar a imagem apresentada na janela, defina
`MIPS_VIDEO_CAPTURE_PREFIX`; o renderer salva as primeiras 20 apresentações
como RGBA. `MIPS_VIDEO_CAPTURE_SKIP=N` descarta as primeiras N apresentações
antes de começar a capturar, útil para inspecionar uma tela após a inicialização.

O renderer pode salvar os dados efetivamente usados por um draw, sem alterar o
estado do jogo. `MIPS_GE_DUMP_DRAW` seleciona o indice da chamada
`mips_video_draw_prim` (começando em 1); `MIPS_GE_DUMP_PREFIX` define o prefixo
dos arquivos. Para capturar o draw #11 e o caminho de leitura dos assets:

```bash
MIPS_GE_DUMP_DRAW=11 \
MIPS_GE_DUMP_PREFIX=artifacts/runs/Run192-draw11 \
MIPS_GE_TRACE_IO_DATA=1 \
MIPS_DISPLAY_DUMP_PREFIX=artifacts/runs/Run192 \
./output/main > artifacts/runs/Run192.console.log 2>&1
```

Para executar no modo interpretador:

```bash
MIPS_GE_DUMP_DRAW=11 \
MIPS_GE_DUMP_PREFIX=artifacts/runs/Run192-draw11 \
MIPS_GE_TRACE_IO_DATA=1 \
MIPS_DISPLAY_DUMP_PREFIX=artifacts/runs/Run192 \
MIPS_TRACE_ALLEGREX_SPECIAL=1 \
./build_fixed_v38.sh --interpreter
```

O segundo exemplo compila, nao executa o jogo; depois da compilacao, inicie
`./output/main` com as mesmas variaveis de ambiente. O diretório indicado no
prefixo precisa existir. As capturas do draw incluem bytes crus da textura,
indices CLUT8 em PGM, paleta e textura RGBA em PPM/raw, estado dos registradores
GE, matrizes, bytes dos quatro primeiros vertices e as etapas de coordenadas
registradas pelo renderer. `MIPS_GE_TRACE_IO_DATA=1` registra até 128 leituras
de `splash_screens.bin`, `frontend_assets.bin` e `data/take.png` (usado pelo
homebrew 2048), incluindo offset, tamanho, destino PSP e hashes FNV-1a. Em
leituras síncronas, o hash de origem do host nao fica disponível porque o HLE
lê diretamente para a memória do guest; o hash do destino ainda é registrado.
Esses arquivos podem ser grandes ou conter dados do jogo; compartilhe somente
os artefatos necessários para o diagnóstico.

## Testes

Os testes Python em `tests/python/` constroem pequenos harnesses C ou verificam
comportamento de modulos. Exemplos:

```bash
python -m unittest tests.python.test_diagnostic_tools
python -m unittest tests.python.test_allegrex_special
python -m unittest tests.python.test_cop1_differential
python -m unittest tests.python.test_ge_clut_load
python -m unittest tests.python.test_video_framebuffer
python -m unittest tests.python.test_video_texture_pipeline
```

Alguns testes do caminho GLES dependem de bibliotecas/contexto grafico
disponiveis no ambiente; um teste ignorado por dependencia ausente nao deve ser
interpretado como validacao visual.

## Estado do versionamento

O workspace usado na investigacao nao foi reconhecido como repositorio Git.
Portanto, nao e possivel verificar commits nem aplicar um fluxo de um commit por
correcao sem antes disponibilizar o historico Git correto. Nenhuma inicializacao
de repositorio ou commit foi feita automaticamente.
