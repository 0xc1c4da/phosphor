-- Chroma Spiral
-- Inspired by: https://www.shadertoy.com/view/tdsyRf

local density = "#Wabc:+-. "

-- Colors are indices in the active canvas palette (fg/bg).
local colors = {
  ansl.color.hex("#ff1493"), -- deeppink
  ansl.color.ansi16.black,
  ansl.color.ansi16.red,
  ansl.color.ansi16.blue,
  ansl.color.hex("#ffa500"), -- orange
  ansl.color.ansi16.yellow,
}

local sin, cos, abs, floor, min = math.sin, math.cos, math.abs, math.floor, math.min

local function clamp(v, a, b)
  if v < a then return a end
  if v > b then return b end
  return v
end

local function map(v, inA, inB, outA, outB)
  return outA + (outB - outA) * ((v - inA) / (inB - inA))
end

local function len2(x, y)
  return (x * x + y * y) ^ 0.5
end

local function rot(x, y, ang)
  local s, c = sin(ang), cos(ang)
  return x * c - y * s, x * s + y * c
end

function main(coord, ctx, cursor, buffer)
  local t = (ctx.time or 0) * 0.0002 -- ctx.time is ms
  local cols, rows = ctx.cols or 0, ctx.rows or 0
  local m = min(cols, rows)
  if m <= 0 then return " " end

  local a = (ctx.metrics and ctx.metrics.aspect) or 1

  local stx = 2.0 * (coord.x - cols / 2) / m * a
  local sty = 2.0 * (coord.y - rows / 2) / m

  for i = 0, 2 do
    local o = i * 3
    stx = stx + sin(t * 3 + o)
    sty = sty + cos(t * 2 + o)
    local ang = -t + len2(stx - 0.5, sty - 0.5)
    stx, sty = rot(stx, sty, ang)
  end

  stx, sty = stx * 0.6, sty * 0.6

  local s = cos(t) * 2.0
  local c = sin(stx * 3.0 + s) + sin(sty * 21.0)
  c = map(sin(c * 0.5), -1, 1, 0, 1)
  c = clamp(c, 0, 1)

  local di = clamp(floor(c * (#density - 1)) + 1, 1, #density)
  local ci = clamp(floor(c * (#colors - 1)) + 1, 1, #colors)

  return { char = density:sub(di, di), fg = colors[ci] }
end


