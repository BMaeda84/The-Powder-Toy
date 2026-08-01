# Benchmark harness

Mede onde o tempo de um frame realmente vai, para que qualquer trabalho de
paralelização seja decidido por medição e não por intuição.

## Como rodar

O `FrameTime` do TPT só é instanciado quando a flag `DEBUG_FRAMETIME` está ligada,
e os tempos só aparecem no HUD de debug. O script liga a flag e o build grava um
CSV quando `TPT_FRAMETIME_CSV` aponta para um caminho.

```powershell
$d = "<pasta de trabalho>"
Copy-Item bench\bench_autorun.lua "$d\autorun.lua"
$env:TPT_FRAMETIME_CSV = "$d\frametime.csv"
$env:TPT_BENCH_FRAMES  = "900"
$env:TPT_BENCH_STEP    = "2"   # 2 = ~24k particulas, 1 = ~144k
.\build-tpt\powder.exe ddir $d
```

`bench_trace.txt` registra a população a cada 100 frames. **Sempre conferir esse
arquivo antes de acreditar em um CSV:** a primeira versão do cenário drenava até
zero por volta do frame 300 (matéria cruzava a borda e era destruída), e o
resultado descrevia uma simulação vazia com aparência plausível. As paredes de
contenção existem por causa disso.

O CSV descarta 120 frames de aquecimento, porque as durações são suavizadas
exponencialmente (alpha 0,05) e levam algumas dezenas de frames para convergir.
A população também leva um tempo para assentar, então as medianas devem ser
tiradas de uma janela estável (ver `bench_trace.txt`).

## Baselines (RTX 4080 / Ryzen 9 5950X, build debugoptimized, single-thread)

Medianas na janela estável, em milissegundos:

| fase | 24k partículas | 144k partículas |
|---|---:|---:|
| Frame time | 16,70 | 41,14 |
| GameModel::UpdateUpTo (simulação) | 7,12 | 27,91 |
| Simulation::UpdateParticles | 5,68 | 25,76 |
| Air::update_air | 0,70 | 0,66 |
| Simulation::RecalcFreeParticles | 0,63 | 1,33 |

Leitura: com carga real, `UpdateParticles` é 80–92% do trabalho de simulação e
escala com a população. `Air::update_air` custa ~0,7 ms **independente** da
quantidade de matéria, porque opera num grid de tamanho fixo — paralelizá-lo não
muda nada perceptível. Com 143.636 partículas (61% da capacidade) o jogo cai para
24 FPS, que é onde o custo passa a ser sentido.

## Alcance de interação (`TPT_REACH_CSV`)

Decomposição espacial do laço de partículas só é válida se o update de uma
partícula tocar uma vizinhança limitada. A sonda mede o deslocamento real por
frame (distância de Chebyshev, que é a métrica relevante porque um halo quadrado
é o que uma decomposição teria de reservar) e registra qual elemento produziu o
maior salto.

Medido no cenário denso:

| elemento | mediana do maior salto | pior salto |
|---|---:|---:|
| SAND / STNE (pós) | 3–4 px | 6 px |
| WATR | 30 px | 31 px |
| OIL | 68 px | **280 px** |

Em 38% dos frames houve salto acima de 64 px, sempre de OIL.

Limites vindos do código, não da medição:

- busca lateral de líquidos: `rt = 30` (a assinatura aparece nos dados — 18% dos
  frames têm salto máximo exatamente 30,00 px);
- `MAX_VELOCITY = 1e4` px por frame, ~16x a largura da tela: o clamp de
  velocidade **não** fornece limite espacial útil;
- `water_equal_test = 0` por padrão, então `flood_water` não explica os saltos
  longos observados; a causa é advecção pelo grid de ar (pressão do fogo);
- alcance genuinamente global, sem qualquer localidade: `WIFI` (canais em
  `wireless[]`), `PRTI`/`PRTO` (portais em `portalp[]`), `ARAY`/`CRAY`/`DRAY`
  (raios limitados só por `XRES`/`YRES`) e `EMP` (gatilho global).

Conclusão: não existe halo fixo que torne a decomposição correta. Pós e sólidos
são locais (≤ 6 px), líquidos parados cabem em 32 px, mas matéria comum advectada
por pressão cruza centenas de pixels num único frame. Excluir uma lista de
elementos especiais não basta — qualquer líquido pode ser arremessado.

## Determinismo (`TPT_RNG_SEED` + `TPT_CHECKSUM_CSV`)

