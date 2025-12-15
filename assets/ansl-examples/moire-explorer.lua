-- Moiré explorer (Lua port)
-- Click to cycle modes (0..2).

settings = { fps = 60 }

local map = ansl.num.map
local floor = math.floor
local sin = math.sin
local cos = math.cos
local atan2 = math.atan2

local density = ansl.string.utf8chars(" ..._-:=+abcXW@#ÑÑÑ")

local mode = 0
local prev_left = false
local prev_right = false

local function clampi(i, lo, hi)
  if i < lo then return lo end
  if i > hi then return hi end
  return i
end

local function dist(ax, ay, bx, by)
  local dx = ax - bx
  local dy = ay - by
  return (dx * dx + dy * dy) ^ 0.5
end

function pre(ctx, cursor)
  cursor = cursor or {}
  local left = (cursor.left == true)
  local right = (cursor.right == true)
  local pressed = (left and not prev_left) or (right and not prev_right)
  if pressed then
    mode = (mode + 1) % 3
  end
  prev_left = left
  prev_right = right
end

function main(coord, ctx, cursor)
  local t = (ctx.time or 0) * 0.0001
  local cols, rows = ctx.cols or 0, ctx.rows or 0
  local m = math.min(cols, rows)
  if m <= 0 then return " " end

  local stx = 2.0 * (coord.x - cols / 2) / m
  local sty = 2.0 * (coord.y - rows / 2) / m
  stx = stx * ((ctx.metrics and ctx.metrics.aspect) or 1)

  local centerAx = cos(t * 3) * 0.5
  local centerAy = sin(t * 7) * 0.5
  local centerBx = cos(t * 5) * 0.5
  local centerBy = sin(t * 4) * 0.5

  local A
  if mode % 2 == 0 then
    A = atan2(centerAy - sty, centerAx - stx)
  else
    A = dist(stx, sty, centerAx, centerAy)
  end

  local B
  if mode == 0 then
    B = atan2(centerBy - sty, centerBx - stx)
  else
    B = dist(stx, sty, centerBx, centerBy)
  end

  local aMod = map(cos(t * 2.12), -1, 1, 6, 60)
  local bMod = map(cos(t * 3.33), -1, 1, 6, 60)

  local a = cos(A * aMod)
  local b = cos(B * bMod)
  local i = ((a * b) + 1) / 2

  local idx0 = floor(i * #density)
  idx0 = clampi(idx0, 0, #density - 1)
  return density[idx0 + 1]
end


