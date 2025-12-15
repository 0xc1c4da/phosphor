-- Doom Flame (full color) — demoscene-clean Lua port of `doom-flame-full-color.js`.
-- API: `render(ctx, layer)`; `layer:set(x, y, char, fg?, bg?)` with xterm-256 indices, `nil` = unset.

settings = { bg = ansl.color.ansi16.black }

local num = ansl.num
local color = ansl.color

local floor = math.floor
local min = math.min
local rndi = math.random -- inclusive range

-- Palette: JS named colors → canonical hex → nearest xterm-256 index.
local pal = {
  color.hex("000000"), -- black
  color.hex("800080"), -- purple
  color.hex("8b0000"), -- darkred
  color.hex("ff0000"), -- red
  color.hex("ff4500"), -- orangered
  color.hex("ffd700"), -- gold
  color.hex("fffacd"), -- lemonchiffon
  color.hex("ffffff"), -- white
}

-- Intensity ramp (JS: '011222233334444444455566667')
local ramp_s = "011222233334444444455566667"
local ramp_len = #ramp_s
local function ramp(u)
  if u < 0 then u = 0
  elseif u > ramp_len - 1 then u = ramp_len - 1
  end
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

-- Persistent buffer (1D, 1-based), re-used across frames.
local cols, rows = 0, 0
local data = {}

function render(ctx, layer)
  local c = tonumber(ctx.cols) or 0
  local r = tonumber(ctx.rows) or 0
  if c <= 0 or r <= 0 then return end

  if c ~= cols or r ~= rows then
    cols, rows = c, r
    local n = cols * rows
    for i = 1, n do data[i] = 0 end
    for i = n + 1, #data do data[i] = nil end
  end

  local cursor = ctx.cursor or {}

  -- Fill the floor with noise (or scribble on press).
  if not cursor.pressed then
    local t = (tonumber(ctx.time) or 0) * 0.0015
    local last = cols * (rows - 1) -- 0-based start index of last row
    for x = 0, cols - 1 do
      local u = floor(num.map(noise(x * 0.05, t), 0, 1, 5, 50))
      local idx1 = last + x + 1
      local prev = data[idx1] or 0
      data[idx1] = (u < prev + 2) and u or (prev + 2)
    end
  else
    local cx = floor(tonumber(cursor.x) or 0)
    local cy = floor(tonumber(cursor.y) or 0)
    if cx >= 0 and cx < cols and cy >= 0 and cy < rows then
      data[cx + cy * cols + 1] = rndi(5, 50)
    end
  end

  -- Propagate towards the ceiling (keep JS iteration order: top → bottom).
  for y = 0, rows - 1 do
    local src_y = (y + 1 < rows) and (y + 1) or (rows - 1)
    local row_off = y * cols
    local src_off = src_y * cols

    for x = 0, cols - 1 do
      local dest_x = x + rndi(-1, 1)
      if dest_x < 0 then dest_x = 0
      elseif dest_x >= cols then dest_x = cols - 1
      end

      local v = (data[src_off + x + 1] or 0) - rndi(0, 2)
      if v < 0 then v = 0 end
      data[row_off + dest_x + 1] = v
    end
  end

  -- Draw.
  local bg0 = pal[1]
  for y = 0, rows - 1 do
    local off = y * cols
    for x = 0, cols - 1 do
      local u = data[off + x + 1] or 0
      local v = ramp(u) -- 0..7

      if v == 0 then
        layer:set(x, y, " ", nil, bg0)
      else
        local bg = pal[v + 1]
        local fg = pal[(v + 2 <= 8) and (v + 2) or 8]
        -- Pass ASCII codepoint directly to avoid any UTF-8 decoding edge cases in LuaJIT.
        layer:set(x, y, 48 + (u % 10), fg, bg)
      end
    end
  end
end


