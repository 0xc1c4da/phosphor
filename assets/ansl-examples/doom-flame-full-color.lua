-- Doom Flame (full color) - xterm-256 edition
-- Port of ansl/src/programs/demos/doom_flame_full_color.js to idiomatic Lua.
--
-- Color model:
--   - fg/bg are xterm-256 indices (0..255), no alpha
--   - nil means "unset"
--
-- Host API:
--   - function render(ctx, layer)
--   - layer:set(x, y, glyph, fg?, bg?)
--   - layer:clear(glyph?)

local num = ansl.num

local min, max, floor = math.min, math.max, math.floor

-- Small helpers (avoid pulling in globals repeatedly)
local function clampi(v, lo, hi)
  v = floor(v)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

-- Random int between a and b inclusive
local function rndi(a, b)
  if b == nil then b = 0 end
  if a > b then a, b = b, a end
  return floor(a + math.random() * (b - a + 1))
end

-- Value noise (Scratchapixel / classic lattice noise)
local function valueNoise()
  local tableSize = 256

  local r = {}
  local perm = {}
  for k = 0, tableSize - 1 do
    r[k + 1] = math.random()
    perm[k + 1] = k
  end

  -- Fisher-Yates shuffle
  for k = tableSize - 1, 1, -1 do
    local i = floor(math.random() * (k + 1))
    perm[k + 1], perm[i + 1] = perm[i + 1], perm[k + 1]
  end

  -- Duplicate for 512 lookup (avoid mod in inner hash)
  for k = 1, tableSize do
    perm[tableSize + k] = perm[k]
  end

  local smoothstep = num.smoothstep
  local mix = num.mix

  local function imod(x)
    -- stable 0..255 even for negative (not expected but harmless)
    local m = x % tableSize
    if m < 0 then m = m + tableSize end
    return m
  end

  return function(px, py)
    local xi = floor(px)
    local yi = floor(py)
    local tx = px - xi
    local ty = py - yi

    local rx0 = imod(xi)
    local rx1 = imod(xi + 1)
    local ry0 = imod(yi)
    local ry1 = imod(yi + 1)

    -- Hash corners through permutation table (0-based values stored in perm[])
    local c00 = r[perm[perm[rx0 + 1] + ry0 + 1] + 1]
    local c10 = r[perm[perm[rx1 + 1] + ry0 + 1] + 1]
    local c01 = r[perm[perm[rx0 + 1] + ry1 + 1] + 1]
    local c11 = r[perm[perm[rx1 + 1] + ry1 + 1] + 1]

    local sx = smoothstep(0, 1, tx)
    local sy = smoothstep(0, 1, ty)

    local nx0 = mix(c00, c10, sx)
    local nx1 = mix(c01, c11, sx)
    return mix(nx0, nx1, sy)
  end
end

-- xterm palette indices for the flame
local color = ansl.color
local pal = {
  color.ansi16.black,              -- 0  (top)
  color.rgb(128, 0, 128),          -- 1  purple
  color.rgb(139, 0, 0),            -- 2  dark red
  color.rgb(255, 0, 0),            -- 3  red
  color.rgb(255, 69, 0),           -- 4  orange red
  color.rgb(255, 215, 0),          -- 5  gold
  color.rgb(255, 250, 205),        -- 6  lemon chiffon
  color.ansi16.bright_white,       -- 7  (bottom)
}

-- Flame intensity ramp (0..7). Source string is 0-based in JS; store as 1-based here.
local flame = {}
do
  local s = "011222233334444444455566667"
  for i = 1, #s do
    flame[i] = tonumber(s:sub(i, i)) or 0
  end
end
local flameLen = #flame

-- Persistent state (resizes on demand)
local cols, rows = 0, 0
local data = {}
local noise = valueNoise()

function render(ctx, layer)
  local c = tonumber(ctx.cols) or 0
  local r = tonumber(ctx.rows) or 0
  if c <= 0 or r <= 0 then return end

  -- Reset buffers on resize (keep table object, overwrite contents)
  if c ~= cols or r ~= rows then
    cols, rows = c, r
    local n = cols * rows
    for i = 1, n do data[i] = 0 end
  end

  local cursor = ctx.cursor or {}
  local pressed = not not cursor.pressed

  -- Fill floor with noise (or scribble if pressed)
  if not pressed then
    local t = (tonumber(ctx.time) or 0) * 0.0015
    local last = cols * (rows - 1) -- 0-based offset of last row
    for x = 0, cols - 1 do
      local v = floor(num.map(noise(x * 0.05, t), 0, 1, 5, 50))
      local idx = last + x + 1
      data[idx] = min(v, (data[idx] or 0) + 2)
    end
  else
    local cx = floor(tonumber(cursor.x) or 0)
    local cy = floor(tonumber(cursor.y) or 0)
    if cx >= 0 and cx < cols and cy >= 0 and cy < rows then
      data[cx + cy * cols + 1] = rndi(5, 50)
    end
  end

  -- Propagate upward with randomness
  local n = cols * rows
  for i = 0, n - 1 do
    local row = floor(i / cols)
    local col = i - row * cols
    local dest_col = clampi(col + rndi(-1, 1), 0, cols - 1)
    local dest = row * cols + dest_col
    local src_row = min(rows - 1, row + 1)
    local src = src_row * cols + col
    data[dest + 1] = max(0, (data[src + 1] or 0) - rndi(0, 2))
  end

  -- Draw
  -- We set every cell each frame (including bg) so the output is deterministic even
  -- if the host isn't clearing the layer each tick.
  local bg0 = pal[1] -- black
  for y = 0, rows - 1 do
    for x = 0, cols - 1 do
      local idx = x + y * cols + 1
      local u = data[idx] or 0
      local v = flame[clampi(u + 1, 1, flameLen)] or 0

      if v == 0 then
        layer:set(x, y, " ", nil, bg0)
      else
        local bg = pal[v + 1] -- v in 1..7 => pal[2..8]
        local fg = pal[min(#pal, v + 2)] -- brighter on top, clamp to white
        layer:set(x, y, tostring(u % 10), fg, bg)
      end
    end
  end
end


