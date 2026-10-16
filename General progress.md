# Estado geral do emulador PSP / Allegrex

## Escopo, criterio e limites deste relatorio

Este documento registra o estado verificavel no codigo e nos artefatos de
diagnostico disponiveis no workspace. "Implementado" significa que existe uma
rota de codigo com logica funcional observavel; nao significa conformidade
completa com o PSP. Onde uma API retorna um valor fixo, usa um modelo
simplificado ou depende de um fallback, isso esta indicado explicitamente.

O inventario combina os fontes atuais com resultados de runs anteriores. Os
resultados dos runs sao evidencias datadas: nao garantem que uma build posterior
produza exatamente o mesmo comportamento. Tambem nao se deve confundir:

* NID/import registrado no linker com uma implementacao funcional;
* um retorno zero com sucesso semantico;
* lista GE enfileirada com display list integralmente fiel;
* draw visivel no GLES com framebuffer PSP atualizado;
* build hibrida/AOT com cobertura integral da CPU;
* teste sintetico de um subsistema com compatibilidade do jogo completo.

Arquivos-chave: `main_v35.c`, `mips_hle.c/.h`, `mips_linker.c`,
`sceKernelThread.c/.h`, `sceSysMem.c/.h`, `mips_ge.c/.h`, `mips_video.c/.h`,
`mips_audio.c/.h`, `mips_sas.c`, `mips_interpreter.c`, `aot_recompiler_v5.py`,
`gow_core_aot.c` e `mips_dispatcher.c/.h`.

## Resumo executivo

| Componente | Estado atual |
| --- | --- |
| Boot, PBP/ELF e relocacoes | Carrega executaveis PSP/PBP, prepara memoria/relocacoes e inicia o codigo de usuario; o suporte a PRX e parcial. |
| CPU interpretada | Implementa um subconjunto amplo de Allegrex/MIPS, COP1 e VFPU; ainda ha operacoes nao suportadas e semanticas aproximadas. |
| AOT | Tradutor estatico para um subconjunto inteiro/MIPS, com blocos especificos ao GOW e fallback para o interpretador; nao traduz VFPU de forma abrangente. |
| HLE | I/O, threads/sincronizacao, memoria, UMD, display, controles, GE e audio possuem rotas; parte delas e simplificada e imports restantes podem cair em zero generico. |
| GE / 2D | Maquina de estado parcial, leitura de display lists, vertices, primitivas, texturas diretas/CLUT e apresentacao GLES existem. UVs `through` foram corrigidos contra PPSSPP; cada draw GE inicia/marca o frame e a apresentacao nao troca frames vazios; listas GE acumulam ate o VBlank. Nao e uma implementacao completa do GE PSP. |
| GE / 3D | Matrizes world/view/projection e conversao de vertices estao presentes; Ozone comprovou geometria visivel, mas distorcida. GOW ainda apresenta falhas visuais. |
| Framebuffer | O renderer apresenta buffers guest e desenha GE diretamente na superficie GLES; draws nao sao rasterizados de volta ao framebuffer guest. |
| Audio | Canais PCM SDL2 funcionam como modelo de saida; SAS mantem estado de vozes, mas a sintese/mixagem SAS real esta incompleta. |
| Estado do GOW | O jogo ja carrega e submete listas GE; a imagem correta/jogabilidade nativa no R36S ainda nao foi atingida. |

## 1. Syscalls, HLE e servicos PSP

### 1.1 Como a chamada chega ao handler

`mips_linker.c` associa NIDs importados a pequenos stubs/syscall codes. A
funcao `mips_hle_syscall(MIPS_CPU *cpu, MIPS_HLE_Kernel *kernel)` despacha a
parte central das chamadas codificadas no namespace interno do HLE. Parte dos
servicos de ThreadMan e memoria e encaminhada a handlers dedicados. Em
`main_v35.c`, imports que nao foram ligados a um handler normal recebem slots
fallback e sao interceptados no dispatcher de syscalls.

O resumo `[HLE Fallback Summary]` conta chamadas por slot quando o processo
termina normalmente. A classificacao `critical-unimplemented` do linker e uma
classificacao por biblioteca/import; nao prova que o import tenha sido chamado
nem que nao exista um desvio especial por NID em `main_v35.c`.

### 1.2 Implementacoes com logica funcional (mas nao PSP-spec-complete)

#### I/O, filesystem e UMD

O HLE contem logica para abrir/ler/fechar arquivos, seek, `getstat`,
listar diretorios, leituras assincronas e espera/poll de I/O. A camada resolve
caminhos PSP e, conforme o caminho, acessa arquivos locais ou a imagem/disco
UMD. Operacoes de mutacao de arquivos como mkdir, rename, remove e rmdir
tambem possuem handlers.

NIDs de I/O relevantes presentes no mapeamento incluem:

| API | NID | Estado observado |
| --- | --- | --- |
| `sceIoOpen` | `0x109F504F`, `0x109F50BC` | Handler de abertura. |
| `sceIoRead` | `0x6A638D83` | Leitura sincrona para memoria guest. |
| `sceIoLseek` / `sceIoLseek32` | `0x27EB27B8` / `0x68963324` | Seek com ABI 64-bit PSP e variante 32-bit. |
| `sceIoReadAsync` / `sceIoOpenAsync` | `0xA0B5A7C2` / `0x89AA9906` | Caminho assincrono com estado de operacao. |
| `sceIoWaitAsync` / `sceIoPollAsync` | `0xE23EEC33` / `0x3251EA56` | Espera/consulta de operacao assincrona. |
| `sceIoClose` | `0x810C4BC3` | Fechamento de handle. |
| `sceIoGetstat` | `0xACE946E8` | Handler de consulta presente para este NID; houve correcao anterior nos atributos PSP de arquivo/diretorio. |
| `sceIoDopen` / `sceIoDread` / `sceIoDclose` | `0xB29DDF9C` / `0xE3EB004C` / `0xEB092469` | Enumeracao de diretorio. |
| `sceIoMkdir` / `sceIoRename` / `sceIoRemove` / `sceIoRmdir` | `0x06A70004` / `0x779103A0` / `0xF27A9C51` / `0x1117C65F` | Operacoes de filesystem. |
| `sceIoLseekAsync` / `sceIoWaitAsyncCB` | `0x71B19E77` / `0x35DBD746` | Rotas assincronas adicionais. |

**Divergencias importantes entre mapeamento e handler:** `sceIoWrite`
(`0x42EC03AC`) e `sceIoWriteAsync` (`0x0925712E`) sao associados no linker aos
codes `0x1111C`/`0x1111D`, mas nao ha cases correspondentes no switch HLE
revisado; nao estao confirmados como implementados e chegam ao default de
syscall desconhecida (`-1`) quando passam por esse switch. `sceIoGetstat`
(`0x55F4717D`) e mapeado como `0x1111E`, enquanto o handler encontrado e
`0x11125` para o outro NID `0xACE946E8`; portanto, somente a rota que chega ao
case existente esta comprovada. `sceIoDevctl`, `sceIoIoctl` e
`sceIoIoctlAsync` tambem aparecem mapeados sem handler dedicado observado e
atingem o mesmo default `-1` nesse caminho. Estas chamadas devem ser tratadas
como nao implementadas/verificadas ate corrigir os codes ou provar um desvio
especial.

#### ThreadMan, sincronizacao e callbacks

`sceKernelThread.c` implementa um scheduler cooperativo, contexto de threads,
estados `FREE/RUNNING/READY/WAITING/DORMANT`, espera por termino de thread,
espera de semaforo, atraso com deadline e espera/wakeup de event flag. Ha
criacao/inicio/saida/exclusao/terminacao de threads, consulta de estado,
prioridade, suspensao/retomada de dispatch, FPL e callbacks.

Assinaturas publicas declaradas em `sceKernelThread.h` incluem:

