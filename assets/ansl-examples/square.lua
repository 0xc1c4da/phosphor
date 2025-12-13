local map = ansl.num.map

function main(coord, ctx)
  local t = ctx.time
  local m = math.min(ctx.cols, ctx.rows)
  local a = (ctx.metrics and ctx.metrics.aspect) or 1

  local x = 2 * (coord.x - ctx.cols / 2) / m
  local y = 2 * (coord.y - ctx.rows / 2) / m / a

  local ang = t * 0.0015
  local s, c = math.sin(-ang), math.cos(-ang)
  local px, py = x * c - y * s, x * s + y * c

  local size = map(math.sin(t * 0.0023), -1, 1, 0.1, 2)
  local dx = math.max(math.abs(px) - size, 0)
  local dy = math.max(math.abs(py) - size, 0)
  local d = math.sqrt(dx * dx + dy * dy)

  if d == 0 then return " " end
  return string.format("%.3f", d):sub(3, 3)
end
