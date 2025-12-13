local length = ansl.vec2.length

function main(coord, ctx)
  local a = (ctx.metrics and ctx.metrics.aspect) or 1
  local m = math.min(ctx.cols * a, ctx.rows)
  local st = {
    x = 2 * (coord.x - ctx.cols / 2) / m * a,
    y = 2 * (coord.y - ctx.rows / 2) / m,
  }
  return (length(st) < 0.7) and "X" or "."
end
