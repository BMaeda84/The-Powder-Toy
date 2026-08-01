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
| Simulation::UpdateParticles | 5,68 | 25,90 |
| Air::update_air | 0,70 | 0,66 |
| Simulation::RecalcFreeParticles | 0,63 | 1,33 |

O valor denso de `UpdateParticles` foi corrigido de 25,76 para 25,90 ms: o CSV
antigo tem spans condicionais e 88 das 780 linhas possuem uma coluna extra. A
extração original por nome deslocava o campo nesses frames; ancorar
`UpdateParticles` pela penúltima coluna preserva o valor correto. A conclusão
não muda: com carga real, `UpdateParticles` é 80–92% do trabalho de simulação e
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
  longos observados; a causa confirmada depois é a troca repetida de posição
  com partículas processadas mais tarde no mesmo frame;
- alcance genuinamente global, sem qualquer localidade: `WIFI` (canais em
  `wireless[]`), `PRTI`/`PRTO` (portais em `portalp[]`), `ARAY`/`CRAY`/`DRAY`
  (raios limitados só por `XRES`/`YRES`) e `EMP` (gatilho global).

Conclusão: não existe halo fixo que torne a decomposição correta. Pós e sólidos
são locais (≤ 6 px), líquidos parados cabem em 32 px, mas OIL leve foi
reposicionado centenas de pixels por trocas sucessivas com líquidos mais densos.
Esse destino depende das partículas processadas depois dele, logo não está
disponível na classificação anterior ao update.

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

## Classificação para decomposição (estágio 3) — RESULTADO NEGATIVO

Estágio desenhado para matar ou aprovar o projeto barato, medindo que fração da
matéria cairia num passe serial. `TPT_CLASSIFY=1` classifica cada partícula em
paralela, serial-por-tipo (leitor ilimitado) ou serial-por-movimento (deslocamento
previsto acima do halo de 32 px), e conta os **mispredicts**: partículas dadas
como paralelas que depois andaram mais que o halo.

A fração serial não é o número que decide. O que decide é o mispredict, porque
**um único** basta para corromper estado numa execução paralela. O critério é
zero, não "poucos".

### Medido (cena densa, 143,6k partículas, 900 frames)

| preditor | fração serial | mispredicts |
|---|---:|---:|
| velocidade de entrada | 0,00% | **2.108** em 444 frames |
| velocidade + advecção do ar | 0,00% | **2.108** em 444 frames |

Idênticos até o dígito. A advecção não explica nada, e a fração serial é zero
porque `serialByMove` quase nunca dispara: as partículas que se deslocam centenas
de pixels têm velocidade prevista abaixo de 19 px/frame.

### Por que o despacho por deslocamento não funciona

O deslocamento longo **não vem de velocidade**: as partículas que se deslocam
centenas de pixels têm velocidade prevista abaixo de 19 px/frame, e incluir o
termo de advecção do ar não mudou um único mispredict. A classificação teria de
acontecer antes do update, quando o destino ainda não existe.

### Quem causa (medido, `TPT_MISPREDICT_CSV`)

| elemento | mispredicts | pior salto | na assinatura de 30 px |
|---|---:|---:|---:|
| OIL | 2.108 | 260 px | **0** |

Um único elemento, e **zero** saltos na janela de 30 px.

**Isto refuta a explicação anterior deste documento.** A hipótese registrada era
que a busca lateral de líquidos (`rt = 30`) realocava a partícula. Se fosse ela,
os saltos se concentrariam em 30 px e apareceriam também em WATR, que é
`Falldown = 2` com `Advection` e `AirDrag` idênticos aos do OIL. Nenhuma das duas
coisas acontece: nenhum salto em 30 px, e WATR não aparece.

### Mecanismo confirmado por instrumentação direta

A sonda seguinte instrumentou exatamente o bloco final de troca em `try_move`.
Nessa execução, os **4.467 de 4.467** mispredicts de OIL haviam sido trocados de
posição: 71.501 trocas no total, máximo de **51 trocas para a mesma partícula em
um frame**. A soma das distâncias das trocas foi 3,234x o deslocamento líquido.

