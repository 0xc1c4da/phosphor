-- Pathfinder
-- Click to spawn new path segments.
--
-- Notes for the native editor runtime:
-- - The C++ "classic main()" shim does NOT automatically write main()'s output into `buffer`.
--   If you want persistent per-cell state across frames, you must write it yourself.
-- - Cursor state lives in `ctx.cursor` (passed as `cursor` arg here):
--     cursor.left / cursor.right
--     cursor.p.left / cursor.p.right  (previous frame)

settings = {
  fps = 30,
  backgroundColour = "#000000",
}

local utf8chars = ansl.string.utf8chars

local width, height = 0, 0
local prevFrame = {} -- previous frame's chars (1D, 1-based)

-- Glyph sets
local roads = "┃━┏┓┗┛┣┫┳┻╋"
local roads_chars = utf8chars(roads)

-- Weighted choices (kept identical to the JS example)
local w_from_top    = utf8chars("┃┃┃┃┃┃┃┃┃┃┃┃┃┃┃┃┃┃┃┃┗┫┣┻╋")
local w_from_bottom = utf8chars("┃┃┃┃┃┃┃┃┃┃┃┃┃┃┃┃┃┃┃┃┏┓┣┫┳╋")
local w_from_left   = utf8chars("━━━━━━━━━━━━━━━━━━━━┓┛┫┳┻╋")
local w_from_right  = utf8chars("━━━━━━━━━━━━━━━━━━━━┏┗┣┳┻╋")

local function choose(list)
  local n = #list
  if n <= 0 then return " " end
  return list[math.random(1, n)] or " "
end

local function contains(set_utf8, ch)
  if not ch or ch == "" then return false end
  return (string.find(set_utf8, ch, 1, true) ~= nil)
end

local function idx1(x, y)
  return y * width + x + 1
end

local function get_prev(x, y)
  if x < 0 or x >= width then return "" end
  if y < 0 or y >= height then return "" end
  return prevFrame[idx1(x, y)] or " "
end

function pre(ctx, cursor, buffer)
  local cols = ctx.cols or 0
  local rows = ctx.rows or 0
  if cols <= 0 or rows <= 0 then return end

  if cols ~= width or rows ~= height then
    width, height = cols, rows
    local len = width * height

    -- Initialize persistent state in `buffer` (1 char per cell).
    for i = 1, len do
      buffer[i] = (math.random() < 0.001) and choose(roads_chars) or " "
    end
    for i = len + 1, #buffer do buffer[i] = nil end
  end

  -- Copy previous frame (shallow, but we only store strings).
  local len = width * height
  for i = 1, len do
    prevFrame[i] = buffer[i] or " "
  end
  for i = len + 1, #prevFrame do prevFrame[i] = nil end
end

function main(coord, ctx, cursor, buffer)
  local x = coord.x or 0
  local y = coord.y or 0
  local i = (coord.index or (x + y * (ctx.cols or width))) + 1

  cursor = cursor or {}
  local pressed = (cursor.valid == true)
    and (cursor.left == true)
    and not (cursor.p and cursor.p.left == true)

  -- Click to spawn a new segment in an empty cell.
  if pressed and (cursor.x == x) and (cursor.y == y) and (get_prev(x, y) == " ") then
    local ch = choose(roads_chars)
    buffer[i] = ch
    return ch
  end

  local last = get_prev(x, y)
  if last == " " then
    local ch = " "

    local top = get_prev(x, y - 1)
    local bottom = get_prev(x, y + 1)
    local left = get_prev(x - 1, y)
    local right = get_prev(x + 1, y)

    if contains("┃┫┣╋┏┓┳", top) then
      ch = choose(w_from_top)
    elseif contains("┃┗┛┣┫┻╋", bottom) then
      ch = choose(w_from_bottom)
    elseif contains("━┏┗┣┳┻╋", left) then
      ch = choose(w_from_left)
    elseif contains("━┓┛┫┳┻╋", right) then
      ch = choose(w_from_right)
    end

    buffer[i] = ch
    return ch
  end

  buffer[i] = last
  return last
end