```c
int hle_create_and_start_thread(MIPS_CPU *cpu, const char *name,
                                uint32_t entry_pc);
void run_scheduler(MIPS_CPU *cpu, ThreadState current_new_state);
bool hle_thread_wait_semaphore(MIPS_CPU *cpu, int semaphore_id, int count,
                               uint64_t deadline_us, uint32_t timeout_addr);
bool hle_thread_wait_event_flag(MIPS_CPU *cpu, int event_flag_id,
                                uint32_t bits, uint32_t mode,
                                uint32_t pattern, uint32_t status_addr,
                                uint32_t timeout_addr,
                                uint64_t deadline_us);
void hle_thread_wake_semaphore(MIPS_CPU *cpu, int semaphore_id, int *count);
void hle_thread_wake_event_flag(MIPS_CPU *cpu, int event_flag_id,
                                uint32_t *pattern, bool multiple);
void hle_sceKernelCreateThread(MIPS_CPU *cpu);
void hle_sceKernelStartThread(MIPS_CPU *cpu);
void hle_sceKernelExitThread(MIPS_CPU *cpu);
void hle_sceKernelDeleteThread(MIPS_CPU *cpu);
void hle_sceKernelDelayThread(MIPS_CPU *cpu);
void hle_sceKernelWaitThreadEndCB(MIPS_CPU *cpu);
uint32_t hle_sceKernelReferThreadStatus(MIPS_CPU *cpu);
void hle_sceKernelCreateFpl(MIPS_CPU *cpu);
void hle_sceKernelAllocateFpl(MIPS_CPU *cpu);
void hle_sceKernelFreeFpl(MIPS_CPU *cpu);
void hle_sceKernelCreateCallback(MIPS_CPU *cpu);
void hle_sceKernelCheckCallback(MIPS_CPU *cpu);
void hle_sceKernelDeleteCallback(MIPS_CPU *cpu);
```

As rotas nomeadas de ThreadMan incluem semaforos (create/wait/signal/poll/
delete), event flags (create/set/clear/delete/wait/refer status), terminacao
de thread, FPL e consultas de tempo. A tabela de NIDs tambem mapeia
`sceKernelGetThreadId`, `sceKernelGetSystemTimeWide/Low`,
`sceKernelReferThreadStatus`, `sceKernelChangeThreadPriority`,
`sceKernelSleepThreadCB` e `sceKernelWaitThreadEndCB`.

Inventario nominal das rotas que possuem handler ou desvio funcional
identificavel no codigo:

* **ThreadMan:** `sceKernelCreateThread`, `sceKernelStartThread`,
  `sceKernelExitThread`, `sceKernelDeleteThread`,
  `sceKernelTerminateThread`, `sceKernelTerminateDeleteThread`,
  `sceKernelDelayThread`, `sceKernelDelayThreadCB`, `sceKernelSleepThreadCB`,
  `sceKernelWaitThreadEndCB`, `sceKernelGetThreadId`,
  `sceKernelGetThreadCurrentPriority`,
  `sceKernelChangeThreadPriority`, `sceKernelReferThreadStatus`,
  `sceKernelCreateSema`, `sceKernelWaitSema`, `sceKernelSignalSema`,
  `sceKernelDeleteSema`, `sceKernelPollSema`, `sceKernelReferSemaStatus`,
  `sceKernelCreateEventFlag`, `sceKernelSetEventFlag`,
  `sceKernelClearEventFlag`, `sceKernelDeleteEventFlag`,
  `sceKernelWaitEventFlag`, `sceKernelReferEventFlagStatus`,
  `sceKernelCreateFpl`, `sceKernelDeleteFpl`, `sceKernelAllocateFpl`,
  `sceKernelFreeFpl`, `sceKernelGetSystemTimeLow`,
  `sceKernelGetSystemTimeWide`, `sceKernelCpuSuspendIntr`,
  `sceKernelCpuResumeIntr`, `sceKernelCpuResumeIntrWithSync`,
  `sceKernelIsCpuIntrEnable`, `sceKernelCheckCallback`,
  `sceKernelDeleteCallback`, `sceKernelLoadModule`,
  `sceKernelStartModule` e `sceKernelStopModule`.
* **SysMem:** `sceKernelTotalFreeMemSize`, `sceKernelMaxFreeMemSize`,
  `sceKernelAllocPartitionMemory`, `sceKernelGetBlockHeadAddr`,
  `sceKernelFreePartitionMemory`, `sceKernelPrintf`,
  `sceMeMalloc`, `sceMeCalloc`, `sceMeFree`.
* **GE:** `sceGeListEnqueue`, `sceGeListSync`, `sceGeDrawSync`,
  `sceGeListUpdateStallAddr`, `sceGeEdramGetAddr`,
  `sceGeEdramGetSize`, `sceGeEdramSetAddrTranslation`,
  `sceGeSetCallback`, `sceGeUnsetCallback`. Varios sao modelos simples;
  a presenca nominal nao significa semantica GE completa.
* **Display:** `sceDisplaySetMode`, `sceDisplaySetFrameBuf`,
  `sceDisplayWaitVblankStart`, `sceDisplayWaitVblankStartCB`,
  `sceDisplayGetVblankCounter`/`sceDisplayGetVcount` no code interno
  compartilhado `0x60004`. O linker tem duas condicoes para o mesmo NID
  `0x9C6EAAD7`; a segunda e inalcancavel, logo a distincao nominal entre essas
  duas APIs precisa ser confirmada.
* **Controle:** `sceCtrlReadBufferPositive`, `sceCtrlPeekBufferPositive`,
  `sceCtrlSetSamplingCycle` e `sceCtrlSetSamplingMode`. A ultima ajusta um
  estado global/modelado; nao significa sampling PSP temporizado nem input
  fisico.
* **Audio:** `sceAudioChReserve`, `sceAudioChRelease`,
  `sceAudioOutputPannedBlocking`, `sceAudioOutputPanned` e
  `sceAudioSetChannelDataLen`.
* **Audio codec:** `sceAudiocodecCheckNeedMem`,
  `sceAudiocodecGetEDRAM` (preparacao de contexto, sem decodificacao).
* **SAS:** `__sceSasInit`, `__sceSasCore`, `__sceSasGetEndFlag`,
  `__sceSasGetOutputMode`, `__sceSasRevType`, `__sceSasRevVON`,
  `__sceSasRevEVOL`, `__sceSasRevParam`, `__sceSasSetVoice`,
  `__sceSasSetKeyOn` (estado/modelo parcial, sem sintese completa).
* **UMD:** `sceUmdCheckMedium`, `sceUmdActivate`, `sceUmdGetDriveStat`,
  `sceUmdWaitDriveStatWithTimer` (estado simulado ligado a raiz/arquivos
  locais).
* **Stdio:** `sceKernelStdin`, `sceKernelStdout`, `sceKernelStderr`.

Esta lista identifica rotas, nao certifica conformidade de todas as
assinaturas/flags/tamanhos nem prova que todas tenham sido chamadas nos jogos.
O helper `hle_sceKernelGetThreadState()` existe em `sceKernelThread.c`, mas
nao foi localizado um mapeamento NID/dispatcher correspondente na tabela
revisada; sua presenca como funcao nao basta para classificar a syscall como
conectada. Tambem ha uma colisao de roteamento a investigar: o linker associa
NID `0x82BC5777` a `sceKernelGetSystemTimeWide`, mas um ramo do fallback de
`main_v35.c` associa o mesmo NID a `hle_sceKernelGetThreadCurrentPriority`.
Qual caminho e alcancado depende de como o import foi resolvido; nao assumir
que as duas semanticas estejam disponiveis para o mesmo import.

O scheduler nao equivale ao scheduler PSP completo: e cooperativo e tem
comportamento proprio para preempcao, trampolins de retorno e modulo principal.
FPL representa blocos fixos com implementacao local simplificada; tres NIDs de
alocacao distintos sao atualmente ligados ao mesmo handler
`sceKernelAllocateFpl`, portanto a diferenca semantica entre allocate/try/
allocate-with-callback deve ser validada.

#### SysMem e ModuleMgr

`sceSysMem.c` fornece alocacao/liberacao de blocos de memoria por particao,
consulta do endereco base de bloco e handlers de memoria ME. A alocacao usa
faixas e metadados do proprio core; nao reproduz todas as particoes,
fragmentacao ou regras do firmware PSP. `hle_sceKernelTotalFreeMemSize` e
`hle_sceKernelMaxFreeMemSize` sao estimativas/modelos, nao consulta de uma
memoria PSP real.

`main_v35.c` tem tratamento de `sceKernelLoadModule`,
`sceKernelStartModule` e `sceKernelStopModule`, com estados registrados e
criacao de thread para o entry point quando presente. ModuleMgr/PRX continua
parcial; o tratamento nao prova compatibilidade geral com todos os modulos.

#### Display, input e GE

