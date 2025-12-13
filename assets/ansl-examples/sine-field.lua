local pat = ansl.string.utf8chars("ABCxyz01═|+:. ")

function main(coord, ctx)
  local t = ctx.time * 0.0001
  local o = math.sin(coord.y * math.sin(t) * 0.2 + coord.x * 0.04 + t) * 20
  local i = math.floor(math.abs(coord.x + coord.y + o) + 0.5) % #pat + 1
  return pat[i]
end
