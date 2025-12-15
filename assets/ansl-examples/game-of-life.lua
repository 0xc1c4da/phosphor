-- Golgol (Lua port)
-- Double-resolution Game of Life using block chars:
-- '█' both cells on, ' ' both off, '▀' upper on, '▄' lower on

local cols, rows = 0, 0
local a = {} -- buffer A (0/1), height = rows*2
local b = {} -- buffer B

local function r01()
  return (math.random() > 0.5) and 1 or 0
end

local function set_cell(buf, x, y, w, h, v)
  if x < 0 or x >= w then return end
  if y < 0 or y >= h then return end
  buf[y * w + x + 1] = v
end

local function get_cell(buf, x, y, w, h)
  if x < 0 or x >= w then return 0 end
  if y < 0 or y >= h then return 0 end
  return buf[y * w + x + 1] or 0
end

function pre(ctx, cursor)
  local c = ctx.cols or 0
  local r = ctx.rows or 0
  if c <= 0 or r <= 0 then return end

  if c ~= cols or r ~= rows then
    cols, rows = c, r
    local len = cols * rows * 2
    for i = 1, len do
      local v = r01()
      a[i] = v
      b[i] = v
    end
    for i = len + 1, #a do a[i] = nil end
    for i = len + 1, #b do b[i] = nil end
  end

  local frame = ctx.frame or 0
  local prev = (frame % 2 == 0) and a or b
  local curr = (frame % 2 == 0) and b or a
  local w = cols
  local h = rows * 2

  cursor = cursor or {}
  local pressed = ((cursor.left == true) and not (cursor.p and cursor.p.left == true))
    or ((cursor.right == true) and not (cursor.p and cursor.p.right == true))

  -- Fill a random 11x11 rect centered on click position.
  if pressed then
    local cx = math.floor(cursor.x or 0)
    local cy = math.floor((cursor.y or 0) * 2)
    local s = 5
    for y = cy - s, cy + s do
      for x = cx - s, cx + s do
        set_cell(prev, x, y, w, h, r01())
      end
    end
  end

  -- Update the automata
  for y = 0, h - 1 do
    for x = 0, w - 1 do
      local current = get_cell(prev, x, y, w, h)
      local neighbors =
        get_cell(prev, x - 1, y - 1, w, h) +
        get_cell(prev, x,     y - 1, w, h) +
        get_cell(prev, x + 1, y - 1, w, h) +
        get_cell(prev, x - 1, y,     w, h) +
        get_cell(prev, x + 1, y,     w, h) +
        get_cell(prev, x - 1, y + 1, w, h) +
        get_cell(prev, x,     y + 1, w, h) +
        get_cell(prev, x + 1, y + 1, w, h)

      local i1 = x + y * w + 1
      if current == 1 then
        curr[i1] = (neighbors == 2 or neighbors == 3) and 1 or 0
      else
        curr[i1] = (neighbors == 3) and 1 or 0
      end
    end
  end
end

function main(coord, ctx)
  local frame = ctx.frame or 0
  local curr = (frame % 2 == 0) and b or a -- matches pre() selecting (frame+1)%2 in JS

  local w = ctx.cols or cols
  if w <= 0 then return " " end

  local idx0 = coord.x + coord.y * 2 * w
  local upper = curr[idx0 + 1] or 0
  local lower = curr[idx0 + w + 1] or 0

  if upper == 1 and lower == 1 then return "█" end
  if upper == 1 then return "▀" end
  if lower == 1 then return "▄" end
  return " "
end