`sceDisplaySetFrameBuf` atualiza o framebuffer que o renderer apresenta.
`sceDisplayWaitVblankStart()` e `sceDisplayWaitVblankStartCB()` aguardam o
proximo tick do timer monotônico (~60 Hz); o VBlank autonomo tambem apresenta
frames pendentes. A temporizacao e a troca basica de framebuffer existem, mas
nao ha entrega completa de interrupcoes ou wakeup/reschedule de threads PSP.

`sceCtrlReadBufferPositive` e `sceCtrlPeekBufferPositive` copiam
`g_ctrl_state` para o buffer guest e retornam uma amostra. O estado e global e,
no codigo revisto, nao ha evidencia de leitura real de controles fisicos do
R36S. `sceCtrlSetSamplingCycle` armazena o ciclo e retorna sucesso.

As chamadas de GE que enfileiram/processam display lists e enviam estado ao
renderer existem; os limites tecnicos estao detalhados na secao 2.

#### Audio

O mixer SDL2, os canais PSP basicos e operacoes de reserva/liberacao/saida de
buffers PCM possuem logica real. Isso nao implica implementacao integral de
codecs ou de SAS; ver secao 3.

### 1.3 Stubs, modelos e bypasses conhecidos

| Servico | Comportamento do core | Limite |
| --- | --- | --- |
| Import sem handler especifico | O slot fallback de `main_v35.c` e contado e o valor inicial de retorno e zero. | Zero pode ser interpretado pelo jogo como sucesso, embora nenhum efeito tenha ocorrido. |
| `sceKernelDcacheWritebackAll` (`0x79D1C3FA`) | Rota especial retorna zero. | Nao existe cache GPU guest equivalente invalidado nem a mesma semantica/reschedule de PPSSPP. Run193 registrou 638 chamadas. |
| `sceRtcGetCurrentTick` (`0x3F7AD767`) | Run195 registra chamadas via fallback; retorno deve ser tratado como nao implementado/valor generico ate provar handler atual. | Ozone fez 906 chamadas em um run historico; candidato de diagnostico, nao causa confirmada. |
| `sceGeSetCallback` / `sceGeUnsetCallback` | Aceitam registro/operacao de forma simplificada. | Nao ha fila completa de interrupcoes/callbacks GE. |
| `sceGeListSync` / `sceGeDrawSync` | Retornam como concluidas devido ao processamento sincrono local. | Nao modelam fila GPU assincrona PSP, stalls e sincronizacao completa. |
| `sceDisplayWaitVblankStart` e relacionados | Esperam o proximo tick do timer monotônico (~60 Hz) e atualizam o contador. | Nao modelam entrega completa de interrupcao, wakeup/reschedule de threads ou cadencia exata do scheduler PSP. |
| `sceCtrl*` de leitura | Copia estado global configurado. | Sem input fisico comprovado nesta analise. |
| `sceAudiocodecCheckNeedMem` / `sceAudiocodecGetEDRAM` | Contexto/campos e valores esperados sao preenchidos de forma simplificada. | Nao decodifica streams de audio. |
| SAS (`__sceSasCore` e relacionadas) | Mantem estado de vozes, flags e contagem de samples; o core pode zerar buffers. | Nao faz sintese PCM SAS equivalente ao PSP. |
| Consulta de memoria livre | Retorna estimativa/constante do allocator local. | Nao representa a memoria dinamica do hardware PSP. |
| CPU intrinsics de interrupcao e locks de Power | Simulados/aceitos no fluxo HLE. | Nao ha estado fisico de CPU/energia PSP. |
| `sceIoWrite`, `sceIoWriteAsync`, alguns NIDs de `sceIoGetstat`, `sceIoDevctl`, `sceIoIoctl`, `sceIoIoctlAsync` | Mapeados para codes internos, mas sem case dedicado encontrado. | No switch central alcancam o default de syscall desconhecida (`-1`); nao possuem resposta PSP implementada. |
| `sceDisplayGetFrameBuf` | Nao foi encontrado mapeamento/handler equivalente no inventario revisto. | Necessario se o jogo consulta explicitamente o framebuffer atual. |

Numa rota diferente, `mips_hle_syscall()` tem `default` com aviso de syscall
desconhecida e retorno `-1`. Isso nao elimina o problema dos imports ligados a
slots fallback, cujo retorno generico pode ser zero.

### 1.4 Syscalls prioritarias para completar/verificar

1. **Falhas de import chamado e sem handler.** Emitir erro identificavel e
   retorno PSP correto; nao retornar zero silenciosamente. Priorizar por
   contagem real no resumo de fallback e pelo caminho de execucao do jogo.
   Inclui os mapeamentos com handler ausente de `sceIoWrite`,
   `sceIoWriteAsync`, alguns NIDs de `sceIoGetstat` e
   `sceIoDevctl`/`sceIoIoctl*`.
2. **Encerramento de modulo e relogio.** `sceKernelSelfStopUnloadModule`
   (`0xD675EBB8`) precisa concluir o fluxo de saida do 2048 corretamente; o
   historico observou fallback generico e spin apos retorno. Implementar/
   validar `sceRtcGetCurrentTick` e comparar
   com `sceKernelGetSystemTimeLow/Wide`; verificar loops dependentes de
   duracao/vblank antes de ajustar.
3. **Cache/GE.** Definir o efeito correto de `sceKernelDcacheWritebackAll` e
   invalidacao de textura/VRAM, depois de confirmar se o renderer tem estado
   cacheado que dependa disso.
4. **Geometria de event flag e waits.** Confirmar modos, padroes, timeout,
   wakeup multiplo e status output com testes direcionados; os mecanismos
   existem, mas ainda sao um modelo local.
5. **SAS/audio codec.** Descodificacao e sintese sao lacunas reais caso os jogos
   dependam do caminho; completar com rastreamento de imports usados.
6. **GE callback/sync e transferencias.** `sceGeSetCallback`, sincronismo real
   de lista e comandos de transferencia VRAM (TRX) sao lacunas do pipeline.
7. **`sceIoDevctl`/`sceIoIoctl*` e `sceDisplayGetFrameBuf`.** Implementar apenas
   quando um trace demonstrar uso ou dependencia.

## 2. Graphics Engine (GE), display lists e video

### 2.1 Estado GE e processamento de comandos

`mips_ge_init(MIPS_GraphicsEngine *ge)`, `mips_ge_begin_list(...)`,
`mips_ge_decode_vtype(...)`, `mips_ge_get_vertex_layout(...)`,
`mips_ge_vertex_stride(...)`, `mips_ge_execute_command(...)` e
`mips_ge_run_list(...)` compoem o nucleo visivel em `mips_ge.c`.
`mips_ge_execute_command()` grava o argumento no banco de registradores GE.
Alguns comandos tambem alteram controle de lista, endereco de vertices,
matrizes, CLUT ou provocam draw; guardar um registrador nao significa que todas
as consequencias desse estado estao implementadas.

Comandos com tratamento de controle/semantica explicita identificados no
codigo:

| Codigo GE | Comando / grupo | Tratamento |
| --- | --- | --- |
| `0x00` | NOP | Ignorado como operacao. |
| `0x01`, `0x02` | VADDR / IADDR | Atualizam enderecos de vertices/indices. |
| `0x04` | PRIM | Envia primitiva/vertices ao renderer. |
| `0x05`, `0x06` | BEZIER / SPLINE | Constantes GE definidas; nao ha caminho explicito de tessellacao no renderer revisado. |
| `0x07` | BBOX | Teste de bounding box simplificado com transformacao. |
| `0x08`-`0x0B` | JUMP / BJUMP / CALL / RET | Controle de fluxo e pilha de display list. |
| `0x0C`, `0x0E`, `0x0F` | END / SIGNAL / FINISH | Termino/sinalizacao; sincronismo nao e completo. |
| `0x10` | BASE | Base de endereco para referencias GE. |
| `0x12` | VTYPE | Decodifica formato/stride e campos de vertices suportados. |
| `0x13`, `0x14` | OFFSET / ORIGIN | Base/offset para enderecos GE. |
| `0x3A`-`0x3B` | World matrix number/data | Carrega matriz world em componentes conforme sequencia. |
| `0x3C`-`0x3D` | View matrix number/data | Carrega matriz view. |
| `0x3E`-`0x3F` | Projection matrix number/data | Carrega matriz projection. |
| `0x42`-`0x47` | Viewport scale/center | Registradores armazenados e usados parcialmente na transformacao de vertices. |
| `0x48`-`0x4D` | Texture scale/offset e offset X/Y | Registradores armazenados; consumo/semantica parcial. |
| `0x9C`-`0x9F` | FBP/FBW/ZBP/ZBW | Estado de framebuffer/profundidade armazenado; nao faz render-to-guest-VRAM. |
| `0xA0`-`0xA7` | TBP/TBW | Endereco e pitch de textura, incluidos slots. |
| `0xB0`-`0xB1` | CLUT address | Endereco da paleta. |
| `0xB8`-`0xBF` | TSIZE | Dimensoes de textura. |
| `0xC0`-`0xC5` | Texture mode/format/CLUT | Estado e carga CLUT; formatos paletizados suportados no renderer. |
| `0xE0`-`0xE1` | Blend fix color | Registradores armazenados; fatores fixos nao suportados exatamente no blend atual. |

