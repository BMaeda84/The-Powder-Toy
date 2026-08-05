# Leitura fora dos limites em `set_emap`, na linha-célula 0

## O que é

`Simulation::set_emap` (src/simulation/Simulation.cpp) propaga energia por paredes
elétricas. O ramo que desce para `y+1` tem uma cadeia de OR cujo terceiro termo
deveria blindar os acessos a `y-1`:

```cpp
if (x==x1 || x==x2 || y<0 ||
        is_wire(x-1, y+1) || is_wire(x+1, y+1) ||
        is_wire(x-1, y-1) || !is_wire(x, y-1) || is_wire(x+1, y-1))
```

O ramo espelhado, que sobe para `y-1`, escreve a guarda equivalente como
`y>=YCELLS-1` — e ela funciona, porque dispara antes de os termos lerem `y+1`
fora do mapa. Aqui os termos leem `y-1`, então a guarda teria de disparar em
`y == 0`: precisa ser **`y<1`**. Escrita como `y<0`, ela nunca é verdadeira, e
`is_wire(x-1, y-1)` acaba lendo `bmap[-1][x-1]`.

O mesmo `y<0` aparece em `FloodINST`. Lá **não** é acesso fora dos limites, porque
`pmap` tem a borda de `CELL` para absorver o índice. Não foi tocado aqui.

## Por que passou despercebido

`bmap` é `unsigned char[YCELLS][XCELLS]`, sem linha de borda, e em
`RenderableSimulation` vem logo depois de `hv`:

```cpp
float vx[96][153], vy[96][153], pv[96][153], hv[96][153];
unsigned char bmap[96][153], emap[96][153];
```

`hv` ocupa 96·153·4 = 58.752 bytes e `bmap` tem alinhamento 1, então `bmap` começa
exatamente onde `hv` termina. `bmap[-1][x]` não sai da página nem quebra: cai nos
últimos 153 bytes do **mapa de calor ambiente**. Sem crash, sem aviso — só um byte
arbitrário decidindo um `if`.

## O efeito observável

Endereço de `bmap[-1][x]`, em bytes dentro de `hv`: `58752 - 153 + x`.

| acesso | byte em `hv` | float | posição |
|---|---:|---:|---|
| `bmap[-1][14]` | 58.613 | 14.653, byte 1 | `hv[95][118]` |
| `bmap[-1][15]` | 58.614 | 14.653, byte 2 | `hv[95][118]` |
| `bmap[-1][16]` | 58.615 | 14.653, byte 3 | `hv[95][118]` |

Os três caem no mesmo float: linha 95, coluna 118 — o canto inferior direito.

Para a cadeia de OR ficar falsa é preciso `bmap[-1][14]` não-fio, `bmap[-1][15]`
fio e `bmap[-1][16]` não-fio. O valor **131,0** serve: em IEEE-754 é `0x43030000`,
que em little-endian dá os bytes `00 00 03 43` — byte 1 = 0 (não-fio), byte 2 = 3
(`WL_DETECT`, fio), byte 3 = 0x43 = 67 (não-fio).

Resultado medido, 3 frames, build **sem modificação** do master:

| configuração | `emap[0][12]` | `emap[0][15]` | `emap[1][15]` |
|---|---:|---:|---:|
| base | 14 | 14 | **14** |
| calor ambiente 131,0 em (118,95) | 14 | 14 | **0** |

A linha 0 é idêntica nas duas. Só o fio de baixo muda: **a temperatura ambiente
num canto do mapa decide se uma parede elétrica na linha do topo conduz para
baixo**, a 95 linhas de distância e sem contato nenhum.

O valor 131,0 depende do layout do objeto (MSVC x64, este build). Em outro
compilador o byte que cai no lugar muda, e o valor teria de ser recalculado — mas
a leitura fora dos limites em si não depende de layout.

## Como reproduzir

Precisa só de um build limpo do upstream; nada de patch.

```powershell
Copy-Item repro-wire-oob\autorun.lua "<pasta>\autorun.lua"
$env:TPT_OOB_TAG = "base"
.\build-tpt\powder.exe ddir <pasta>
$env:TPT_OOB_TAG = "calor"; $env:TPT_OOB_HEAT = "131.0"
.\build-tpt\powder.exe ddir <pasta>
```

Sai um `resultado_<tag>.txt` por execução. O cenário monta uma parede elétrica em
`x=10..20` na linha-célula 0, um fio isolado em `(15,1)` sob uma célula interior
do span, e dispara `set_emap(12,0)` com um SPRK no pixel (49,4) — o bloco "spark
updates from walls" mapeia `y%CELL==0` para `ny = y/CELL - 1 = 0`.

A geometria não é arbitrária: para chegar ao termo defeituoso, todos os termos
anteriores da cadeia precisam ser falsos. Daí o fio de baixo ter de estar numa
célula **interior** (senão `x==x1` ou `x==x2` curto-circuita) e não ter vizinhos
laterais embaixo (senão `is_wire(x±1, y+1)` curto-circuita).

## Como reverificar a correção

Trocar `y<0` por `y<1` e rodar as duas configurações de novo. Esperado:

| configuração | `emap[1][15]` antes | depois |
|---|---:|---:|
| base | 14 | 14 |
| calor 131,0 | 0 | **14** |

Ou seja: o comportamento normal não muda, e o calor ambiente deixa de influenciar.

## Como foi encontrado

Varredura das rotinas de flood fill atrás de erros de espelhamento entre os ramos
esquerdo/direito e cima/baixo, na mesma linha do PR #1104. A varredura achou dois
índices de bitmap trocados (`flood_water`, já no #1104, e `ApplyDecorationFill`) e
duas guardas espelhadas escritas como `y<0` (`FloodINST` e esta).

A de `ApplyDecorationFill` (`bitmap[(x1+1)+y*XRES]` onde caberia `x2+1`) foi
medida e **não tem efeito nenhum**: em cinco padrões, incluindo adversariais, as
duas versões dão contagem e cobertura idênticas. O termo do bitmap na varredura à
direita é inalcançável ali, porque o span sempre marca o bitmap por inteiro. Em
`flood_water` ele importa justamente porque um `continue` pode pular a marcação.
