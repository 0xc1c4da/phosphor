local dens = ansl.string.utf8chars("Ñ@#W$9876543210?!abc;:+=-,._ ")

function main(coord, ctx)
  local sign = (coord.y % 2) * 2 - 1
  local i = (ctx.cols + coord.y + coord.x * sign + ctx.frame) % #dens + 1
  return dens[i]
end
