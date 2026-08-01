# Desenho: paralelização do laço de partículas

Documento de desenho para o mod. Todas as afirmações numéricas vêm das medições
em `README.md` desta pasta; onde algo é hipótese, está marcado como tal.

> **Status:** desenho histórico, invalidado no estágio 3. O deslocamento longo de
> OIL depende de trocas repetidas com partículas processadas depois dele e não é
> previsível no despacho anterior ao update. As medições de quebra de custo
> limitaram “só pós/sólidos” a 1,67x mesmo com threads infinitas. O restante fora
> de `MovementPhase` tem 56,01% do custo e teto infinito de 2,27x no update, mas
> dá só 1,85x na simulação com oito threads e não é uma fase segura: callbacks de
> elemento dentro dele escrevem `pmap`, criam, matam e movem partículas. Não
> implementar o estágio 4 descrito abaixo; ver `README.md` para a evidência e a
> decisão atuais.

## Objetivo e não-objetivos

**Objetivo:** com 143 mil partículas, sair de 24 FPS para 60 FPS, reduzindo
`UpdateParticles` de 25,8 ms para algo em torno de 8–10 ms.

**Não-objetivos:**

- equivalência bit a bit com o TPT serial. É impossível por construção: a ordem
  de atualização muda, e no TPT a ordem é fisicamente visível (issue #826
  documenta isso). Saves antigos vão evoluir diferente;
- upstream. Isto é um mod. Se um dia virar proposta upstream, o paralelismo terá
  de ser opcional e desligado por padrão;
- GPU. Descartado: exigiria reescrever a semântica ordem-dependente, o que
  converge para um projeto diferente.

**Compromisso inegociável:** determinismo *entre execuções*. A mesma cena, o
mesmo número de frames e o mesmo número de threads devem produzir exatamente o
mesmo estado. Sem isso não há como testar nada.

## O que as medições restringem

| fato medido | consequência de desenho |
|---|---|
| `UpdateParticles` = 92% do trabalho de simulação | só vale paralelizar este laço |
| pós/sólidos se deslocam ≤ 6 px por frame | o caso comum é local com folga |
| busca lateral de líquidos `rt = 30` (leitura e escrita) | halo mínimo de 32 px |
| 62 elementos leem raio 1, 36 leem raio 2, STKM 4 | varredura de elemento cabe no halo |
| `DTEC` limitado a 25 px | cabe no halo |
| OIL chegou a 280 px num frame, por trocas repetidas posteriores | **não** dá para prever o destino antes do update |
| `MAX_VELOCITY = 1e4` px/frame | o clamp de velocidade não dá limite útil |
| `LDTC`, `ETRD`, raios, `WIFI`, portais, `EMP` sem localidade | exclusão por tipo, passe serial |

A assimetria central: **leitura longa é enumerável por tipo; escrita longa não
é**, porque matéria comum pode ser reposicionada repetidamente por trocas com
partículas processadas depois dela. Daí duas regras distintas de despacho.

## Inventário de perigos (estado mutável compartilhado)

Isto é mais determinante que o alcance espacial, porque **não é espacial** — não
existe halo que resolva.

1. **RNG único.** `RNG rng` é membro de `Simulation`, com 27 chamadas só em
   `Simulation.cpp` mais o código de elementos. Chamado de várias threads é
   corrida de dados *e* destrói o determinismo, porque a ordem das chamadas passa
   a depender do escalonador.
2. **Lista livre única.** `pfree` é uma lista encadeada intrusiva através de
   `data[i].life`. `create_part` faz pop, `kill_part` faz push. Concorrência
   corrompe a lista. E qualquer matéria comum cria/destrói partículas, então isso
   não é excluível por tipo.
3. **Grid de ar com realimentação.** O laço escreve `vx`/`vy`/`pv` na célula da
   partícula. Célula é `CELL = 4` px, então o halo cobre, desde que a posse de
   célula seja respeitada.
