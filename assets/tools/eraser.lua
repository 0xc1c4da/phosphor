settings = {
  id = "eraser",
  icon = "🧽",
  label = "Eraser",

  -- Tool parameters (host renders UI; values are available under ctx.params.*)
  params = {
    size = { type = "int", label = "Size", min = 1, max = 50, step = 1, default = 1 },
  },
}

local function clamp(v, a, b)
  if v < a then return a end
  if v > b then return b end
  return v
end

local function erase(ctx, layer, x, y)
  if not ctx or not layer then return end
  local p = ctx.params or {}

  local cols = tonumber(ctx.cols) or 0
  if cols <= 0 then return end
  if x < 0 or x >= cols then return end
  if y < 0 then return end

  local size = tonumber(p.size) or 1
  if size < 1 then size = 1 end
  if size > 200 then size = 200 end
  local r = math.floor(size / 2)

  local function erase_cell(px, py)
    if px < 0 or px >= cols then return end
    if py < 0 then return end

    -- Unset character by writing space.
    layer:set(px, py, " ")

    -- Unset per-cell fg/bg (if supported by host).
    if layer.clearStyle then
      layer:clearStyle(px, py)
    end
  end

  for dy = -r, r do
    for dx = -r, r do
      erase_cell(x + dx, y + dy)
    end
  end
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

  -- Phase 1: mouse drag erasing (left/right click+hold).
  if phase == 1 then
    local cursor = ctx.cursor
    if type(cursor) == "table" and cursor.valid and (cursor.left or cursor.right) then
      local px = tonumber(cursor.p and cursor.p.x) or tonumber(cursor.x) or caret.x
      local py = tonumber(cursor.p and cursor.p.y) or tonumber(cursor.y) or caret.y
      local prev_left = (cursor.p and cursor.p.left) == true
      local prev_right = (cursor.p and cursor.p.right) == true

      -- Only erase when moving between cells or on press edge (match pencil semantics).
      local moved_cell = (tonumber(cursor.x) ~= px) or (tonumber(cursor.y) ~= py)
      local pressed_edge = (cursor.left and not prev_left) or (cursor.right and not prev_right)

      if moved_cell or pressed_edge then
        caret.x = clamp(tonumber(cursor.x) or caret.x, 0, cols - 1)
        caret.y = math.max(0, tonumber(cursor.y) or caret.y)
        erase(ctx, layer, caret.x, caret.y)
      end
    end
    return
  end

  -- Phase 0: keyboard navigation erases after moving.
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
    erase(ctx, layer, caret.x, caret.y)
  end
end