local pat = ansl.string.utf8chars(" |▁|▂|▃|▄|▅|▆|▇|▆|▅|▄|▃|▂|▁")

function main(coord)
  return pat[coord.index % #pat]
end