Logo, o mecanismo é a troca repetida com partículas mais densas processadas
depois do OIL no mesmo frame. O destino líquido não existe quando a classificação
pré-update teria de decidir o passe, portanto não há preditor conservador baseado
apenas no estado de entrada que implemente o desenho original.

O que está estabelecido por medição, e basta para a decisão de projeto: o
deslocamento por frame não é previsível a partir do estado disponível antes do
update, então a regra de despacho por deslocamento não é implementável como
desenhada.

Isso invalida a regra de despacho por deslocamento proposta em
`DESIGN_MULTITHREAD.md`. O que sobra:

- **halo ≥ 280 px** (o pior salto medido): com `XRES = 612` cabem 2 faixas, o que
  não é paralelismo;
- **líquidos inteiros no passe serial**: em cena típica de TPT são fração grande
  da matéria, e o teto de ganho cai junto;
- **mudar o movimento de líquidos** para não teleportar: deixa de ser
  paralelização e passa a ser mudança de física, com o comportamento de água e
  óleo alterado de forma visível.

Nenhuma dessas é o projeto que estava desenhado.

### Ressalva sobre a reprodutibilidade do contador

O total de mispredicts passou de 2.108 para 4.467 entre execuções com a mesma
semente e o mesmo preditor, embora a sonda nova só incremente contadores. A causa
dessa divergência continua desconhecida. Ela impede comparar a taxa absoluta
entre esses dois runs, mas não muda a atribuição do mecanismo dentro do run
instrumentado: todos os 4.467 casos tinham troca registrada.

## Quebra de custo de `UpdateParticles` (`TPT_UPDATE_COST_CSV`)

Esta sonda mede os dois limites que restaram depois do resultado negativo do
estágio 3, sem criar threads e sem alterar a ordem ou a física. É inativa por
padrão e grava um CSV de schema fixo quando recebe um caminho. "Inativa" aqui
significa sem relógio, alocação ou I/O: o binário ainda contém os desvios baratos
da sonda, e não foi feita uma comparação contra um binário anterior à
instrumentação.

```powershell
$env:TPT_UPDATE_COST_CSV = "$d\update-cost.csv"
$env:TPT_UPDATE_COST_SAMPLE_STRIDE = "32" # padrão: 32
```

Durante esta medição, `TPT_REACH_CSV`, `TPT_CLASSIFY`, `TPT_CHECKSUM_CSV` e
`TPT_MISPREDICT_CSV` ficaram desligados. Essas sondas fazem trabalho dentro ou
ao redor do mesmo laço e contaminariam o custo.

### O que exatamente é contado

**Eixo A — classe de matéria.** A classe é congelada pelo tipo na entrada da
iteração, que é o único estado disponível para um despacho real. A classificação
usa a máscara oficial `Properties & STATE_FLAGS`, não `Falldown`:

- candidato local: `TYPE_PART` mais `TYPE_SOLID`, excluindo os leitores sem
  limite espacial já conhecidos;
- fluido: `TYPE_LIQUID` mais `TYPE_GAS`;
- resíduos explícitos: sólidos especiais, energia, atores, estado inesperado,
  slots mortos e custo fixo fora do laço.

O relógio só é lido quando a classe muda entre slots consecutivos. Assim todos
os `continue` antecipados entram em alguma categoria, sem um span caro por
partícula. `solid_special` fica separado porque somá-lo ao candidato inflaria o
teto do caminho 2 com máquinas que já se sabe que precisam de passe serial.

**Eixo B — movimento genérico contra restante.** O limite sintático é a única
chamada a `MovementPhase`: ela contém `PlanMove`, todos os `do_move`/`try_move`,
reflexão e as duas buscas laterais. O custo dessa chamada é amostrado de forma
rotativa por índice e por frame. Os dois timestamps ficam diretamente ao redor
da chamada no call site; retornos de helpers, branches e contadores da sonda
ficam fora do intervalo. Cada duração observada é expandida pelo stride, e a
janela cobre ciclos inteiros de todos os strides usados. O complemento é
calculado por subtração e chama-se `update_remainder`.