Estágio 0 do `DESIGN_MULTITHREAD.md`, e pré-requisito de qualquer trabalho de
física neste fork: sem um valor comparável entre execuções não há como afirmar
que uma mudança preservou comportamento.

**O TPT não é reprodutível por padrão.** `RNG::RNG()` semeia com
`time(nullptr)`, então duas execuções da mesma cena divergem. `TPT_RNG_SEED` fixa
a semente; valor ausente ou zero mantém o comportamento original, e o jogo normal
não muda.

`TPT_CHECKSUM_CSV` grava, por frame, um FNV-1a de 64 bits sobre o estado:
partículas vivas (índice, tipo, posição, velocidade, temperatura, `life`,
`ctype`, `tmp`..`tmp4`, `flags`, `dcolour`) e os grids `pv`, `vx`, `vy`, `hv`.
Slots mortos ficam de fora, porque guardam lixo da vida anterior e mediriam o
alocador, não a física. O grid de ar entra porque realimenta o movimento no frame
seguinte: uma divergência só nele apareceria depois, nas partículas.

Floats entram pelos bits, não pelo valor — o objetivo é justamente pegar
diferença de último bit vinda de reordenação de operações.

### Resultado medido

Cena de 24k partículas, 300 frames:

| execuções | resultado |
|---|---|
| 3× com `TPT_RNG_SEED=12345` | os 300 frames idênticos, checksum final `7d0c66a214696a08` |
| 2× sem semente | divergem **no frame 1** (`c90a13a0…` vs `6a7e6330…`) |

Na divergência sem semente a população é a mesma nos dois lados no frame 1, o que
localiza a causa no RNG e não em criação/destruição de matéria.

O uso prático é esse: o arnês aponta o **primeiro** frame que diverge, em vez de
só dizer que o resultado final ficou diferente.

```powershell
$env:TPT_RNG_SEED     = "12345"
$env:TPT_CHECKSUM_CSV = "$d\checksum.csv"
```

## RNG por partícula (estágio 1)

O laço compartilhava um único `Simulation::rng`. Os números que cada partícula
recebia dependiam de quantas chamadas as anteriores tinham feito — ou seja, da
ordem de visita. Sob threads isso é corrida de dados e destrói a
reprodutibilidade.

Agora cada partícula é semeada por `(currentTick, índice)` via `RNG::seedFrom`,
que usa splitmix64. O misturador não é enfeite: xoroshiro128+ tem qualidade ruim
nos primeiros valores quando semeado com estados próximos, e `seed()` faz
`s[0] = s[1] = sd`, que é exatamente o caso ruim. Como aqui as sementes são
vizinhas por construção (índices consecutivos), usar `seed()` produziria
correlação visível entre partículas adjacentes.

O estado global de `rng` é salvo e restaurado em volta do laço, porque
`BeforeSim`, `CheckStacking` e as ferramentas seguem num fluxo sequencial próprio.

### Consequência de desenho

O fluxo passa a ser função apenas da **identidade** da partícula. Logo o
resultado independe da ordem de visita, do particionamento em faixas **e do
número de threads**. Isso corrige o que o `DESIGN_MULTITHREAD.md` dizia
originalmente sobre número de threads virar parâmetro do save: com semeadura por
partícula, não vira.

### Verificação

| teste | resultado |
|---|---|
| 3 execuções, semente 12345, 300 frames | idênticas: `8eba5a5720c39d06` |
| checksum vs. estágio 0 | diferente (`7d0c66a2…`), como esperado |
| trajetória de população vs. estágio 0 | idêntica até o frame ~250; divergência máxima de 13 partículas (0,036%) |
| custo em `UpdateParticles` (cena densa) | não mensurável acima do ruído |

Sobre o custo: uma execução isolada sugeriu −3,4%, o que seria um ganho. Três
execuções mostraram dispersão de **9,6%** entre si, contra uma diferença de
+0,3% em relação à baseline. Não há base para afirmar ganho nem custo.

**Calibração importante para os próximos estágios:** o ruído execução a execução
em `UpdateParticles` é da ordem de 10%. Qualquer ganho reivindicado no estágio 4
precisa ficar bem acima disso, e medido com repetições, para ser levado a sério.

## Lista livre segmentada (estágio 2)

`pfree` era um único LIFO encadeado por `data[i].life`. `Alloc` e `Free` vindos de
threads diferentes corromperiam o encadeamento, e qualquer matéria comum aloca e
libera — o perigo não é excluível por tipo como os elementos de alcance global.