As demais palavras tambem podem atualizar o array de registradores; varias
funcionalidades dependem de o renderer consumir esse valor. O mapa nao e uma
declaracao de que todos os comandos PSP foram semanticamente implementados.
`mips_ge_run_list()` percorre a lista, com tratamento de controle, validacao e
instrumentacao; stalls, callbacks e sincronizacao ainda nao equivalem ao
hardware.

### 2.2 O que o caminho 2D realmente faz

Ha decodificacao de VTYPE e stride, leitura de vertices crus e envio de
primitivas ao OpenGL ES. O renderer converte pontos, linhas, line strips,
triangulos, triangle strips e triangle fans em modos GLES; sprites PSP sao
expandidos em triangulos. O modo `through` tem caminho separado de coordenadas.
Tambem existem upload de textura, CLUT4/8/16/32, paleta RGBA, unswizzle,
amostragem/shader de funcao de textura e apresentacao de framebuffer guest.

Isso permite que uma homebrew com caminho simples possa produzir draws
visiveis, mas nao prova que abertura de janela, todo o 2D PSP ou qualquer jogo
esteja totalmente funcional. O teste GLES sintetico valida componentes
isolados; os testes do pipeline nao substituem validacao visual no dispositivo.

### 2.3 3D: existe transformacao basica; nao existe equivalencia integral

As matrizes world/view/projection sao lidas e aplicadas no caminho de vertices.
O renderer tambem interpreta posicao, UV/cor, estado de profundidade e
viewport/offset em algum nivel; portanto, nao e correto dizer que o projeto
nao tem matrizes ou que nao consegue desenhar geometria 3D. O run historico do
Ozone em PURE_INTERPRETER mostrou geometria visivel em GLES, embora
distorcida/recortada.

O que falta para considerar o GE 3D confiavel:

* confirmar a matematica e convencao de matrizes, viewport, offset, clip,
  divisao por W e conversao para pixels em comparacao com o mesmo draw no
  PPSSPP;
* completar formatos de vertices, indices, pesos/skinning e morph targets.
  Caminhos nao suportados, como multiplos conjuntos morph, nao sao equivalentes
  a ler a geometria PSP inteira;
* implementar e validar corretamente testes/operacoes de profundidade,
  culling, clipping, stencil, fog, alpha/color tests e blending;
* completar combinacoes de textura/CLUT, modos de textura, filtros e
  coordenadas, alem de sincronismo de cache e VRAM;
* rasterizar ou manter coerencia dos resultados no framebuffer guest quando o
  jogo consulta, reutiliza ou transfere a imagem renderizada;
* comparar sequencia GE e resultados de pixels por draw com referencia.

Na captura GOW Run192, o draw #11 continha dados de textura CLUT8 swizzled
128x256 e paleta nao vazios; UVs amostravam uma regiao com glifo. Entretanto, o
estado calculado reportou coordenadas X aproximadamente 374-396 no campo
rotulado NDC, fora do intervalo GLES usual. Run193, executado em
HYBRID_AOT_INTERPRETER, tambem registrou coordenadas desse tipo e 11 fallbacks
de blending 10/10. Sao discrepancias concretas para investigar, nao uma
demonstracao isolada da causa de todo o quadro preto.

### 2.4 Framebuffer e apresentacao

`sceDisplaySetFrameBuf`/`mips_video_set_framebuffer()` apresentam a memoria
guest apontada pelo estado display. O pipeline GE faz draws diretamente na
superficie GLES. O codigo revisto nao rasteriza esses draws de volta a VRAM
guest no endereco `FBP/FBW`; o framebuffer que o guest leu ou apresentou pode
portanto divergir da imagem GLES visivel. Essa arquitetura afeta efeitos que
leem o framebuffer como textura, fades/copies, readback e transferencias GE.

Nos artefatos de Run193, 20 capturas de display guest 480x272 RGBA eram todas
zeradas enquanto o log registrava 318 enqueues GE. Run195 do Ozone tambem teve
draws visiveis e capturas do framebuffer guest zeradas. Estes resultados
historicos sustentam a separacao entre "desenho GLES" e "memoria guest", mas
nao identificam sozinhos qual superficie o jogo tentou usar em cada instante.
O proximo dado decisivo e uma captura sincronizada do mesmo frame: framebuffer
guest, FBP/FBW/formato de `SetFrameBuf`, estado do draw e `glReadPixels`.

#### Cadencia de frames e UVs 2D

O renderer aplicava `TEXSCALE`/`TEXOFFSET` tambem a coordenadas UV em modo
`through`, contrariando o decoder de referencia PPSSPP. O caminho `through`
agora preserva UV float/short e usa a conversao `/128` para UV byte; a regressao
GLES direcionada passou.

Tambem foi corrigido o ciclo de apresentacao: antes cada `sceGeListEnQueue()`
limpava e apresentava a superficie, embora jogos submetam varias listas antes
do VBlank. O trace de comparacao do 2048 contem 1.267 enqueues, tornando essa
cadencia um defeito concreto e compativel com a imagem intermitente. Depois,
Run222-224 mostraram sintomas visuais em GOW/Ozone/2048. O caminho real
`mips_video_draw_prim()` nao iniciava nem marcava um frame; alem disso o timer
fazia swap mesmo quando nenhum frame novo estava pendente, limpando a imagem
anterior e apresentando frames vazios. Agora o draw inicia o frame e o
presentador so troca a superficie quando ha um draw ou framebuffer atualizado.
O teste GLES foi fortalecido para exercitar o draw sem `begin_frame()` manual
e verificar que um VBlank ocioso preserva o backbuffer. Os tres testes GLES
direcionados e o build do interpretador passaram; a melhoria visual ainda
precisa ser confirmada nos jogos.

Nos Runs225-227 o usuario confirmou que o Ozone e o 2048 piscaram menos, mas
nao pararam; o GOW continuou piscando. O log do GOW terminou por timeout e os
outros dois foram fechados manualmente, todos sem fault. A inspeção do handler
`SetFrameBuf(sync=0)` encontrou mais uma troca prematura: ele fazia swap host
imediato antes de a lista GE seguinte concluir seus draws diretos para GLES.
Agora o ponteiro guest muda imediatamente, mas a troca da janela e adiada ao
VBlank, quando o draw GE pendente ja pode compor a imagem. Isso preserva a
semantica de selecao de scanout sem apresentar uma superficie intermediaria
incompleta. A build e os tres testes GLES passaram novamente.

Nos Runs228-230, o usuario observou duas piscadas iniciais no GOW e depois
estabilidade; no Ozone, flicker apenas no inicio e depois estabilidade; o 2048
continuou piscando ate o timeout. GOW/Ozone foram fechados manualmente (exit
0) e o 2048 encerrou pelo limite de 60 s (exit 124); todos fizeram shutdown
sem CPU fault e registraram 460/339/1.310 enqueues GE. A mudança de ciclo de
apresentação melhorou notavelmente GOW e Ozone; não reverter. O 2048 requer
diagnóstico específico de framebuffer, textura/alpha e sequência de draws,
pois a correção comum não removeu seu flicker.

A analise dos 20 dumps do Run230 mostrou que todos os buffers guest RGBA são
byte a byte zero, embora o log registre draws GE texturizados e o usuário tenha
visto elementos na tela. Isto confirma que o caminho GE atual desenha em GLES
sem escrever rasterização de volta à VRAM guest; os dumps guest não representam
o backbuffer apresentado e não explicam sozinhos o flicker. Foi adicionado
`MIPS_VIDEO_CAPTURE_PREFIX` para capturar até 20 backbuffers GLES imediatamente
antes do swap, com FBP/FBW/formato e contagem de draws associados no log. A
regressão verifica que a captura sincronizada contém o framebuffer esperado.
Próximo passo: capturar 2048 novamente e comparar essas imagens com a seleção
de buffer e os draws por apresentação antes de alterar o renderer.