Esse nome é deliberado. O complemento **não é uma fase comprovadamente sem
movimento nem segura para paralelizar**: `Element::Update` roda antes do corte,
109 callbacks de elementos chamam criação, morte ou mudança de tipo, e `WARP`
troca posições e escreve `pmap` diretamente. A condução de calor também escreve
a temperatura de vizinhos. Portanto o eixo B é um teto temporal; a fração
realmente independente só pode ser menor.

O writer roda em `AfterSim`, fora do intervalo de `UpdateParticles`. O CSV inclui
contagens de classe e de caminho, amostras cruas, `class_coverage_error_ns` e os
resíduos necessários para refazer a conta.

### Método

- build `debugoptimized`, MSVC, `-j 3`;
- cena densa, semente 12345, 900 frames, população estável de 143.636;
- janela 640–895, 256 frames por execução; 256 é múltiplo de 16, 32 e 64, então
  todas as fases da amostragem rotativa aparecem o mesmo número de vezes;
- 3 pares intercalados, estritamente sequenciais, controle com sonda desligada e
  medição com sonda ligada;
- stride 32 nos três runs principais; runs adicionais com 16 e 64 para testar
  convergência da amostragem;
- percentuais calculados pela razão das somas na janela, não por média de médias.

No MSVC desta máquina, `steady_clock` usa QPC e os pares vazios deram mínimo e
p50 de 0 ns por quantização — por isso esses dois valores não corrigem nada. A
sonda mede 64 lotes de 2.048 pares consecutivos e usa a mediana das médias dos
lotes, resistente a preempções ocasionais. O baseline ficou entre 19,775 e
19,971 ns nos três runs. A correção subtrai do agregado cru de movimento
`baseline × amostras` antes de multiplicar pelo stride. O CSV preserva tanto a
estimativa crua quanto a calibrada e os agregados crus por classe.

O controle atual mediu `UpdateParticles` em **26,234 ms** (26,178–27,392 ms;
dispersão 1,213 ms ou 4,63%) e a simulação em 27,987 ms. A sonda acrescentou
6,86% na mediana dos pares (5,46–7,16%). Por isso os milissegundos abaixo usam o
total do controle e as frações da sonda, em vez de chamar o custo do observador
de trabalho físico. Uma tentativa intermediária abriu processos concorrentes;
ela foi integralmente descartada e os dados publicados vêm apenas dos runs
sequenciais. Os dados e contadores por run estão em
`update_cost_dense_summary.csv`.

### Eixo A — pós/sólidos contra líquidos/gases

| run | pós + sólidos locais | líquidos + gases | outros + não atribuído |
|---|---:|---:|---:|
| 1 | 39,9214% | 60,0663% | 0,0123% |
| 2 | 39,9751% | 60,0145% | 0,0104% |
| 3 | 40,3305% | 59,6599% | 0,0096% |
| **mediana** | **39,9751%** | **60,0145%** | **0,0104%** |
| dispersão (máx − mín) | 0,4091 p.p. (1,02%) | 0,4064 p.p. (0,68%) | 0,0027 p.p. |

Normalizado aos 26,234 ms do controle: **10,487 ms** ficam no candidato
pós/sólidos, **15,744 ms** em líquidos/gases e 0,003 ms no restante.

Na janela estável a cena contém 75.096 partículas `TYPE_PART` e 68.540
`TYPE_LIQUID`. Não há `TYPE_SOLID`, gás vivo ou leitor especial nessa cena —
`STNE` é classificado pelo TPT como pó. Portanto os 40,0% são um teto para esta
carga específica, não uma alegação sobre saves dominados por máquinas sólidas.

### Eixo B — `MovementPhase` contra restante do update

| run | movimento cru | movimento calibrado | `update_remainder` calibrado |
|---|---:|---:|---:|
| 1 | 54,1837% | 44,0322% | 55,9678% |
| 2 | 54,3620% | 43,9938% | 56,0062% |
| 3 | 53,6066% | 43,7979% | 56,2021% |
| **mediana** | **54,1837%** | **43,9938%** | **56,0062%** |
| dispersão (máx − mín) | 0,7554 p.p. | 0,2343 p.p. (0,53%) | 0,2343 p.p. (0,42%) |