4. **Contadores e mapas globais:** `elementCount[]`, `emap[]`, `wireless[]`,
   `portalp[]` — 37 escritas no laço.
5. **`parts.active`** e o índice do último ativo.

## Arquitetura proposta

### Decomposição

Faixas **verticais** (colunas), largura ≥ 2 × halo = **64 px**. Com `XRES = 612`,
isso dá no máximo 9 faixas — suficiente para 8 threads.

Por que 2 × halo: se a faixa `k` alcança até `halo` px para fora, o alcance entra
apenas na faixa vizinha. Processando faixas alternadas, duas faixas concorrentes
estão a ≥ 2 de distância, e seus halos não se sobrepõem. Com largura < 2 × halo,
os halos de `k-1` e `k+1` colidiriam dentro de `k`.

Por que verticais e não horizontais: a gravidade é vertical, então o movimento em
massa é vertical e **fica dentro** de uma faixa vertical. Cruzar fronteira passa a
ser o caso raro, não o comum. (Hipótese a validar com a sonda de alcance: medir
quantas partículas cruzariam fronteira em cada orientação.)

### Fases por frame

```
1. classificar   (paralelo, sem efeito colateral)
2. passe par     (paralelo: faixas 0,2,4,...)
3. passe ímpar   (paralelo: faixas 1,3,5,...)
4. passe serial  (thread única, ordem de índice fixa)
```

**Fase 1 — classificar.** Para cada partícula, decidir o destino:

- tipo em `{LDTC, ETRD, ARAY, CRAY, DRAY, WIFI, PRTI, PRTO, EMP, STKM, FIGH}`
  → passe serial (leitura ilimitada);
- deslocamento pretendido > `halo - raio_de_varredura_do_tipo`
  → passe serial (escrita longa);
- caso contrário → faixa que contém a partícula.

O deslocamento pretendido é estimado a partir da velocidade atual mais o
incremento máximo possível de um frame (gravidade + advecção, ambos limitados).
A estimativa precisa ser **conservadora**: errar para o lado de mandar ao passe
serial custa desempenho; errar para o outro lado corrompe estado.

**Fases 2 e 3 — passes paralelos.** Cada thread processa suas faixas na ordem de
índice de partícula, exatamente como o laço serial faz hoje.

**Fase 4 — passe serial.** Tudo que foi adiado, em ordem de índice.

### Como cada perigo é resolvido

| perigo | solução |
|---|---|
| RNG único | **Implementado no estágio 1, e melhor do que o previsto aqui:** semeadura por *partícula*, via `(currentTick, índice)` com splitmix64, em vez de por faixa. Muda a sequência vs. o TPT serial — aceito. |
| Lista livre | Particionar `pfree` em N segmentos fixos, um por thread. Alocação e liberação ficam no segmento dono. Preserva visibilidade imediata da partícula criada, ao contrário de fila diferida. |
| Grid de ar | Coberto pela posse de faixa, já que `CELL = 4` ≪ halo. |
| `elementCount[]` | Acumular por thread, somar no fim do frame. |
| `wireless`/`portalp` | Só tocados por elementos do passe serial. |
| `parts.active` | Recalculado após o passe serial. |

### Argumento de determinismo

O estado final depende de: ordem das fases (fixa), atribuição faixa→thread
(fixa), ordem de iteração dentro da faixa (índice, fixa), sequência de RNG por
faixa (função de frame e faixa, fixa) e segmento da lista livre (fixo). Nenhum
desses depende do escalonador. Logo o resultado é reproduzível para um dado
número de threads.

**Corrigido no estágio 1:** este documento afirmava originalmente que mudar o
número de threads mudaria o resultado, por mudar o particionamento. Com a
semeadura de RNG por partícula que acabou sendo implementada, o fluxo de
aleatórios é função apenas da identidade da partícula, e não do particionamento.
Some-se a isso que a ordem de iteração *dentro* de cada faixa continua sendo a de
índice, e o resultado deixa de depender do número de threads. O número de threads
**não** vira parâmetro do save.