Run231 confirmou o mecanismo do flicker: as capturas GLES alternam entre o
tabuleiro (50 draws e paleta com 10 cores) e quadros totalmente pretos (1 draw),
inclusive mantendo o mesmo FBP. O início de cada frame limpava a superfície e
recarregava a VRAM guest zerada, apagando os draws GLES anteriores antes de um
draw parcial/transparente. O renderer agora usa um alvo GLES persistente,
carrega o framebuffer guest quando `SetFrameBuf` o marca dirty e blita o alvo
para a janela no VBlank; a regressão verifica a persistência de um draw 2D após
apresentação. Os três testes GLES direcionados e o build do interpretador
passaram. O usuário fechou manualmente o Run231 (exit 0); não houve CPU fault e
o shutdown foi limpo. A validação visual da correção no 2048 ainda está
pendente.

No Run232, o usuário observou que as piscadas rápidas diminuíram, mas
continuaram. As capturas mostraram que os quadros pretos reapareciam quando o
jogo alternava entre `0x44000000` e `0x44088000`, cujos dumps guest são ambos
zero. O renderer persistente ainda tratava qualquer troca de endereço como
motivo para limpar e reimportar o framebuffer vazio. A importação passou a
comparar conteúdo e metadados, ignorando troca de FBP quando a imagem guest é
idêntica; a regressão cobre dois endereços com pixels equivalentes e preserva
draw GE.

Run233 confirmou a correção: o usuário relata que o flicker parou. As três
primeiras capturas são quadros pretos de inicialização, mas as capturas #5-20
contêm a composição do tabuleiro, inclusive durante alternância de FBP e
apresentações com apenas um novo draw. Os dumps guest continuam zerados; o
processo foi fechado manualmente (`exit=0`) e encerrou sem CPU fault. Isso
confirma a resolução do flicker recorrente neste teste, não que o 2D esteja
completo ou que o GOW esteja compatível. Próxima prioridade: fidelidade e
pixels remanescentes do 2048 e ampliar cobertura 2D GE.

Run215 confirmou mudanca visual no 2048: a captura do usuario mostra uma area
grande quadriculada em fundo preto, mas ainda nao o tabuleiro reconhecivel. O
log tem 2.737 enqueues GE e 1.369 chamadas registradas de `SetFrameBuf`; os 20
dumps guest continuam zerados. Run216 do GOW iniciou corretamente, configurou
framebuffer valido, registrou 1.586 enqueues GE e draws, mas a janela ficou no
cinza inicial e o processo nao teve shutdown normal antes do timeout. O log
nao registra sucesso de `sceDisplayWaitVblankStart*`, pois esses retornos zero
nao eram rastreados. Foi adicionado `MIPS_TRACE_VBLANK=1` para registrar os
primeiros oito waits e depois cada 60o, incluindo se havia frame pendente. O
draw #11 do GOW ainda tem vertices transformados para NDC X aproximadamente
374-396, muito fora da tela; nao atribuir a tela cinza a esse draw sozinho.

Run217 tambem terminou por timeout (exit 137), em `PURE_INTERPRETER`, com
1.585 submissões de listas GE e contador de draws chegando a 172.032, sem
registro de CPU fault. **O comando usado nao incluiu `MIPS_TRACE_VBLANK=1`;
isso foi omitido na instrucao anterior.** Portanto, Run217 nao permite dizer se
o VBlank foi chamado nem se `mips_video_present()` trocou a janela.

Run220 corrigiu o caminho do executavel e carregou
`umd0/PSP_GAME/SYSDIR/EBOOT.BIN`; terminou pelo timeout (exit 137), nao por
erro fatal registrado. Com `MIPS_TRACE_VBLANK=1`, o log nao tem nenhuma linha
`[HLE Display VBlank]`. Ainda assim, registrou 278 chamadas
`sceDisplaySetFrameBuf` e 281 submissões de lista GE. Portanto, durante os 45
segundos capturados, o jogo/emulador nao chegou ao handler HLE de espera de
VBlank. Como a unica rota atual a `mips_video_present()` e essa espera, nao
houve swap pelo caminho normal de apresentacao nessa captura. Isso explica por
que a janela pode ficar na cor inicial, mas nao estabelece se o jogo chega a
VBlank numa execucao mais longa nem por que a espera ainda nao ocorreu.

