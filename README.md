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

O modo interpretador e recomendado para os traces que instrumentam
`mips_interpreter_step`; traces colocados somente ali nao demonstram que um
bloco executado via AOT tomou o mesmo caminho.

## Investigacao atual: tela inicial do GOW

### Sintoma observado

O usuario relata que a execucao mais recente nao mudou visualmente em relacao
ao run anterior: a tela fica preta durante o intervalo em que deveriam ocorrer
os fades brancos e depois aparece novamente o padrao com um bloco preto no
canto. O jogo nao foi confirmado como estando na tela inicial renderizada
corretamente; essa continua sendo uma interpretacao dos fades e das listas
submetidas, nao uma conclusao visual.

### O que foi corrigido e medido

1. **Tabela Allegrex SPECIAL.** Comparando com as tabelas e semantica do
   PPSSPP presente em `PPSSPP/`, foram corrigidos no interpretador:
   `CLZ/CLO`, `MADD/MADDU`, `MAX/MIN`, `MSUB/MSUBU` e as formas codificadas de
   `ROTR/ROTRV`. Antes, alguns functs eram interpretados como operacoes
   diferentes ou nao eram tratados.
2. **Trace de instrucoes candidatas.** A variavel
   `MIPS_TRACE_ALLEGREX_SPECIAL=1` ativa linhas `[ALLEGREX SPECIAL]`
   deduplicadas por PC, com instrucao e operandos. Esse trace so cobre o caminho
   pelo interpretador.
3. **Regressao de CPU.** `tests/python/test_allegrex_special.py` verifica os
   resultados dos encodings afetados; o teste e compilado e executado por
   Python usando GCC.
4. **Run191 contra Run190.** O Run191 observou quatro PCs executando `CLZ`, e
   nao mostrou os outros functs candidatos no trecho. Nos mesmos primeiros
   draws texturizados, o renderer registrou CLUT8 swizzled como **128x256** no
   Run191; no Run190, o estado GE cru `TSIZE0=0x002020` era decodificado e
   registrado como **1x1**. A coincidencia temporal indica uma mudanca efetiva
   no estado da textura, mas a cadeia exata entre a instrucao corrigida e o
   comando GE ainda nao foi demonstrada.
5. **Coordenadas dos draws.** Apesar do tamanho de textura diferente, os
   primeiros quads texturizados reportam praticamente os mesmos limites no
   log, aproximadamente `x=373..396`, `y=-23..3`, sob o rotulo GLES NDC. Esses
   valores ficam muito fora do intervalo visivel NDC usual `[-1, 1]`. Isso e
   uma pista forte de que transformacao, viewport, offset ou interpretacao das
   coordenadas precisa ser auditada antes de concluir que a textura e a unica
   causa visual. Ainda e preciso confirmar que esses valores deveriam estar em
   NDC e conferir a conversao contra PPSSPP.
6. **Video/MPEG.** O Run191 registra `sceMpegInit` e consultas de memoria MPEG,
   e abre/le o arquivo `splash_screens.bin`. No trecho verificado nao foi
   encontrada uma chamada de decode MPEG/AVC. Isso nao prova que o splash nao
   seja video, nem que nenhuma chamada ocorreu fora do trecho analisado.
7. **Display lists e texturas.** Runs anteriores registraram uma display list
   com `B8002020`, formato CLUT8, modo swizzled, CLUT de 1024 bytes e UVs
   variados. O watch de memoria existente encontrou leituras no endereco
   observado, mas nao exclui escritores que contornem `mips_write*`, como
   copias diretas do HLE/carregamento.

### Validacao mais recente

Os testes direcionados Allegrex e COP1 passaram:

```bash
python -m unittest tests.python.test_allegrex_special tests.python.test_cop1_differential
```

O build local do modo interpretador tambem terminou com sucesso:

```bash
wsl bash -lc 'cd /mnt/c/Users/onysi/Downloads/GOW && ./build_fixed_v38.sh --interpreter'
```

O build mostrou warnings preexistentes/fora do escopo (incluindo avisos de
tipos e VFPU) mas nao terminou com erro. O sucesso de compilacao e dos testes
nao significa que o GOW ja renderiza corretamente nem que a build WSL seja a
build ARM64 do R36S.

### Proximos diagnosticos sugeridos

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
  evidencias de chamadas de decode ou dados PMF/PSS.

## Testes

Os testes Python em `tests/python/` constroem pequenos harnesses C ou verificam
comportamento de modulos. Exemplos:

```bash
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