A coluna crua demonstra por que calibrar o intervalo curto é obrigatório: ela
superestima movimento em cerca de 10 p.p. Normalizado ao controle, **11,541 ms**
estão no movimento genérico calibrado e **14,693 ms** no restante, usando
alocação proporcional do overhead. Nos extremos — todo overhead fora ou dentro
do candidato — a fração física de `update_remainder` fica entre 52,81% e 60,06%.

Os runs de convergência deram 44,7660% de movimento com stride 16 e 44,6512%
com stride 64. Eles ficam 0,62–0,97 p.p. acima do intervalo principal, portanto
são concordância aproximada, não sobreposição. A dispersão total entre os cinco
runs é 0,9681 p.p. (2,20% do movimento) e não muda a decisão.

### Prova de cobertura e de exercício

- `class_coverage_error_ns = 0` em todos os 768 frames da janela dos três runs;
- soma dos buckets de classe + `fixed_ns` = `update_ns`; o resíduo mediano do
  eixo A é só 0,0104%;
- população = 143.636 nos checkpoints 200–900 de todos os seis runs principais
  e dos dois runs de convergência;
- por frame estável: 143.636 chamadas de `MovementPhase`, pelo menos 327.997
  chamadas de `do_move` e de `try_move`, 684.138 passos de busca lateral e
  45.892 callbacks de elemento;
- a busca lateral vertical foi exercitada; a variante de gravidade arbitrária
  ficou em zero porque o cenário usa gravidade vertical — lacuna registrada;
- nenhum slot mudou de classe entre a entrada e `MovementPhase` nessa cena;
- controle e sonda produziram os mesmos 300 checksums em uma validação separada,
  com hash final `fc72af49e35cd27e` e população 143.636.

### Tetos e decisão

Aplicando Amdahl aos percentuais medidos:

| caminho | fração candidata | 8 threads, `UpdateParticles` | threads infinitas, `UpdateParticles` | 8 threads, simulação | threads infinitas, simulação |
|---|---:|---:|---:|---:|---:|
| (2) só pós/sólidos locais | 39,98% | 1,54x | **1,67x** | 1,49x | **1,60x** |
| (3) só `update_remainder` | 56,01% | 1,96x | **2,27x** | 1,85x | **2,11x** |

Como limite conservador, todo o overhead observado também foi cobrado do bucket
candidato e o maior resultado entre os runs foi normalizado ao controle. O
caminho 2 chega só a 43,10% (1,76x infinito em `UpdateParticles`). O caminho 3
pode chegar a 60,06%; mesmo esse cenário generoso dá 2,11x em oito threads para
`UpdateParticles`, mas apenas **1,97x na simulação inteira**.

Portanto o caminho 2 está descartado pelo teto. O caminho 3 **não** está
descartado pelo teto infinito, mas falha o alvo de 2x com oito threads e seu
bucket não é a fase segura descrita na hipótese: parte dele escreve `pmap`, cria,
mata ou move partículas e teria de permanecer serial depois de uma auditoria
real dos callbacks. Com a fração central, seriam necessárias 22 threads
ideais só para cruzar 2x na simulação; sincronização e a redução do bucket seguro
ainda diminuiriam isso. Basta a auditoria retirar 2,7 p.p. do update — menos de
5% do bucket — para até o teto infinito da simulação voltar a ficar abaixo de
2x.

Recomendação: se o objetivo de pelo menos 2x em oito threads continuar, seguir
com **(1) detecção de conflito com rollback** é o único caminho não limitado por
esta quebra de custo. Isso não o aprova: o custo de detectar, registrar e repetir
conflitos ainda precisa ser medido, e as trocas repetidas de OIL mostram que o
pior caso é real. O caminho 3 merece no máximo uma auditoria de segurança antes
de qualquer thread, caso se aceite um alvo menor ou muito mais paralelismo. Se a
complexidade de rollback não for aceitável para o mod, parar aqui continua sendo
a decisão fundamentada.

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