Ressalva honesta: isso vale para o fluxo de aleatórios, que era o acoplamento mais
óbvio. Se restar alguma dependência de ordem *entre* faixas — matéria que cruza
fronteira, por exemplo — ela ainda pode reintroduzir sensibilidade ao
particionamento. Só o estágio 3 permitirá afirmar isso com medição.

## Estágios (cada um verificável isoladamente)

**Estágio 0 — arnês de determinismo.** Checksum do estado (posições, tipos,
temperaturas, velocidades) após N frames. Provar que o TPT atual é reprodutível
execução a execução. Sem isso não há como detectar regressão. Nenhuma mudança de
comportamento. *Pré-requisito absoluto.*

**Estágio 1 — RNG por região.** Trocar o RNG único por RNG indexado, ainda em
execução serial. Verificação: reprodutível, e a mudança visual vs. baseline deve
ser apenas a esperada por sequência diferente de aleatórios.

**Estágio 2 — lista livre particionada.** Ainda serial, com N segmentos.
Verificação: checksum idêntico ao do estágio 1.

**Estágio 3 — classificação e passe serial.** Implementar as 4 fases, mas com
**todas** as faixas rodando em série. Não há ganho de desempenho; o objetivo é
validar que a reestruturação por si só não muda o resultado além do previsto.

**Estágio 4 — paralelizar.** Ligar as threads nos passes par/ímpar. Só aqui
aparece ganho. Verificação: checksum estável entre execuções, e comparação de
desempenho com a baseline em `bench/`.

**Estágio 5 — ajuste.** Número de faixas, largura de halo, limiar de despacho.

## Verificação

Cada estágio precisa de:

- build limpo;
- checksum determinístico repetido (mesma cena, 3 execuções, mesmo hash);
- matriz de desempenho contra `bench/baseline_24k.csv` e `baseline_144k.csv`;
- sonda de alcance (`TPT_REACH_CSV`) confirmando que nenhuma partícula do passe
  paralelo excedeu o halo — isto é o teste que pega erro de classificação, e
  deve ser um assert em build de debug;
- inspeção visual de uma cena com líquido, pó e fogo.

## Riscos, e o que mataria o projeto

1. **Classificação conservadora demais.** Se boa parte da matéria cair no passe
   serial, o ganho evapora. Os dados dizem que o caso comum é ≤ 6 px, então a
   expectativa é boa — mas cenas com muita pressão (explosões) são exatamente as
   mais pesadas *e* as que mais arremessam matéria. **Este é o maior risco, e é
   medível cedo:** o estágio 3 já permite contar quantas partículas seriam
   adiadas, antes de escrever qualquer thread.
2. **Desequilíbrio de carga.** Matéria costuma acumular no fundo e num lado. Uma
   faixa pode ter 10× mais trabalho que outra. Mitigação: mais faixas que
   threads, com roubo de trabalho — mas roubo quebra o determinismo por
   atribuição fixa, então teria de ser atribuição fixa calculada a partir de uma
   contagem determinística.
3. **Perigo não inventariado.** 195 elementos, e eu li poucos. Algum pode tocar
   estado global que eu não listei. O assert de halo no estágio 4 pega parte
   disso, mas não tudo.
4. **O ganho real ficar bem abaixo de 5,2×.** O teto teórico assume paralelismo
   perfeito. Com passe serial, halo e desequilíbrio, 2,5–3,5× é a expectativa
   realista — o que ainda leva 25,8 ms para 8–10 ms e cruza o orçamento de 60 FPS.
   Abaixo de 2× o custo de complexidade provavelmente não se paga.

## Decisão pendente

O estágio 0 é útil independentemente do resto: um checksum determinístico é a
base de qualquer trabalho futuro em física neste fork. Recomendo começar por ele
mesmo que a decisão sobre paralelizar mude depois.
