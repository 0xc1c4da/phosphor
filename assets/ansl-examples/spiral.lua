-- Spiral
-- Uses ansl.sort.brightness() to sort a density ramp based on the current Unscii font atlas.
--
-- Inspired by this shader by ahihi:
-- https://www.shadertoy.com/view/XdSGzR

settings = { fps = 60 }

local floor = math.floor
local sin = math.sin
local atan = math.atan
local pi = math.pi

local TAU = pi * 2

-- JS: sort('▅▃▁?ab012:. ', 'Simple Console', false)
-- Native: use current font atlas (Unscii) for brightness sort.
local density_sorted = ansl.sort.brightness("▅▃▁?ab012:. ", false)
local density = ansl.string.utf8chars(density_sorted)

local function atan2(y, x)
  -- LuaJIT supports math.atan(y, x), but keep a safe fallback.
  if x == 0 then
    if y > 0 then return pi / 2 end
    if y < 0 then return -pi / 2 end
    return 0
  end
  local a = atan(y / x)
  if x < 0 then
    if y >= 0 then return a + pi end
    return a - pi
  end
  return a
end

local function length2(x, y)
  return math.sqrt(x * x + y * y)
end

function main(coord, ctx)
  local t = (ctx.time or 0) * 0.0006
  local cols = ctx.cols or 0
  local rows = ctx.rows or 0
  local m = math.min(cols, rows)
  if m <= 0 then return " " end

  local a = (ctx.metrics and ctx.metrics.aspect) or 1

  local stx = 2.0 * (coord.x - cols / 2) / m * a
  local sty = 2.0 * (coord.y - rows / 2) / m

  local radius = length2(stx, sty)
  local rot = 0.03 * TAU * t
  local turn = atan2(sty, stx) / TAU + rot

  local n_sub = 1.5
  local turn_sub = (n_sub * turn) % n_sub

  local k = 0.1 * sin(3.0 * t)
  local s = k * sin(50.0 * ((radius ^ 0.1) - 0.4 * t))
  local turn_sine = turn_sub + s

  local n = #density
  if n <= 0 then return " " end

  local i_turn = floor((n * turn_sine) % n)

  local rr = radius * 0.5
  if rr < 1e-6 then rr = 1e-6 end
  local i_radius = floor(1.5 / (rr ^ 0.6) + 5.0 * t)

  local idx = (i_turn + i_radius) % n
  return density[idx + 1] or " "
end