A comparacao direta do HLE de display com PPSSPP encontrou a divergencia
estrutural: no core local, `sceDisplaySetMode` apenas salva modo/dimensoes,
`sceDisplaySetFrameBuf` atualiza o endereco de scanout sem honrar `sync`, e
nao ha evento/timer autonomo de VBlank; a troca GLES depende de o jogo chamar
`sceDisplayWaitVblankStart*`. No PPSSPP, o VBlank e agendado pelo CoreTiming,
`sceDisplaySetMode` espera um VBlank e `sceDisplaySetFramebuf` distingue
buffer imediato de buffer latched para o proximo VBlank. Assim, o run confirma
que a apresentacao atual podia nunca ocorrer durante uma sequencia de draws
que nao chegava ao wait. Run221 foi um smoke test de 8 segundos depois da
implementacao: terminou por timeout (exit 124), mas o dispatcher, renderer e
audio fizeram shutdown sem CPU fault. O trace registrou VBlank autonomo (#1,
#3, #5 e #60), a espera feita por `sceDisplaySetMode` (contador 96),
`SetFrameBuf` com `sync=1` e alternancia repetida de buffers com `sync=0`,
alem de submissões GE. Isso confirma que os novos caminhos sao executados
pelo GOW; nao confirma que o frame tenha pixels corretos nem que a janela tenha
apresentado a imagem. Como o callback roda a cada 1024 dispatches, periodos
perdidos podem ser agrupados; o trace tambem pode omitir esses contadores
porque so registra os primeiros eventos e multiplos exatos de 60. O proximo
trabalho e validar a apresentacao visual e capturar o backbuffer GLES, nao
alterar a transformacao do draw #11 por tentativa.

Os 20 dumps de display do Run220 (480x272, stride 512, formato 3, alternando
`0x04000000`/`0x0408C000`) contem somente bytes zero e sao identicos. Isso
combina com a arquitetura atual, que desenha primitivas GE diretamente na
superficie GLES e nao rasteriza pixels de volta na VRAM guest; nao prova que o
backbuffer GLES esteja vazio. O dump do draw #11 confirma textura CLUT8 com
indices e paleta nao vazios, mas seus vertices resultam em NDC X≈374–396,
fora da tela. `tools/check_ge_coordinates.py` reproduziu corretamente as
etapas salvas; esse teste valida a conta local, nao uma captura executada no
PPSSPP. O draw #11 e um defeito/estado suspeito independente, ainda sem prova
de que seja a causa unica do sintoma visual.

### Auditoria dirigida do core e GE contra as referencias locais

Foram consultados os PDFs locais de Allegrex/COP1/COP2, Cache e Graphics
Engine, junto ao codigo PPSSPP de `sceDisplay`, `sceGe`, `VertexDecoderCommon`
e `TransformUnit`. A especificacao local descreve no GE hardware de clipping,
morphing, ate oito bones e tessellacao Bezier/B-spline; isso e bem maior que
o subconjunto atual do renderer.

Defeitos verificaveis no codigo atual:

1. Em vertex position 3D, S8/S16 sao usados como inteiros crus, enquanto o
   decoder PPSSPP normaliza por 1/128 e 1/32768. Em `through`, S8 tambem diverge:
   PPSSPP decodifica posicao zero; o renderer usa os bytes como coordenadas.
2. Desenhos com indices ou mais de um morph target sao descartados. Pesos e
   matrizes de bones nao sao aplicados ao vertice.
3. O estado de rasterizacao e reduzido: culling e sempre desligado, depth
   usa `LEQUAL` fixo, e nao ha pipeline GE para scissor, testes alpha/cor,
   stencil, logic-op, iluminacao/fog completos ou limpeza controlada por
   primitivas GE. Fatores de blend nao suportados caem em source-alpha.
4. `BEZIER`/`SPLINE` nao tem implementacao de tessellacao. Listas GE rodam de
   forma sincronica dentro de enqueue; sync/callbacks retornam sucesso
   simplificado, e `SIGNAL` nao entrega o mecanismo real de interrupt/callback.
5. O GE desenha direto no GLES, nao na memoria eDRAM/framebuffer PSP. FBP/FBW
   nao recebem os pixels dos draws, entao framebuffer feedback, transferencias
   e readback de superficies nao podem funcionar como no hardware.
6. O VBlank agora usa timer monotônico (~60 Hz), `sceDisplaySetMode` espera
   pelo próximo VBlank e `SetFrameBuf` diferencia `sync=0` (imediato) de
   `sync=1` (latched). Run221 confirma esses caminhos em execução no GOW, mas
   não valida imagem visível. O callback é consultado a cada 1024 dispatches;
   períodos perdidos são agrupados e não há entrega completa de interrupção,
   wakeup/reschedule de threads como no PPSSPP.

Os Run216/220 tem um problema adicional mensurado: o draw #11 transformado
fica fora do NDC esperado (X aproximadamente 374-396 no estado capturado).
Isso e compativel com vertices invisiveis, mas a cadeia precisa ser comparada
com PPSSPP no mesmo estado; o draw usa posicao float, portanto nao e explicado
pelo erro S8/S16 acima. No Run220, o trace habilitado nao registrou espera
VBlank, e portanto nao ocorreu swap pelo caminho normal durante a captura.
O bloqueio de apresentacao esta ligado a uma lacuna confirmada no HLE de
display: falta VBlank autonomo e `SetMode`/`SetFrameBuf` nao seguem a
sincronizacao do PPSSPP. Ainda falta medir a contribuicao de cada divergencia;
o estado fora de tela continua sendo um problema de geometria separado. Os
logs nao sustentam atribuir esse sintoma ao audio ou a uma falha de CPU.

### 2.5 Blending

O renderer implementa combinacoes limitadas de fatores GLES fixos. Fatores
fora do conjunto suportado usam fallback para
`GL_SRC_ALPHA/GL_ONE_MINUS_SRC_ALPHA`. O log historico reporta fator 10/10;
no PPSSPP, valor 10 corresponde a FIXA para o fator source e FIXB para o fator
destination. Os registradores GE de cor fixa existem, mas a saida atual nao
modela todos os fatores FIXA/FIXB com `glBlendColor` ou shader. Isso pode
alterar fades e composicao; ainda e necessario confirmar o impacto por pixels
e o estado do jogo.

## 3. Audio e SAS

### 3.1 Audio convencional / SDL2

`mips_audio.c/.h` fornece uma camada de audio host com SDL2, canais PSP,
reserva/liberacao, parametros basicos e envio de buffers PCM ao dispositivo.
O projeto trata buffers de audio como dados PCM para saida/mixagem. O modelo
nao substitui toda a fila, latencia, callbacks, formatos, frequencias e
sincronizacao do PSP.

### 3.2 `sceAudiocodec`

As APIs de consulta/configuracao, incluindo
`sceAudiocodecCheckNeedMem`/`sceAudiocodecGetEDRAM`, sao implementacoes de
contexto/valores. Nao ha evidencias de que os codecs PSP necessarios estejam
decodificando todos os streams suportados.

### 3.3 SAS Core

`mips_sas.c` acompanha estado de inicializacao, vozes, flags de fim, numero de
samples e parametros como reverb/volume em parte das chamadas. `__sceSasCore`
nao tem sintese PSP completa das vozes; o caminho pode zerar buffer e avancar
estado sem produzir as amostras da musica/efeitos. Faltam, conforme a API usada,
decodificacao de fontes, pitch, envelope, loop, key-on/off real, mixagem de
vozes, reverb e entrega correta do PCM.

Assim, a existencia de canais SDL2 nao prova audio de jogo. Historicamente,
runs mostraram chamadas frequentes de SAS; a prioridade deve vir de contagens
de imports novos e de uma captura audivel/PCM, nao apenas de um retorno zero.

## 4. CPU Allegrex e interpretador

### 4.1 Interface e fluxo

`mips_interpreter_step(MIPS_CPU *cpu)` executa um passo do interpretador.
`main_v35.c` configura `dispatcher.fallback_interpreter = interpreter_step`.
O dispatcher escolhe bloco AOT ou fallback, com excecoes para syscalls e
regioes especificas. Instrumentacao colocada apenas no interpretador nao
observa instrucoes que foram executadas em um bloco AOT.

### 4.2 Cobertura de opcodes (parcial)

O `switch(opcode)` tem caminhos para:

| Opcode primario | Grupo / cobertura observada |
| --- | --- |
| `0x00` | SPECIAL inteiro Allegrex, incluindo os functs relacionados abaixo. |
| `0x01`-`0x07` | REGIMM, J/JAL, branches condicionais usuais e alguns likely. |
| `0x08`-`0x0F` | Aritmetica/comparacao/logica immediate e LUI. |
| `0x10` | COP0 selecionado. |
| `0x11` | COP1: transfers, branches, aritmetica/conversao/comparacoes selecionadas. |
| `0x12` | COP2/VFPU transfers/controls. |
| `0x18`-`0x19` | Grupos de aritmetica vetorial VFPU. |
| `0x1B` | VFPU3: variantes de comparacao/min/max descritas abaixo. |
| `0x1C`, `0x1F` | SPECIAL2 e SPECIAL3 selecionados. |
| `0x20`-`0x2F` | Loads/stores integer selecionados, incluindo unaligned LWL/LWR/SWL/SWR e CACHE. |
| `0x31`, `0x39` | LWC1/SWC1. |
| `0x32`, `0x36`, `0x3E` | Loads/stores VFPU LV.S/LV.Q/SV.Q. |
| `0x34`, `0x37`, `0x3A`, `0x3C`, `0x3F` | Operacoes VFPU/conversoes/prefixos/controles selecionados. |

* **SPECIAL (`opcode 0x00`)**: shifts logicos/aritmeticos, JR/JALR, MOVZ/MOVN,
  SYSCALL/BREAK/SYNC, MFHI/MTHI/MFLO/MTLO, MULT/MULTU/DIV/DIVU, ADD/ADDU,
  SUB/SUBU, AND/OR/XOR/NOR, SLT/SLTU, CLZ/CLO em funct `0x16/0x17`,
  MADD/MADDU em `0x1C/0x1D`, MAX/MIN em `0x2C/0x2D` e MSUB/MSUBU em
  `0x2E/0x2F`. ROTR/ROTRV compartilham functs de SRL/SRLV e sao distinguidos
  pelos campos `rs`/`shamt`. O codigo atual tem cases dedicados para estes
  functs, conforme `mips_interpreter.c`.
* **REGIMM e branches**: branches condicionais e likely selecionados,
  saltos J/JAL e branches usuais.
* **Imediatos**: ADDI/ADDIU, SLTI/SLTIU, ANDI/ORI/XORI/LUI.
* **Loads/stores**: LB/LH/LW/LBU/LHU, SB/SH/SW, LWL/LWR/SWL/SWR e CACHE;
  LWC1/SWC1 e operacoes VFPU de load/store selecionadas.
* **COP0**: subset de move/controle e suporte necessario a excecoes/estado
  simulado, nao coprocessador PSP completo.
* **COP1/FPU**: transferencias GPR/FPR, branches FPU, aritmetica single,
  comparacoes e conversoes selecionadas; FCR31/arredondamento/excecoes nao
  estao provados completos.
* **SPECIAL2/SPECIAL3**: instrucoes selecionadas, como CLZ/CLO, MFIC/MTIC,
  EXT/INS e BSHFL. A implementacao de SPECIAL2 nao substitui o encoding correto
  de instrucoes que pertencem a SPECIAL.
* **VFPU/COP2**: instrucoes selecionadas listadas abaixo. Nao e um VFPU
  integral.

Os grupos primarios VFPU/COP2 com tratamento explicito incluem
`0x12`, `0x18`, `0x19`, `0x1B`, `0x32`, `0x34`, `0x36`, `0x37`,
`0x3A`, `0x3C`, `0x3E` e `0x3F`. Entre as operacoes codificadas estao
transferencias GPR/VFPU e controles, LV.S/LV.Q/SV.Q, prefixes, valores
imediatos, vadd/vsub/vdiv, vmul/vdot/vhdp, operacoes monadicas/conversoes e o
grupo VFPU3. VFPU3 `0x1B` inclui `vcmp`, `vmin`, `vmax`, `vscmp`, `vsge` e
`vslt`; a cobertura foi adicionada depois de runs historicos que registraram
avisos para esse opcode.

As funcoes VFPU usam arrays de 128 registradores e helpers para mapear
vetores/prefixos. Ainda ha operacoes com formato/dimensao simplificados,
limites de registrador lineares, diferencas de NaN/denormais e arredondamento
por testar. A palavra "grupo tratado" nao significa que todos os sub-opcodes,
prefixos, flags e excecoes desse grupo estejam corretos.

### 4.3 Falhas/ressalvas de ISA

O default do interpretador ainda pode emitir
`[Interpreter Warning] Unsupported ...`; portanto nao ha cobertura completa da
ISA. Historicamente o GOW em PURE_INTERPRETER registrou 1.022 avisos do opcode
primario `0x1B` antes da implementacao VFPU3. Run193 nao elimina esse problema
como evidencia, pois foi uma execucao hibrida/AOT. E necessario um run puro
posterior e, para validar a semantica, vetores comparados com PPSSPP.

O historico tambem encontrou bugs de classificacao de funct SPECIAL e rota
rotacoes; alguns foram corrigidos em etapa posterior. Para certificar a lista
final, usar os testes `test_allegrex_special.py`, `test_cop1_differential.py`,
`test_vfpu3.py` e uma matriz de cobertura produzida contra as tabelas do
PPSSPP. Ainda nao existe equivalencia geral instrucao-a-instrucao para todos
os jogos.

## 5. AOT estatico e ponte hibrida

### 5.1 Tradutor

`aot_recompiler_v5.py` define `MIPSInstruction` (decodifica PC/palavra e campos
opcode, rs, rt, rd, shamt, funct, immediates e target) e
`AOTRecompiler.translate_instruction(self, inst)`, que retorna codigo C e
comentario de assembly. O tradutor reconhece relocacoes HI16/LO16/J26 para
nao congelar constantes anteriores a relocacao.

O AOT traduz um subconjunto de instrucoes inteiras/MIPS, branches, loads/
stores e algumas operacoes COP0/COP1/SPECIAL2/SPECIAL3. O suporte cobre
codigos especificos gerados para o executavel GOW; nao traduz toda a ISA e
nao implementa VFPU em amplitude comparavel ao interpretador. O arquivo
`gow_core_aot.c` contem a imagem/blocos gerados para o alvo especifico.

### 5.2 Dispatcher e fallback

`MIPS_Dispatcher` contem uma tabela esparsa de `AOTBlockFunction` e um
`fallback_interpreter`. `mips_dispatcher_lookup()` devolve o bloco se
registrado; para endereco PSP valido sem bloco devolve o interpretador. A
imagem AOT e protegida por uma assinatura do executavel GOW para nao ser
aplicada a outro ELF/PBP carregado no mesmo endereco.

`main_v35.c` registra os blocos somente se a assinatura casar. O build
`FORCE_INTERPRETER` executa todo o codigo pelo interpretador. A versao hibrida
desvia syscalls importadas para o HLE e preserva fallback interpretado onde
nao ha bloco. Ha tratamento especial para codigo inicial, trampolins e uma
regiao helper do GOW em que o dispatcher forca o interpretador para evitar
divergencia de controle/delay slot observada anteriormente.

### 5.3 Limites de paridade

Interpretador e AOT nao partilham uma tabela semantica unica. As regras de
instrucao estao duplicadas entre o switch C e o gerador Python, entao uma
correcao no interpretador nao garante correcao no AOT. Validacao necessaria:

1. mesmos vetores de estado inicial/instrucao para o interpretador e AOT;
2. comparar GPR, HI/LO, FPR/FCR31, VFPU, memoria e PC bit a bit;
3. repetir com relocacoes aplicadas e delay slots;
4. executar jogos leves em interpreter e AOT e comparar HLE, lista GE e
   framebuffer;
5. executar GOW no interpreter antes de atribuir progresso a correcoes nao
   presentes no AOT.

## 6. Evidencias dos runs e estado do projeto

| Run / evidencia | Resultado comprovado | Interpretacao permitida |
| --- | --- | --- |
| Run192 (GOW, anterior a correcoes posteriores) | Leu `frontend_assets.bin` e `splash_screens.bin`; draw #11 tinha textura CLUT8/paleta com dados; transform reportada fora da faixa NDC; 12 fallbacks de blend 10/10. | A textura integralmente ausente/preta fica menos provavel; transform/blend ainda sao candidatos, nao causa raiz provada. |
| Run193 (GOW, HYBRID_AOT_INTERPRETER) | AOT ativo; 318 enqueues GE; 11 fallbacks blend 10/10; 20 buffers guest de display todos zerados; `faults=0`. | Execucao limpa nao significa render correto nem valida as correcoes recentes do interpretador. |
| Run195 (Ozone, PURE_INTERPRETER) | 455 enqueues GE, draws de geometria visiveis/distorcidos, guest framebuffer zerado, `faults=0`. | Existe output geometrico via GLES; GE 3D esta incompleto e output guest/GLES divergem. |
| Run194 (Cave History) / Run196-200 (2048) | Runs recentes nao chegaram a draws GE; 2048 leu `take.png` sem erro de I/O registrado; logs posteriores apontaram caminho de `abort()` em PNG e spin apos self-stop como investigacao. | Nao sao testes validos de renderer; a falha da homebrew 2048 esta no fluxo de inicializacao/PNG/HLE/CPU, mas a instrucao-raiz ainda exige conclusao. |
| Run212/214 (2048, PURE_INTERPRETER) | A correcao de `LWL/LWR/SWL/SWR` fez as tres scanlines capturadas coincidirem com o PNG de referencia; Run214 chegou a pelo menos 24.576 draws GE e encerrou com status zero. | A divergencia do PNG estava nas semanticas de merge das instrucoes Allegrex, nao em GE, audio ou leitura do arquivo. A validacao nao prova cobertura completa de todos os formatos/fluxos PNG. |
| Run213 (GOW, PURE_INTERPRETER, apos a correcao) | 20 buffers guest de display capturados continuam zerados; draw #11 ainda reporta X/NDC em torno de 374–396, fora da faixa usual. O log encerra renderer e audio sem erro fatal. | A correcao do CPU nao resolveu a imagem do GOW. O bloqueio observado agora e separado: framebuffer guest vazio e/ou estado/transformacao GE; requer comparacao do mesmo frame com PPSSPP. |
| Run222-224 (GOW, Ozone, 2048, PURE_INTERPRETER) | Todos terminaram por timeout (exit 124), shutdown normal e `faults=0`; registraram 764/524/1.302 enqueues GE. O usuario viu GOW com retangulo preto e restante piscando branco, Ozone com 3D distorcido piscando/linhas horizontais, e 2048 com tabuleiro visivel, uma casa branca, pixels falhos e flicker. | Os tres chegam ao renderer; a semelhanca do flicker justificou corrigir o ciclo de draw/present. A correção está implementada e tem regressão GLES, mas os sintomas específicos de geometria/linhas/pixels ainda precisam de nova validação visual e podem ser problemas GE independentes. |
| Run225-227 (após marcar draw e evitar VBlanks vazios) | GOW terminou pelo limite de 120 s e continuou piscando; Ozone e 2048 foram fechados manualmente e o usuário notou menos flicker, ainda presente. Os três mantêm atividade GE, sem faults. | A primeira correção melhorou parcialmente Ozone/2048, mas não resolveu GOW. Foi identificada e corrigida outra causa comum: o HLE fazia swap imediato ao `SetFrameBuf(sync=0)` antes dos draws GE subsequentes; agora o swap host é adiado ao VBlank. Próxima execução deve validar especificamente se isso elimina o flash intermediário. |
| CompareGE-20261015-174904 (2048, Ozone, PSPRev 1.5) | Os 20 dumps guest de 2048 e Ozone e os dois validos de PSPRev contem apenas zeros. O video de 2048 mostra o quadro 4x4 somente em poucos frames, no restante a janela fica preta. O draw #11 de 2048 mapeia vertices do quad para pixels (23,43) e (34,50), mas a textura capturada e um atlas cinza. O screenshot Ozone mostra geometria 3D ampliada/cortada e acinzentada; no draw #10, os quatro primeiros vertices ficam dentro do NDC, e a textura resolvida e um padrao preto/branco. PSPRev configurou framebuffer 0x04000000/stride 512/formato 5551, mas nao registrou draws GE e seus dumps ficaram zerados. | Os logs confirmam sintomas graficos, nao uma causa comum. Os dumps sao da VRAM guest e nao do backbuffer GLES; como o renderer desenha GE diretamente na superficie GLES, zeros neles nao provam que nao houve draw. O quad capturado do 2048 nao tem erro obvio de conversao de coordenadas; a causa da intermitencia ainda precisa ser isolada. PSPRev parece nao ter chegado a um draw GE utilizavel no periodo capturado. |

Run192 tem mais detalhe: a textura do draw #11 e CLUT8 swizzled 128x256,
continha 253 indices distintos (11.571 nao nulos) e 190 cores RGB distintas
depois do resolve. A imagem do atlas tem glifos/itens reconheciveis; isso
contradiz "textura inteira zerada", mas nao valida alpha, coordenadas, shader,
blend ou pixels finais. O dump antigo tambem repetia estado e tinha labels de
registradores que precisavam de correcao; nao tratar cada campo daquela
captura como fonte final de verdade.

Run192 registrou 17 imports `critical-unimplemented` no link (8 de
ThreadManForUser, 8 de `sceAudio` e 1 de `sceGe_user`) e 187 imports fallback
registrados ao todo (170 nao criticos). Um registro de import nao prova
chamada. Em Run193, o resumo de chamadas mostrou 728 chamadas a slots
fallback, com 638 de NID `0x79D1C3FA` (`sceKernelDcacheWritebackAll`).
Run195 registrou 906 chamadas ao NID `0x3F7AD767` (`sceRtcGetCurrentTick`).
Estes contadores dao prioridade para inspecao, mas nao provam sozinhos
causalidade visual.

Para o 2048, a imagem `TesterGames/2048PSP/data/take.png` passou verificacoes
de CRC dos chunks PNG e descompressao zlib. Runs196–208 confirmaram leituras
corretas, mas a saida de inflate divergia na terceira linha, byte 6. A
comparacao com PPSSPP localizou a causa nas semanticas little-endian de merge
de `LWL/LWR/SWL/SWR` em `mips_interpreter.c`; as quatro operacoes foram
corrigidas e ganharam regressao para todos os alinhamentos. Run212 confirmou
as tres linhas capturadas byte a byte, e Run214 chegou a renderizar milhares
de draws GE. A falha de PNG esta resolvida para esse caso; o caminho antigo de
abort/self-stop nao e mais o bloqueio atual do 2048.

`tools/STATUS.md` mantem o quadro confirmado/hipotese/descartado. Os scripts
`tools/summarize_log.py`, `tools/analyze_png_abort.py`,
`tools/check_ge_coordinates.py`, `tools/vram_to_png.py`,
`tools/imports_report.py`, `tools/blend_factor_table.py` e outros auxiliares
foram adicionados ao longo da investigacao para tornar os dados comparaveis.
Consultar cada script para formato de entrada/saida e nao inferir que sua
existencia prove que a causa foi corrigida.

## 7. Prioridade tecnica recomendada

1. Expandir a cobertura GE 2D e investigar fidelidade/pixels ainda incorretos
   no 2048; o flicker recorrente foi validado como resolvido no Run233.
2. Capturar simultaneamente janela/backbuffer GLES, framebuffer guest e estado
   GE do GOW no mesmo frame; identificar FBP/FBW/formato e onde os pixels
   desaparecem.
3. Comparar coordenadas/estado do draw #11 com PPSSPP e localizar o estagio
   matematico onde X/NDC sai da faixa esperada antes de alterar transformacoes.
4. Investigar o estado de blend FIXA/FIXB e outros estados GE somente com caso
   reproduzivel e comparacao de pixels.
5. Validar as instrucoes Allegrex/VFPU realmente usadas pelo GOW com testes
   diferenciais, incluindo a cobertura AOT/hibrida apos a correcao do
   interpretador.
6. Usar PSP Revolution e PSP Keys Test como testes diagnosticos adicionais,
   sem substituir o GOW como objetivo nem confundir inicializacao com render.
7. Completar HLE por contagem de imports chamados; priorizar tempo, cache/GE,
   SAS e sincronismo conforme evidencias, evitando stubs de sucesso.
8. Medir desempenho e validar no ARM64/R36S somente depois de consistencia
   funcional; build WSL/x86-64 nao prova build/execucao nativa ARM64.

## 8. Interfaces publicas de referencia

As assinaturas abaixo sao APIs C internas do emulador (nao prototipos C
oficiais do SDK PSP):

```c
/* mips_ge.h */
void mips_ge_init(MIPS_GraphicsEngine *ge);
void mips_ge_begin_list(MIPS_GraphicsEngine *ge);
void mips_ge_decode_vtype(MIPS_GraphicsEngine *ge, uint32_t vtype_val);
void mips_ge_get_vertex_layout(const MIPS_GraphicsEngine *ge,
                               GE_VertexLayout *layout);
int mips_ge_vertex_stride(const MIPS_GraphicsEngine *ge);
void mips_ge_execute_command(MIPS_GraphicsEngine *ge, MIPS_CPU *cpu,
                             uint32_t cmd_word);
void mips_ge_run_list(MIPS_GraphicsEngine *ge, MIPS_CPU *cpu,
                      uint32_t start_pc, uint32_t stall_pc);

/* mips_dispatcher.h */
typedef void (*AOTBlockFunction)(MIPS_CPU *cpu);
int mips_dispatcher_init(MIPS_Dispatcher *disp);
void mips_dispatcher_free(MIPS_Dispatcher *disp);
void mips_dispatcher_execute(MIPS_Dispatcher *disp, MIPS_CPU *cpu);

/* CPU / HLE */
void mips_interpreter_step(MIPS_CPU *cpu);
void mips_hle_syscall(MIPS_CPU *cpu, MIPS_HLE_Kernel *kernel);
void hle_syscall(MIPS_CPU *cpu);
```

APIs host adicionais declaradas publicamente:

```c
/* mips_video.h */
int mips_video_init(MIPS_VideoSystem *vid);
void mips_video_free(MIPS_VideoSystem *vid);
void mips_video_draw_prim(MIPS_VideoSystem *vid, MIPS_CPU *cpu,
                          MIPS_GraphicsEngine *ge, int prim_type,
                          int vertex_count);
void mips_video_set_framebuffer(MIPS_VideoSystem *vid, MIPS_CPU *cpu,
                                uint32_t topaddr, uint32_t stride,
                                uint32_t format, uint32_t width,
                                uint32_t height);
void mips_video_begin_frame(MIPS_VideoSystem *vid);
bool mips_video_present(MIPS_VideoSystem *vid);

/* mips_audio.h */
int mips_audio_init(MIPS_AudioSystem *audio);
void mips_audio_free(MIPS_AudioSystem *audio);
int mips_audio_reserve_channel(MIPS_AudioSystem *audio, int channel,
                               int sample_count, int format);
int mips_audio_set_channel_length(MIPS_AudioSystem *audio, int channel,
                                  int sample_count);
int mips_audio_release_channel(MIPS_AudioSystem *audio, int channel);
int mips_audio_output_panned(MIPS_AudioSystem *audio, int channel,
                             int left_vol, int right_vol,
                             const int16_t *samples, int length);
void mips_audio_mix(MIPS_AudioSystem *audio, uint8_t *stream, int len);
```

As funcoes de video/audio e handlers individuais adicionais estao declaradas
nos respectivos headers (`mips_video.h`, `mips_audio.h`,
`sceKernelThread.h`, `sceSysMem.h`). A tabela de NIDs em
`mips_linker.c` e a fonte de verdade para o mapeamento de imports; conferir
tambem o dispatcher especial de `main_v35.c` antes de classificar um import.

## Conclusao

O projeto possui mais do que um carregador: ha execucao Allegrex interpretada,
AOT parcial e especifica ao GOW, HLE com I/O/thread/memoria, processamento GE
parcial e renderer GLES que consegue desenhar geometria. Ainda assim, nao ha
base para declarar syscall-complete, ISA-complete, GE-complete, audio-complete
ou paridade interpreter/AOT. O bloqueio visual do GOW permanece associado a
lacunas demonstraveis no pipeline GE/framebuffer e a discrepancias de
transform/blend, mas a causa-raiz final ainda requer capturas sincronizadas e
comparacao diferencial.