Agora há uma lista por segmento. `Free` devolve ao segmento **de quem chamou**, não
a um derivado do índice: assim uma thread nunca escreve na lista de outra, mesmo
matando partícula criada por outra. Se o segmento próprio estiver vazio, `Alloc`
varre os demais em ordem fixa antes de crescer `active` — sem esse resgate, slots
liberados por outra thread ficariam encalhados e a simulação bateria no teto de
partículas com memória sobrando.

`TPT_FREELIST_SEGMENTS` define a contagem (padrão 1, idêntico ao original) e
`TPT_FREELIST_BANDING=1` atribui o segmento pela faixa vertical da partícula,
simulando em série o que o estágio 4 fará com uma thread por faixa.

### O teste que quase passou sem testar nada

Com o cenário de bench, todas as configurações davam o mesmo checksum e
integridade perfeita. Parecia aprovado. Os contadores `local_alloc` e `rescue`
mostraram **`alloc_local = 0`**: as ~36k partículas vieram todas de incrementar
`active`, a lista livre nunca foi usada para alocar, e portanto o código novo
jamais rodou.

Daí `churn_autorun.lua`, que cria e destrói matéria todo frame em faixas
rotativas, mantendo a população estável para forçar alocação vinda da lista.

### Verificação

| teste | resultado |
|---|---|
| 1 segmento, cena de bench | checksum idêntico ao estágio 1 (`8eba5a5720c39d06`) |
| cena de rotatividade, 1 segmento | 13.305 alocações pela lista livre |
| cena de rotatividade, 8 segmentos + banding | 13.432 alocações; partição realmente exercitada |
| 8 segmentos, 2 execuções | idênticas nos 300 frames |
| integridade, todas as execuções | 0 frames com corrupção em 300 |

O checksum com banding difere do de 1 segmento (`89f80832…` vs `783ea282…`), como
esperado: a atribuição de slots muda. A população fica em 3.883 contra 3.893,
diferença de 0,26%.

**Lacuna assumida:** `rescue = 0` em todas as execuções. O caminho de resgate entre
segmentos nunca disparou, porque cada faixa sempre achou slot na própria lista.
É bom sinal para a contenção esperada no estágio 4, mas significa que esse ramo
continua **sem teste**. Precisa de um cenário com faixas assimétricas — uma que
só destrói e outra que só cria.

## Alcance de leitura (análise estática)

O `pmap` é `int[YRES][XRES]` cru e os elementos o recebem como `int (*)[XRES]`
(ver `UPDATE_FUNC_SUBCALL_ARGS`), então não há como instrumentar as leituras com
um wrapper sem mudar a assinatura de update de todos os elementos. Os números
abaixo vêm de extração estática dos limites de laço, **não** de medição — a
diferença importa e há risco de padrão não capturado.

Varredura de vizinhança centrada, 99 dos 195 elementos:

| raio | elementos |
|---:|---:|
| 1 px | 62 |
| 2 px | 36 |
| 4 px | 1 (STKM) |

Ou seja: a esmagadora maioria lê no máximo um 5x5. Os limites longos são poucos e
nomeáveis:

- `DTEC`: raio de `tmp2`, **limitado a 25 px** pelo próprio código;
- busca lateral de líquidos no `Simulation.cpp`: `rt = 30`, que é leitura tanto
  quanto escrita;
- `LDTC`: varre em 8 direções e o próprio comentário diz *"tmp is the number of
  particles that will be scanned before scanning stops. Unbounded if 0"* — 0 é
  valor válido, portanto **ilimitado**;
- `ETRD`: procura a partícula sparkável mais próxima com
  `maxDistance = hypot(XRES, YRES)`, isto é, **a diagonal da tela inteira**;
- `ARAY`/`CRAY`/`DRAY`: caminhada direcional até encontrar bloqueio, limitada só
  por `XRES`/`YRES`;
- `WIFI`, `PRTI`/`PRTO`, `EMP`: sem localidade nenhuma por design.

### A assimetria que define o desenho

Leitura e escrita quebram a localidade por motivos diferentes, e isso muda o que
é possível:

- as leituras longas vêm de um conjunto **enumerável** de elementos-máquina
  (`LDTC`, `ETRD`, raios, `WIFI`, portais, `EMP`), que são raros numa cena e
  podem ser mandados para um passe serial por tipo;
- as escritas longas vêm de **matéria comum** advectada por pressão (o óleo dos
  280 px), e portanto não podem ser excluídas por tipo.

Logo, qualquer decomposição precisa de duas regras diferentes: lista de exclusão
por tipo para leitura, e despacho por deslocamento pretendido para escrita.
