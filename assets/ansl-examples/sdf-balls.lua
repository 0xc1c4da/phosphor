-- SDF Balls

local map = ansl.num.map
local sdCircle = ansl.sdf.sdCircle
local opSmoothUnion = ansl.sdf.opSmoothUnion

local density = ansl.string.utf8chars("#ABC|/:÷×+-=?*· ")

local function transform(p, trans, rot)
  local s, c = math.sin(-rot), math.cos(-rot)
  local dx, dy = p.x - trans.x, p.y - trans.y
  return { x = dx * c - dy * s, y = dx * s + dy * c }
end

local function clampi(i, lo, hi)
  if i < lo then return lo end
  if i > hi then return hi end
  return i
end

function main(coord, ctx)
  local t = (ctx.time or 0) * 0.001 + 10.0 -- ctx.time is ms
  local cols, rows = ctx.cols or 0, ctx.rows or 0
  local m = math.min(cols, rows)
  if m <= 0 then return " " end

  local a = (ctx.metrics and ctx.metrics.aspect) or 1
  local st = {
    x = 2.0 * (coord.x - cols / 2) / m * a,
    y = 2.0 * (coord.y - rows / 2) / m,
  }

  local s = map(math.sin(t * 0.5), -1, 1, 0.0, 0.9)
  local d = 1e100

  local num = 12
  for i = 1, num do
    local r = map(math.cos(t * 0.95 * i / (num + 1)), -1, 1, 0.1, 0.3)
    local ang = (i - 1) / num * math.pi + math.pi
    local x = map(math.cos(t * 0.23 * ang), -1, 1, -1.2, 1.2)
    local y = map(math.sin(t * 0.37 * ang), -1, 1, -1.2, 1.2)
    local f = transform(st, { x = x, y = y }, t)
    d = opSmoothUnion(d, sdCircle(f, r), s)
  end

  local c = 1.0 - math.exp(-3.0 * math.abs(d))
  local idx = clampi(math.floor(c * #density) + 1, 1, #density)
  return density[idx]
end


