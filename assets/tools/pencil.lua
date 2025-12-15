settings = { icon = "🖉", label = "Pencil" }

local function clamp(v, a, b)
  if v < a then return a end
  if v > b then return b end
  return v
end

local function paint(ctx, layer, x, y)
  if not ctx or not layer then return end
  local ch = ctx.brush
  if type(ch) ~= "string" or #ch == 0 then ch = " " end

  local fg = ctx.fg
  if type(fg) ~= "number" then fg = nil end
  local bg = ctx.bg
  if type(bg) ~= "number" then bg = nil end

  layer:set(x, y, ch, fg, bg)
end

function render(ctx, layer)
  if not ctx or not layer then return end
  if not ctx.focused then return end

  local cols = tonumber(ctx.cols) or 0
  if cols <= 0 then return end

  local caret = ctx.caret
  if type(caret) ~= "table" then return end

  caret.x = clamp(tonumber(caret.x) or 0, 0, cols - 1)
  caret.y = math.max(0, tonumber(caret.y) or 0)

  local phase = tonumber(ctx.phase) or 0

  -- Phase 1: mouse drag painting (left click+hold).
  if phase == 1 then
    local cursor = ctx.cursor
    if type(cursor) == "table" and cursor.valid and cursor.left then
      caret.x = clamp(tonumber(cursor.x) or caret.x, 0, cols - 1)
      caret.y = math.max(0, tonumber(cursor.y) or caret.y)
      paint(ctx, layer, caret.x, caret.y)
    end
    return
  end

  -- Phase 0: keyboard navigation paints after moving.
  local keys = ctx.keys or {}
  local moved = false

  if keys.left then
    if caret.x > 0 then
      caret.x = caret.x - 1
    elseif caret.y > 0 then
      caret.y = caret.y - 1
      caret.x = cols - 1
    end
    moved = true
  end
  if keys.right then
    if caret.x < cols - 1 then
      caret.x = caret.x + 1
    else
      caret.y = caret.y + 1
      caret.x = 0
    end
    moved = true
  end
  if keys.up then
    if caret.y > 0 then
      caret.y = caret.y - 1
      moved = true
    end
  end
  if keys.down then
    caret.y = caret.y + 1
    moved = true
  end
  if keys.home then
    caret.x = 0
    moved = true
  end
  if keys["end"] then
    caret.x = cols - 1
    moved = true
  end

  if moved then
    paint(ctx, layer, caret.x, caret.y)
  end
end
