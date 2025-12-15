-- SDF Two Circles (Lua port)
-- Smooth union of two circles; the second follows the editor caret.

local sdCircle = ansl.sdf.sdCircle
local opSmoothUnion = ansl.sdf.opSmoothUnion
local v2 = ansl.vec2

local density = ansl.string.utf8chars("#WX?*:÷×+=-· ")

local function clampi(i, lo, hi)
  if i < lo then return lo end
  if i > hi then return hi end
  return i
end

function main(coord, ctx, cursor)
  local cols, rows = ctx.cols or 0, ctx.rows or 0
  local m = math.min(cols, rows)
  if m <= 0 then return " " end

  local a = (ctx.metrics and ctx.metrics.aspect) or 1
  local st = v2.vec2(
    2.0 * (coord.x - cols / 2) / m * a,
    2.0 * (coord.y - rows / 2) / m
  )

  -- Use the editor caret (text caret in cell space) as the second circle center.
  local caret = ctx and ctx.caret
  local cx = (caret and caret.x) or (cols / 2)
  local cy = (caret and caret.y) or (rows / 2)

  local pointer = v2.vec2(
    2.0 * (cx - cols / 2) / m * a,
    2.0 * (cy - rows / 2) / m
  )

  local d1 = sdCircle(st, 0.2)
  local d2 = sdCircle(v2.sub(st, pointer), 0.2)
  local d = opSmoothUnion(d1, d2, 0.7)

  local c = 1.0 - math.exp(-5.0 * math.abs(d))
  local i = clampi(math.floor(c * #density) + 1, 1, #density)
  return density[i]
end


