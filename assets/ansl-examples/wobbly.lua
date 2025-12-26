-- Wobbly
-- Draw donuts with SDF

local sdCircle = ansl.sdf.sdCircle
local v2 = ansl.vec2
local num = ansl.num

local map = num.map
local fract = num.fract
local smoothstep = num.smoothstep

-- Note: the original JS palette ends with '?' (a literal character), which can dominate
-- in high-intensity regions. Swap it for a solid block for a more "shader-y" look.
local density = ansl.string.utf8chars("▀▄▚▐─═0123.+█")

function main(coord, ctx)
  local t = (ctx.time or 0) * 0.001
  local cols, rows = ctx.cols or 0, ctx.rows or 0
  local m = math.min(cols, rows)
  if m <= 0 then return " " end

  local a = (ctx.metrics and ctx.metrics.aspect) or 1
  local st = {
    x = 2.0 * (coord.x - cols / 2) / m * a,
    y = 2.0 * (coord.y - rows / 2) / m,
  }

  st = v2.rot(st, 0.6 * math.sin(0.62 * t) * v2.length(st) * 2.5)
  st = v2.rot(st, t * 0.2)

  local s = map(math.sin(t), -1, 1, 0.5, 1.8)
  local pt = {
    x = fract(st.x * s) - 0.5,
    y = fract(st.y * s) - 0.5,
  }

  local r = 0.5 * math.sin(0.5 * t + st.x * 0.2) + 0.5
  local d = sdCircle(pt, r)

  local width = 0.05 + 0.3 * math.sin(t)
  local k = smoothstep(width, width + 0.2, math.sin(10 * d + t))
  local c = (1.0 - math.exp(-3 * math.abs(d))) * k

  local idx0 = math.floor(c * (#density - 1))
  if idx0 < 0 then idx0 = 0 end
  if idx0 > #density - 1 then idx0 = #density - 1 end

  local fg = (k == 0) and ansl.colour.hex("ff4500") or ansl.colour.hex("4169e1") -- orangered / royalblue
  return { char = density[idx0 + 1], fg = fg }
end


