-- Plasma
-- Oldschool plasma demo

settings = { fps = 60 }

local map = ansl.num.map
local floor = math.floor
local sin = math.sin
local cos = math.cos

local density = ansl.string.utf8chars("$?01▄abc+-><:. ")

local PI = math.pi
local PI23 = PI * 2 / 3
local PI43 = PI * 4 / 3

local function clampi(i, lo, hi)
  if i < lo then return lo end
  if i > hi then return hi end
  return i
end

local function len2(x, y)
  return (x * x + y * y) ^ 0.5
end

function main(coord, ctx)
  local t1 = (ctx.time or 0) * 0.0009
  local t2 = (ctx.time or 0) * 0.0003
  local cols, rows = ctx.cols or 0, ctx.rows or 0
  local m = math.min(cols, rows)
  if m <= 0 then return " " end

  local a = (ctx.metrics and ctx.metrics.aspect) or 1
  local stx = 2.0 * (coord.x - cols / 2) / m * a
  local sty = 2.0 * (coord.y - rows / 2) / m

  local cx = sin(-t1)
  local cy = cos(-t1)

  local v1 = sin((coord.x * sin(t1) + coord.y * cos(t1)) * 0.08)
  local v2 = cos(len2(stx - cx, sty - cy) * 4.0)
  local v3 = v1 + v2

  local idx0 = floor(map(v3, -2, 2, 0, 1) * #density)
  idx0 = clampi(idx0, 0, #density - 1)

  -- Colours are quantized for performance (as in JS).
  local quant = 2
  local mult = 255 / (quant - 1)
  local r = floor(map(sin(v3 * PI + t1), -1, 1, 0, quant)) * mult
  local g = floor(map(sin(v3 * PI23 + t2), -1, 1, 0, quant)) * mult
  local b = floor(map(sin(v3 * PI43 - t1), -1, 1, 0, quant)) * mult

  local bg = ansl.colour.rgb(r, g, b)
  return { char = density[idx0 + 1], fg = ansl.colour.ansi16.white, bg = bg }
end


