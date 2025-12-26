settings = {
    id = "10-move-layer",
    icon = "✋",
    label = "Move Layer",
    shortcut = "Ctrl+Alt+M"
}

-- Move Layer tool:
-- Non-destructively translates the active layer by modifying its persisted (offset_x, offset_y).
-- - Mouse: click+drag to move, release to stop; right-click cancels current drag.
-- - Keyboard: arrow keys nudge; Shift+arrow nudges by 10; Esc cancels current drag.

local dragging = false
local drag_start_x = 0
local drag_start_y = 0
local drag_base_off_x = 0
local drag_base_off_y = 0

local function to_int(v, def)
  v = tonumber(v)
  if v == nil then return def end
  return math.floor(v)
end

local function begin_drag(canvas, cursor)
  if not canvas or not cursor or cursor.valid ~= true then return false end
  local x0 = to_int(cursor.x, 0)
  local y0 = to_int(cursor.y, 0)
  local ox, oy = canvas:getLayerOffset()
  ox = to_int(ox, 0)
  oy = to_int(oy, 0)

  dragging = true
  drag_start_x = x0
  drag_start_y = y0
  drag_base_off_x = ox
  drag_base_off_y = oy
  return true
end

local function cancel_drag(canvas)
  if not dragging then return end
  dragging = false
  if canvas then
    canvas:setLayerOffset(drag_base_off_x, drag_base_off_y)
  end
end

local function update_drag(canvas, cursor)
  if not dragging or not canvas or not cursor or cursor.valid ~= true then return end
  local x = to_int(cursor.x, drag_start_x)
  local y = to_int(cursor.y, drag_start_y)
  local dx = x - drag_start_x
  local dy = y - drag_start_y
  canvas:setLayerOffset(drag_base_off_x + dx, drag_base_off_y + dy)
end

local function end_drag()
  dragging = false
end

function render(ctx, layer)
  if not ctx or not layer then return end
  if not ctx.focused then return end

  local canvas = ctx.canvas
  if canvas == nil then return end

  local phase = to_int(ctx.phase, 0)
  local keys = ctx.keys or {}
  local mods = ctx.mods or {}
  local cursor = ctx.cursor or {}

  -- Phase 0: keyboard nudges + cancel.
  if phase == 0 then
    if keys.escape == true then
      cancel_drag(canvas)
      return
    end

    if dragging then
      return
    end

    local step = (mods.shift == true) and 10 or 1
    local dx = 0
    local dy = 0
    if keys.left  == true then dx = dx - step end
    if keys.right == true then dx = dx + step end
    if keys.up    == true then dy = dy - step end
    if keys.down  == true then dy = dy + step end

    if dx ~= 0 or dy ~= 0 then
      canvas:nudgeLayerOffset(dx, dy)
    end
    return
  end

  -- Phase 1: mouse drag.
  if phase == 1 then
    if cursor.valid ~= true then return end

    local left = (cursor.left == true)
    local right = (cursor.right == true)
    local prev_left = (cursor.p and cursor.p.left) == true
    local prev_right = (cursor.p and cursor.p.right) == true

    local press_left = left and (not prev_left)
    local release_left = (not left) and prev_left
    local press_right = right and (not prev_right)

    if dragging then
      if left then
        update_drag(canvas, cursor)
      end
      if release_left then
        end_drag()
      end
      if press_right then
        cancel_drag(canvas)
      end
      return
    end

    if press_left then
      if begin_drag(canvas, cursor) then
        update_drag(canvas, cursor)
      end
    end
  end
end

