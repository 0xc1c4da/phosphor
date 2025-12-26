-- Sand game
-- Click to drop sand

settings = {
  fps = 60,
}

local floor = math.floor

local function clampi(i, lo, hi)
  if i < lo then return lo end
  if i > hi then return hi end
  return i
end

local function dist2(ax, ay, bx, by)
  local dx = ax - bx
  local dy = ay - by
  return dx * dx + dy * dy
end

local function newParticle()
  local s = "sand"
  return s:sub(math.random(1, 4), math.random(1, 4))
end

local cols, rows = 0, 0
local cur = {}  -- current state chars (1-based)
local nxt = {}  -- next state

local function idx1(x, y)
  return x + y * cols + 1
end

local function groundChar(x)
  local s = "GROUND_"
  local i = (x % 7) + 1
  return s:sub(i, i)
end

local function wallChar(y)
  local s = "WALL"
  local i = (y % 4) + 1
  return s:sub(i, i)
end

local function get_char(x, y)
  if x < 0 or x >= cols then return " " end
  if y < 0 or y >= rows then return " " end
  if y >= rows - 1 then
    return groundChar(x)
  end
  if x <= 0 or x >= cols - 1 then
    return wallChar(y)
  end
  return cur[idx1(x, y)] or " "
end

local function alive(ch)
  return ch ~= " " and ch ~= nil
end

local function isWallChar(ch)
  return ch == "W" or ch == "A" or ch == "L"
end

function pre(ctx, cursor)
  local c = ctx.cols or 0
  local r = ctx.rows or 0
  if c <= 0 or r <= 0 then return end

  if c ~= cols or r ~= rows then
    cols, rows = c, r
    local n = cols * rows
    for i = 1, n do
      cur[i] = (math.random() > 0.5) and newParticle() or " "
      nxt[i] = " "
    end
    for i = n + 1, #cur do cur[i] = nil end
    for i = n + 1, #nxt do nxt[i] = nil end
  end

  cursor = cursor or {}
  local down = (cursor.left == true) or (cursor.right == true)

  -- JS uses cursor.pressed (held down). Emulate: while the button is down,
  -- continuously inject particles in a small radius.
  if down then
    local cx = floor(cursor.x or 0)
    local cy = floor(cursor.y or 0)
    local r2 = 3 * 3
    for y = cy - 3, cy + 3 do
      for x = cx - 3, cx + 3 do
        if x >= 0 and x < cols and y >= 0 and y < rows and dist2(x, y, cx, cy) < r2 then
          if y < rows - 1 and x > 0 and x < cols - 1 then
            cur[idx1(x, y)] = (math.random() > 0.5) and newParticle() or " "
          end
        end
      end
    end
  end

  local frame = ctx.frame or 0

  for y = 0, rows - 1 do
    for x = 0, cols - 1 do
      local out = " "

      if y >= rows - 1 then
        out = groundChar(x)
      elseif x <= 0 or x >= cols - 1 then
        out = wallChar(y)
      else
        local me = get_char(x, y)
        local below = get_char(x, y + 1)
        local above = get_char(x, y - 1)
        local left = get_char(x - 1, y)
        local right = get_char(x + 1, y)
        local topleft = get_char(x - 1, y - 1)
        local topright = get_char(x + 1, y - 1)
        local bottomleft = get_char(x - 1, y + 1)
        local bottomright = get_char(x + 1, y + 1)

        if alive(me) then
          if alive(below) and (((frame % 2 == 0) and alive(bottomright)) or ((frame % 2 == 1) and alive(bottomleft))) then
            out = me
          else
            out = " "
          end
        else
          if alive(above) then
            out = above
          elseif alive(left) and (frame % 2 == 0) and alive(topleft) then
            out = topleft
          elseif alive(right) and (frame % 2 == 1) and alive(topright) then
            out = topright
          else
            out = " "
          end

          if isWallChar(out) then
            out = " "
          end
        end
      end

      nxt[idx1(x, y)] = out
    end
  end

  cur, nxt = nxt, cur
end

function main(coord, ctx, cursor)
  local x = coord.x or 0
  local y = coord.y or 0

  if y >= (ctx.rows or rows) - 1 then
    return {
      char = groundChar(x),
      bg = ansl.colour.rgb(138, 162, 70),
      fg = ansl.colour.rgb(211, 231, 151),
    }
  end

  if x <= 0 or x >= (ctx.cols or cols) - 1 then
    return {
      char = wallChar(y),
      bg = ansl.colour.rgb(247, 187, 39),
      fg = ansl.colour.ansi16.white,
    }
  end

  -- While holding, render a "droplet" area like the JS example (visual feedback),
  -- but keep the simulation state update in pre().
  cursor = cursor or {}
  local down = (cursor.left == true) or (cursor.right == true)
  local ch = cur[(coord.index or 0) + 1] or " "
  if down and dist2(x, y, floor(cursor.x or 0), floor(cursor.y or 0)) < 3 * 3 then
    ch = (math.random() > 0.5) and newParticle() or " "
  end
  return {
    char = ch,
    bg = ansl.colour.ansi16.white,
    fg = ansl.colour.rgb(179, 158, 124),
  }
end


