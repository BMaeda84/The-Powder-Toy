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
