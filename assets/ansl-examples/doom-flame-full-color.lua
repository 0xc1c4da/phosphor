-- Doom Flame (full colour)

settings = { bg = ansl.colour.ansi16.black }

local num = ansl.num
local colour = ansl.colour

local floor = math.floor
local min = math.min
local max = math.max

-- Random int between a and b, inclusive.
local function rndi(a, b)
  if b == nil then b = 0 end
  if a > b then a, b = b, a end
  return math.random(a, b)
end

local function clampi(i, lo, hi)
  if i < lo then return lo end
  if i > hi then return hi end
  return i
end

-- Palette: JS named colours → canonical hex → nearest index in the active canvas palette.
local pal = {
  colour.hex("000000"), -- black
  colour.hex("800080"), -- purple
  colour.hex("8b0000"), -- darkred
  colour.hex("ff0000"), -- red
  colour.hex("ff4500"), -- orangered
  colour.hex("ffd700"), -- gold
  colour.hex("fffacd"), -- lemonchiffon
  colour.hex("ffffff"), -- white
}

-- Intensity ramp (JS: '011222233334444444455566667')
local ramp_s = "011222233334444444455566667"
local ramp_len = #ramp_s
local function ramp(u)
  u = clampi(u, 0, ramp_len - 1)
  return ramp_s:byte(u + 1) - 48 -- '0'..'7' → 0..7
end

-- Value noise (Scratchapixel). Same structure as JS, but with 1-based tables.
local function value_noise()
  local tableSize = 256
  local r = {}
  local p = {}

  for k = 0, tableSize - 1 do
    r[k + 1] = math.random()
    p[k + 1] = k
  end

  -- JS shuffle: swap each entry with a random one in the full table, and mirror into +256.
  for k = 0, tableSize - 1 do
    local i = floor(math.random() * tableSize) -- 0..255
    local a, b = k + 1, i + 1
    p[a], p[b] = p[b], p[a]
    p[a + tableSize] = p[a]
  end

  local smoothstep = num.smoothstep
  local mix = num.mix

  return function(px, py)
    local xi = floor(px)
    local yi = floor(py)
    local tx = px - xi
    local ty = py - yi

    local rx0 = xi % tableSize
    local rx1 = (rx0 + 1) % tableSize
    local ry0 = yi % tableSize
    local ry1 = (ry0 + 1) % tableSize

    local c00 = r[p[p[rx0 + 1] + ry0 + 1] + 1]
    local c10 = r[p[p[rx1 + 1] + ry0 + 1] + 1]
    local c01 = r[p[p[rx0 + 1] + ry1 + 1] + 1]
    local c11 = r[p[p[rx1 + 1] + ry1 + 1] + 1]

    local sx = smoothstep(0, 1, tx)
    local sy = smoothstep(0, 1, ty)
    return mix(mix(c00, c10, sx), mix(c01, c11, sx), sy)
  end
end

local noise = value_noise()

-- Persistent heat buffer (1D, 0-based addressed via idx+1)
local cols, rows = 0, 0
local data = {}

function pre(ctx, cursor, buffer)
  local c = ctx.cols or 0
  local r = ctx.rows or 0
  if c <= 0 or r <= 0 then return end

  if c ~= cols or r ~= rows then
    cols, rows = c, r
    local n = cols * rows
    for i = 1, n do data[i] = 0 end
    for i = n + 1, #data do data[i] = nil end
  end

  cursor = cursor or {}
  local down = (cursor.left == true) or (cursor.right == true)

  -- Fill the floor with noise (or inject heat on press).
  if not down then
    local t = (ctx.time or 0) * 0.0015
    local last = cols * (rows - 1) -- 0-based start index of last row
    for x = 0, cols - 1 do
      local u = floor(num.map(noise(x * 0.05, t), 0, 1, 5, 50))
      local idx1 = last + x + 1
      local prev = data[idx1] or 0
      data[idx1] = min(u, prev + 2)
    end
  else
    local cx = floor(cursor.x or 0)
    local cy = floor(cursor.y or 0)
    if cx >= 0 and cx < cols and cy >= 0 and cy < rows then
      data[cx + cy * cols + 1] = rndi(5, 50)
    end
  end

  -- Propagate towards the ceiling with some randomness (same as JS loop).
  local n = cols * rows
  for i = 0, n - 1 do
    local row = floor(i / cols)
    local col = i % cols
    local dest = row * cols + clampi(col + rndi(-1, 1), 0, cols - 1)
    local src = min(rows - 1, row + 1) * cols + col
    data[dest + 1] = max(0, (data[src + 1] or 0) - rndi(0, 2))
  end
end

function main(coord, ctx, cursor, buffer)
  local u = data[(coord.index or 0) + 1] or 0
  local v = ramp(clampi(u, 0, ramp_len - 1)) -- 0..7

  if v == 0 then
    return { char = " ", bg = pal[1] }
  end

  local bg = pal[v + 1]
  local fg = pal[clampi(v + 2, 1, 8)]
  return { char = u % 10, fg = fg, bg = bg }
end


