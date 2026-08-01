# Rascunho de issue para o upstream — NÃO PUBLICADO

Estado: **rascunho**. Ver "Pendências antes de publicar" no fim. Não abrir a issue
antes de resolver os dois itens listados lá.

---

## Título proposto

Light liquids can be carried hundreds of pixels in a single frame by repeated swaps

## Corpo proposto

While profiling the simulation loop I measured single-frame displacements that
seem larger than expected, and I would like to know whether this is intended.

### What I observed

In a scene with a band of OIL resting above WATR, with a heat source below, some
OIL particles end up hundreds of pixels away from where they started **within one
frame**. The largest net displacement I recorded was 259 px on a 612 px wide grid.

It is not one long jump. It is many short ones. Instrumenting the swap block at
the end of `Simulation::try_move`, where the displaced particle is repositioned
onto the mover's previous coordinates, showed:

- every particle that moved further than 32 px in a frame had been swapped; none
  had reached that distance any other way;
- a single OIL particle was swapped **51 times in one frame**;
- the sum of individual swap distances was about **3.2x** the net displacement,
  which is what you would expect from repeated shoves that partially cancel.

So a light liquid sitting in a denser one gets pushed back and forth by every
heavier neighbour that moves into it during the pass, and the accumulated travel
is large even though each individual shove is small.

### Why this might matter

- `WATR` in the same scene never exceeded ~30 px, matching the sideways search
  limit, so the effect is specific to the lighter liquid being displaced rather
  than to liquid movement in general.
- The distance a particle travels ends up depending on how many heavier particles
  happen to be processed after it in the same frame, which makes it sensitive to
  iteration order.

### Is this intended?

Density-based displacement is obviously deliberate, and I am not assuming this is
a bug. My question is whether the *unbounded accumulation* within a single frame
is intended, or whether a light particle is expected to be shoved at most once or
a small number of times per frame.

### Context

Found while measuring the simulation loop for a personal parallelisation
experiment in a fork. The analysis and instrumentation were done with the help of
an LLM (Claude), reviewed and run by me.

---

## Pendências antes de publicar

1. **Reproduzir em build limpo.** Todos os números vieram do fork instrumentado.
   Antes de publicar é preciso confirmar o efeito num TPT sem modificação — por
   exemplo pausando e comparando a posição de uma partícula de OIL marcada entre
   dois frames, ou com um script Lua que registre `sim.partProperty(id, "x")` a
   cada tick. Sem isso o relato não é verificável por terceiros.

2. **Explicar a divergência de contagem.** O total de mispredicts passou de 2.108
   para 4.467 entre execuções com a mesma semente e o mesmo preditor, e a sonda de
   troca só incrementa contadores. Não citar número absoluto nenhum na issue até
   isso estar entendido. As conclusões acima usam só razões e máximos internos a
   uma mesma execução, que não dependem disso — mas a causa da divergência precisa
   ser conhecida antes de afirmar qualquer coisa em público.

Enquanto esses dois itens não estiverem fechados, este arquivo fica como rascunho.
