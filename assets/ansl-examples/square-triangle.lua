-- oeö
-- Inspired by Ernst Jandl, 1964

local map = ansl.num.map
local smoothstep = ansl.num.smoothstep

local floor = math.floor
local cos = math.cos
local atan2 = math.atan2
local max = math.max
local PI = math.pi

local chars = { " ", "e", "o", "ö" }
local colours = {
  ansl.colour.ansi16.white,
  ansl.colour.hex("527EA8"),
  ansl.colour.hex("BB2A1C"),
  ansl.colour.hex("DFA636"),
}

local function len2(x, y)
  return (x * x + y * y) ^ 0.5
end

local function polygon(px, py, edges, time)
  time = time or 0
  local N = edges
  -- Note: JS uses atan2(p.x, p.y) (swapped) — keep for matching look.
  local a = (atan2(px, py) + 2 + time * PI) / (2 * PI)
  local b = (floor(a * N) + 0.5) / N
  local c = len2(px, py) * cos((a - b) * 2 * PI)
  return smoothstep(0.3, 0.31, c)
end

function main(coord, ctx)
  local m = max(ctx.cols or 0, ctx.rows or 0)
  if m <= 0 then return " " end
  local a = (ctx.metrics and ctx.metrics.aspect) or 1

  local stx = 2.0 * (coord.x - (ctx.cols or 0) / 2) / m
  local sty = 2.0 * (coord.y - (ctx.rows or 0) / 2) / m / a

  local t = ctx.time or 0

  local centerTx = stx + 0
  local centerTy = sty + cos(t * 0.0021) * 0.5
  local colourT = polygon(centerTx, centerTy, 3, t * 0.0002)
  local triangle = (colourT <= 0.1) and 1 or 0

  local centerQx = stx + cos(t * 0.0023) * 0.5
  local centerQy = sty + 0
  local colourQ = polygon(centerQx, centerQy, 4, -t * 0.0004)
  local quadrato = (colourQ <= 0.1) and 2 or 0

  local i0 = triangle + quadrato -- 0..3
  return { char = chars[i0 + 1], fg = colours[i0 + 1] }
end


