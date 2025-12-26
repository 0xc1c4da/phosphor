-- Numbers
-- Fun with integers

settings = { bg = ansl.colour.ansi16.black }

local map = ansl.num.map
local floor = math.floor
local sin = math.sin

local function clampi(i, lo, hi)
  if i < lo then return lo end
  if i > hi then return hi end
  return i
end

-- CGA palette (canonical). Converted to the active canvas palette via nearest match.
local CGA = {
  ansl.colour.hex("000000"), -- black
  ansl.colour.hex("0000aa"), -- blue
  ansl.colour.hex("00aa00"), -- green
  ansl.colour.hex("00aaaa"), -- cyan
  ansl.colour.hex("aa0000"), -- red
  ansl.colour.hex("aa00aa"), -- magenta
  ansl.colour.hex("aa5500"), -- brown
  ansl.colour.hex("aaaaaa"), -- light gray
  ansl.colour.hex("555555"), -- dark gray
  ansl.colour.hex("5555ff"), -- bright blue
  ansl.colour.hex("55ff55"), -- bright green
  ansl.colour.hex("55ffff"), -- bright cyan
  ansl.colour.hex("ff5555"), -- bright red
  ansl.colour.hex("ff55ff"), -- bright magenta
  ansl.colour.hex("ffff55"), -- yellow
  ansl.colour.hex("ffffff"), -- white
}

-- Match JS palette pruning (0-based splices):
-- CGA.splice(10,1); CGA.splice(4,1); CGA.splice(2,1); CGA.splice(0,1)
local function remove0(t, idx0)
  table.remove(t, idx0 + 1)
end
remove0(CGA, 10)
remove0(CGA, 4)
remove0(CGA, 2)
remove0(CGA, 0)

local ints = {
  488162862,
  147460255,
  487657759,
  1042482734,
  71662658,
  1057949230,
  487540270,
  1041305872,
  488064558,
  488080430,
}

local numX = 5     -- number width
local numY = 6     -- number height
local spacingX = 2 -- spacing, after scale
local spacingY = 1

local function bit(n, k)
  -- LuaJIT: use arithmetic shift via division (n is positive here).
  return math.floor(n / (2 ^ k)) % 2
end

function main(coord, ctx)
  local f = ctx.frame or 0
  local rows = ctx.rows or 0

  local scale = map(sin(f * 0.01), -1, 1, 0.99, rows / numY)
  if scale <= 0 then scale = 1 end

  local x = coord.x / scale
  local y = coord.y / scale

  local sx = numX + spacingX / scale
  local sy = numY + spacingY / scale
  local cx = floor(x / sx)
  local cy = floor(y / sy)

  local offs = math.floor(map(sin(f * 0.012 + (cy * 0.5)), -1, 1, 0, 100) + 0.5)
  local num = (cx + cy + offs) % 10

  local nx = floor(x % sx)
  local ny = floor(y % sy)

  local b = 0
  if nx < numX and ny < numY then
    local n = ints[num + 1]
    local k = (numX - nx - 1) + (numY - ny - 1) * numX
    b = bit(n, k)
  end

  local cidx = (num % #CGA) + 1
  local fg = (b == 1) and CGA[cidx] or CGA[6] -- match JS: CGA[5] after splices (0-based) => 6 (1-based)
  return { char = (b == 1) and "▇" or ".", fg = fg }
end


