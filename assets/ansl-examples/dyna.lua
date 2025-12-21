-- Dyna
-- A remix of Paul Haeberli’s Dynadraw (follows the editor caret)

settings = { fps = 60 }

local smoothstep = ansl.num.smoothstep
local utf8chars = ansl.string.utf8chars

local MASS = 20   -- pencil mass
local DAMP = 0.95 -- pencil damping
local RADIUS = 15 -- pencil radius (in cells)

local density = utf8chars(" .:░▒▓█Ñ#+-")

local cols, rows = 0, 0
local data = {} -- per-cell intensity [0..1]

local dyna = {
  pos = { x = 0, y = 0 },
  vel = { x = 0, y = 0 },
  pre = { x = 0, y = 0 },
}

local function clampi(i, lo, hi)
  if i < lo then return lo end
  if i > hi then return hi end
  return i
end

local function len2(x, y)
  return (x * x + y * y) ^ 0.5
end

local function bresenham_line(ax, ay, bx, by)
  local x0 = math.floor(ax)
  local y0 = math.floor(ay)
  local x1 = math.floor(bx)
  local y1 = math.floor(by)
  local dx = math.abs(x1 - x0)
  local dy = -math.abs(y1 - y0)
  local sx = (x0 < x1) and 1 or -1
  local sy = (y0 < y1) and 1 or -1
  local err = dx + dy

  local out = {}
  while true do
    out[#out + 1] = { x = x0, y = y0 }
    if x0 == x1 and y0 == y1 then break end
    local e2 = 2 * err
    if e2 >= dy then err = err + dy; x0 = x0 + sx end
    if e2 <= dx then err = err + dx; y0 = y0 + sy end
  end
  return out
end

local function dyna_update(ctx, cursor)
  local caret = ctx and ctx.caret
  local tx = (caret and caret.x) or (cursor and cursor.x) or (cols / 2)
  local ty = (caret and caret.y) or (cursor and cursor.y) or (rows / 2)

  local fx = tx - dyna.pos.x
  local fy = ty - dyna.pos.y
  local ax = fx / MASS
  local ay = fy / MASS

  dyna.vel.x = (dyna.vel.x + ax) * DAMP
  dyna.vel.y = (dyna.vel.y + ay) * DAMP

  dyna.pre.x = dyna.pos.x
  dyna.pre.y = dyna.pos.y

  dyna.pos.x = dyna.pos.x + dyna.vel.x
  dyna.pos.y = dyna.pos.y + dyna.vel.y
end

function pre(ctx, cursor)
  local c = ctx.cols or 0
  local r = ctx.rows or 0
  if c <= 0 or r <= 0 then return end

  if c ~= cols or r ~= rows then
    cols, rows = c, r
    local n = cols * rows
    for i = 1, n do data[i] = 0 end
    for i = n + 1, #data do data[i] = nil end
  end

  local a = (ctx.metrics and ctx.metrics.aspect) or 1
  dyna_update(ctx, cursor)

  local pts = bresenham_line(dyna.pos.x, dyna.pos.y, dyna.pre.x, dyna.pre.y)
  for _, p in ipairs(pts) do
    local sx = math.max(0, p.x - RADIUS)
    local ex = math.min(cols, p.x + RADIUS)
    local sy = math.floor(math.max(0, p.y - RADIUS * a))
    local ey = math.floor(math.min(rows, p.y + RADIUS * a))

    for y = sy, ey - 1 do
      for x = sx, ex - 1 do
        local dx = (p.x - x)
        local dy = (p.y - y) / a
        local l = 1.0 - len2(dx, dy) / RADIUS
        if l > 0 then
          local idx1 = x + y * cols + 1
          local v = data[idx1] or 0
          if l > v then data[idx1] = l end
        end
      end
    end
  end
end

function main(coord, ctx)
  local i1 = (coord.index or 0) + 1
  local v = smoothstep(0, 0.9, data[i1] or 0)
  data[i1] = (data[i1] or 0) * 0.99
  local di = clampi(math.floor(v * (#density - 1)) + 1, 1, #density)
  return density[di]
end


