local map = ansl.num.map
local sdBox = ansl.sdf.sdBox
local opSmoothUnion = ansl.sdf.opSmoothUnion

local density = ansl.string.utf8chars("▚▀abc|/:÷×+-=?*·")

local function transform(p, tx, ty, rot)
  local s, c = math.sin(-rot), math.cos(-rot)
  local dx, dy = p.x - tx, p.y - ty
  return { x = dx * c - dy * s, y = dx * s + dy * c }
end

function main(coord, ctx)
  local t = ctx.time
  local m = math.max(ctx.cols, ctx.rows)
  local a = (ctx.metrics and ctx.metrics.aspect) or 1
  local st = { x = 2 * (coord.x - ctx.cols / 2) / m, y = 2 * (coord.y - ctx.rows / 2) / m / a }

  local d = 1e100
  local k = map(math.sin(t * 0.0005), -1, 1, 0, 0.4)
  local g = 1.2
  for by = -g, g, g * 0.33 do
    for bx = -g, g, g * 0.33 do
      local r = t * 0.0004 * (bx + g * 2) + (by + g * 2)
      local f = transform(st, bx, by, r)
      d = opSmoothUnion(d, sdBox(f, { x = g * 0.33, y = 0.01 }), k)
    end
  end

  local c = 1 - math.exp(-5 * math.abs(d))
  local i = math.floor(c * #density) + 1
  if i < 1 then i = 1 elseif i > #density then i = #density end
  return density[i]
end
