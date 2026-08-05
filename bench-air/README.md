# Issue #668 — "Biased Air simulation?"

Medição da assimetria relatada em
https://github.com/The-Powder-Toy/The-Powder-Toy/issues/668 (aberta em 2019,
9 comentários, sem PR vinculado, sem causa confirmada em 7 anos).

## Método

Grid de células é 153×96, então o espelho horizontal de `x` é `152 - x`. O
cenário parte de uma condição **simétrica por construção** e mede o quanto o
campo de pressão se afasta da simetria ao longo do tempo.

Duas métricas:

- **absoluta**: `Σ |pv[y][x] − pv[y][152−x]|` — magnitude do desvio;
- **com sinal**: `Σ (pv[y][x] − pv[y][152−x])` — direção do viés. Positivo
  significa mais pressão à esquerda.

**Cuidado com o domínio da soma com sinal.** Varrer `x` de 1 até 151 faz cada par
espelhado entrar duas vezes com sinais opostos, e a soma dá zero para *qualquer*
campo — mede nada. A primeira versão deste teste cometeu esse erro e produziu um
"zero" que parecia confirmar simetria. A soma corrigida varre apenas a metade
esquerda, de modo que cada par entre uma única vez.

## Resultados (200 frames)

| cenário | assim. com sinal | magnitude | relativo |
|---|---:|---:|---:|
| só ar, sem partícula alguma | 0,000085 | 18.936 | 4,5e-9 |
| com 17k partículas, criadas esq→dir | **+87,8** | 839 | +10,5% |
| com 17k partículas, criadas dir→esq | **−70,7** | 838 | −8,4% |

## Conclusão

1. **O solver de ar isolado é simétrico.** Sem partículas, o desvio fica na ordem
   de 1e-9 relativo — arredondamento de float. `Air::update_air` não é a causa.
2. **A assimetria aparece com partículas** e é grande, ~10% do campo.
3. **A causa é a ordem de ID de partícula.** Criar o mesmo bloco, com a mesma
   geometria, da direita para a esquerda **inverte o sinal** do viés. O que muda
   entre as duas execuções é apenas qual lado recebeu os IDs baixos.

O mecanismo é o acoplamento partícula→ar dentro de `UpdateParticles`: cada
partícula escreve em `vx`/`vy`/`pv` da própria célula, e as processadas depois
leem um campo já modificado pelas anteriores. Como saves são carregados em ordem
de varredura, os IDs baixos ficam em cima e à esquerda — que é exatamente o viés
"para cima e para a esquerda" relatado na issue.

Isso é a mesma propriedade de design que o mantenedor jacob1 descreveu ao fechar
a issue #1102: *"All particles are moved single threaded, in their particle id
order, which often causes effects like this."*

## Consequência prática

Não há patch pequeno aqui. Não é um erro de estêncil no `Air.cpp`; é
consequência do laço sequencial em ordem de ID. Corrigir de verdade exigiria
desacoplar a escrita das partículas no grid de ar do passe de leitura — por
exemplo acumulando as contribuições num buffer separado e aplicando ao final do
frame, o que muda comportamento e compatibilidade de saves.

## Como reproduzir

```powershell
Copy-Item bench-air\air_bias_autorun.lua "<pasta>\autorun.lua"
$env:TPT_AIR_FRAMES  = "200"
$env:TPT_AIR_NOPARTS = "1"   # 1 = só ar; 0 = com partículas
$env:TPT_AIR_REVERSE = "0"   # 1 = cria o bloco da direita para a esquerda
$env:TPT_AIR_TAG     = "meu_teste"
.\build-tpt\powder.exe ddir <pasta>
```

Sai um CSV com as duas métricas por frame. Não precisa de patch no C++: tudo é
lido por `sim.pressure` via Lua, então roda em build limpo do upstream.
