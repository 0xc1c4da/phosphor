-- SDF Circle (Lua port)
-- Draw a smooth circle using exp()

settings = { fps = 60 }

local sdCircle = ansl.sdf.sdCircle
local utf8chars = ansl.string.utf8chars

local density = utf8chars("/\\MXYZabc!?=-. ")

local function clampi(i, lo, hi)
  if i < lo then return lo end
  if i > hi then return hi end
  return i
end

function main(coord, ctx)
  local t = (ctx.time or 0) * 0.002 -- ctx.time is ms
  local cols, rows = ctx.cols or 0, ctx.rows or 0
  local m = math.min(cols, rows)
  if m <= 0 then return " " end

  local a = (ctx.metrics and ctx.metrics.aspect) or 1
  local st = {
    x = 2.0 * (coord.x - cols / 2) / m * a,
    y = 2.0 * (coord.y - rows / 2) / m,
  }

  local radius = math.cos(t) * 0.4 + 0.5
  local d = sdCircle(st, radius)
  local c = 1.0 - math.exp(-5.0 * math.abs(d))

  local i = clampi(math.floor(c * #density) + 1, 1, #density)
  local ch = (coord.x % 2 == 1) and "│" or density[i]
  return { char = ch, fg = ansl.color.ansi16.white, bg = ansl.color.ansi16.black }
end


