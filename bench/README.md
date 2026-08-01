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
