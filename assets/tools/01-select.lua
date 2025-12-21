settings = {
  id = "01-select",
  icon = "⬚",
  label = "Select",

  -- Action routing hints (used by host Action Router).
  -- - When active, this tool handles selection ops + clipboard + selection transforms.
  -- - When inactive, we still want selection/clipboard actions to work as a fallback.
  handles = {
    { action = "selection.clear_or_cancel", when = "active" },
    { action = "selection.delete", when = "active" },
    { action = "edit.select_all", when = "active" },
    { action = "edit.copy", when = "active" },
    { action = "edit.cut", when = "active" },
    { action = "edit.paste", when = "active" },
    { action = "selection.op.rotate_cw", when = "active" },
    { action = "selection.op.flip_x", when = "active" },
    { action = "selection.op.flip_y", when = "active" },
    { action = "selection.op.center", when = "active" },
    { action = "selection.crop", when = "active" },

    { action = "selection.clear_or_cancel", when = "inactive" },
    { action = "selection.delete", when = "inactive" },
    { action = "edit.select_all", when = "inactive" },
    { action = "edit.copy", when = "inactive" },
    { action = "edit.cut", when = "inactive" },
    { action = "edit.paste", when = "inactive" },
  },

  -- Tool parameters (host renders UI; values are available under ctx.params.*)
  params = {
    -- Stable ordering for host UI.
    copyMode = { type = "enum", label = "Copy Mode", items = { "layer", "composite" }, default = "layer" },
    pasteMode = { type = "enum", label = "Paste Mode", items = { "both", "char", "color" }, default = "both" },
    transparentSpaces = { type = "bool", label = "Paste: Transparent spaces", default = false },

    -- Selection transforms (UI triggers)
    transform = {
      type = "enum",
      label = "Transform",
      items = { "none", "rotate_cw", "flip_x", "flip_y", "center", "crop_to_selection" },
      default = "none",
      order = 50,
    },
    applyTransform = { type = "button", label = "Apply", sameLine = true, order = 51 },
  },
}

local selecting = false
local sel_x0 = 0
local sel_y0 = 0

local function is_table(t) return type(t) == "table" end

local function to_int(v, def)
  v = tonumber(v)
  if v == nil then return def end
  return math.floor(v)
end

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

-- Read the selection contents from the active layer.
-- Returns: x, y, w, h, cells[] where cells is row-major array of {cp=..., fg=..., bg=..., attrs=...}
local function read_selection(canvas, layer)
  if not canvas or not layer or not canvas:hasSelection() then
    return nil
  end
  local x, y, w, h = canvas:getSelection()
  x = to_int(x, 0); y = to_int(y, 0); w = to_int(w, 0); h = to_int(h, 0)
  if w <= 0 or h <= 0 then return nil end

  local cells = {}
  local idx = 1
  for j = 0, h - 1 do
    for i = 0, w - 1 do
      local _, fg, bg, cp, attrs = layer:get(x + i, y + j)
      cells[idx] = { cp = to_int(cp, 32), fg = fg, bg = bg, attrs = to_int(attrs, 0) }
      idx = idx + 1
    end
  end
  return x, y, w, h, cells
end

local function write_cell(layer, x, y, cell)
  if cell == nil then return end
  local cp = cell.cp
  local fg = cell.fg
  local bg = cell.bg
  local attrs = cell.attrs
  if type(attrs) ~= "number" then attrs = 0 end
  if fg == nil and bg == nil and attrs == 0 then
    layer:set(x, y, cp)
    layer:clearStyle(x, y)
  else
    layer:set(x, y, cp, fg, bg, attrs)
  end
end

local function clear_cell(layer, x, y)
  layer:set(x, y, " ")
  layer:clearStyle(x, y)
end

local function clear_rect(layer, x, y, w, h)
  if w <= 0 or h <= 0 then return end
  for j = 0, h - 1 do
    for i = 0, w - 1 do
      clear_cell(layer, x + i, y + j)
    end
  end
end

local function round_int(v)
  return math.floor(v + 0.5)
end

local function commit_if_moving(canvas)
  if canvas and canvas:isMovingSelection() then
    canvas:commitMoveSelection()
  end
end

local function selection_flip_x(ctx, canvas, layer)
  if not canvas:hasSelection() then return false end
  commit_if_moving(canvas)
  local x, y, w, h, src = read_selection(canvas, layer)
  if not x then return false end
  local idx = 1
  for j = 0, h - 1 do
    for i = 0, w - 1 do
      local si = (w - 1 - i)
      local sidx = (j * w) + si + 1
      write_cell(layer, x + i, y + j, src[sidx])
      idx = idx + 1
    end
  end
  return true
end

local function selection_flip_y(ctx, canvas, layer)
  if not canvas:hasSelection() then return false end
  commit_if_moving(canvas)
  local x, y, w, h, src = read_selection(canvas, layer)
  if not x then return false end
  for j = 0, h - 1 do
    for i = 0, w - 1 do
      local sj = (h - 1 - j)
      local sidx = (sj * w) + i + 1
      write_cell(layer, x + i, y + j, src[sidx])
    end
  end
  return true
end

local function selection_rotate_cw(ctx, canvas, layer, cols)
  if not canvas:hasSelection() then return false end
  commit_if_moving(canvas)
  local x, y, w, h, src = read_selection(canvas, layer)
  if not x then return false end

  local new_w = h
  local new_h = w
  if cols and new_w > cols then
    return false
  end

  -- Rotate around center to minimize drift.
  local cx = x + (w - 1) / 2.0
  local cy = y + (h - 1) / 2.0
  local nx = round_int(cx - (new_w - 1) / 2.0)
  local ny = round_int(cy - (new_h - 1) / 2.0)
  if cols then
    nx = clamp(nx, 0, math.max(0, cols - new_w))
  else
    if nx < 0 then nx = 0 end
  end
  if ny < 0 then ny = 0 end

  -- Build rotated buffer (row-major).
  local dst = {}
  for oy = 0, h - 1 do
    for ox = 0, w - 1 do
      local sidx = (oy * w) + ox + 1
      local dx = (h - 1 - oy)
      local dy = ox
      local didx = (dy * new_w) + dx + 1
      dst[didx] = src[sidx]
    end
  end

  -- Clear old rect, then write new.
  clear_rect(layer, x, y, w, h)
  for j = 0, new_h - 1 do
    for i = 0, new_w - 1 do
      local didx = (j * new_w) + i + 1
      write_cell(layer, nx + i, ny + j, dst[didx])
    end
  end

  canvas:setSelection(nx, ny, nx + new_w - 1, ny + new_h - 1)
  return true
end

local function selection_center(ctx, canvas, layer, cols, rows)
  if not canvas:hasSelection() then return false end
  commit_if_moving(canvas)
  local x, y, w, h, src = read_selection(canvas, layer)
  if not x then return false end
  if cols and w > cols then return false end

  local nx = 0
  local ny = 0
  if cols then nx = math.floor((cols - w) / 2) end
  if rows then ny = math.floor((rows - h) / 2) end
  if nx < 0 then nx = 0 end
  if ny < 0 then ny = 0 end
  if cols then nx = clamp(nx, 0, math.max(0, cols - w)) end

  if nx == x and ny == y then
    return true
  end

  clear_rect(layer, x, y, w, h)
  for j = 0, h - 1 do
    for i = 0, w - 1 do
      local idx = (j * w) + i + 1
      write_cell(layer, nx + i, ny + j, src[idx])
    end
  end
  canvas:setSelection(nx, ny, nx + w - 1, ny + h - 1)
  return true
end

-- NOTE: True crop (changing canvas dimensions) is not currently available from Lua.
-- This implements a "crop contents" surrogate on the active layer:
--   - moves the selection to (0,0)
--   - clears everything else in the current visible rows/cols
local function selection_crop_contents(ctx, canvas, layer, cols, rows)
  if not canvas:hasSelection() then return false end
  commit_if_moving(canvas)
  local x, y, w, h, src = read_selection(canvas, layer)
  if not x then return false end
  if cols and w > cols then return false end

  cols = to_int(cols, 0)
  rows = to_int(rows, 0)
  if cols <= 0 or rows <= 0 then
    return false
  end

  -- Clear entire visible region.
  for yy = 0, rows - 1 do
    for xx = 0, cols - 1 do
      clear_cell(layer, xx, yy)
    end
  end

  -- Paste selection at origin.
  for j = 0, h - 1 do
    for i = 0, w - 1 do
      local idx = (j * w) + i + 1
      write_cell(layer, i, j, src[idx])
    end
  end
  canvas:setSelection(0, 0, w - 1, h - 1)
  return true
end

function render(ctx, layer)
  if not ctx or not layer then return end
  if not ctx.focused then return end

  local canvas = ctx.canvas
  if canvas == nil then return end

  local phase = to_int(ctx.phase, 0)
  local cols = to_int(ctx.cols, 0)
  local rows = to_int(ctx.rows, 0)
  local caret = ctx.caret or {}
  local mods = ctx.mods or {}
  local hotkeys = ctx.hotkeys or {}
  local actions = ctx.actions or {}
  local p = ctx.params or {}
  local keys = ctx.keys or {}

  -- Phase 0: keyboard shortcuts.
  if phase == 0 then
    -- UI-driven transforms (button).
    if (p.applyTransform == true) and canvas:hasSelection() then
      local op = p.transform
      if type(op) ~= "string" then op = "none" end
      if op == "rotate_cw" then
        selection_rotate_cw(ctx, canvas, layer, cols)
      elseif op == "flip_x" then
        selection_flip_x(ctx, canvas, layer)
      elseif op == "flip_y" then
        selection_flip_y(ctx, canvas, layer)
      elseif op == "center" then
        selection_center(ctx, canvas, layer, cols, rows)
      elseif op == "crop_to_selection" then
        if ctx.out ~= nil then
          ctx.out[#ctx.out + 1] = { type = "canvas.crop_to_selection" }
        else
          selection_crop_contents(ctx, canvas, layer, cols, rows)
        end
      end
      selecting = false
    end

    -- Cancel / clear.
    if hotkeys.cancel or actions["selection.clear_or_cancel"] then
      if canvas:isMovingSelection() then
        canvas:cancelMoveSelection()
      else
        canvas:clearSelection()
      end
      selecting = false
      return
    end

    -- Select all.
    if (hotkeys.selectAll or actions["edit.select_all"]) and cols > 0 and rows > 0 then
      canvas:setSelection(0, 0, cols - 1, rows - 1)
      selecting = false
      return
    end

    -- Clipboard operations.
    if hotkeys.copy or actions["edit.copy"] then
      local mode = p.copyMode
      if type(mode) ~= "string" then mode = "layer" end
      canvas:copySelection(mode)
      return
    end
    if hotkeys.cut or actions["edit.cut"] then
      -- Cut is always per-layer (destructive). Copy mode doesn't apply.
      canvas:cutSelection()
      selecting = false
      return
    end
    if hotkeys.paste or actions["edit.paste"] then
      local x = to_int(caret.x, 0)
      local y = to_int(caret.y, 0)
      local mode = p.pasteMode
      if type(mode) ~= "string" then mode = "both" end
      local transparent = (p.transparentSpaces == true)
      canvas:pasteClipboard(x, y, nil, mode, transparent)
      selecting = false
      return
    end

    -- Backspace: delete selection contents (match eraser/edit-tool "backspace clears" behavior).
    -- NOTE: Backspace is bound to the `selection.delete` action in key-bindings.json.
    -- We intentionally do not special-case `keys.backspace` here to avoid double-execution
    -- when the host Action Router dispatches `selection.delete`.

    -- Delete selection contents.
    if (hotkeys.deleteSelection or actions["selection.delete"]) and canvas:hasSelection() then
      canvas:deleteSelection()
      selecting = false
      return
    end

    -- Selection transforms (key-bindings/actions; tool-gated here in Select tool).
    if actions["selection.op.rotate_cw"] and canvas:hasSelection() then
      if selection_rotate_cw(ctx, canvas, layer, cols) then
        selecting = false
        return
      end
    end
    if actions["selection.op.flip_x"] and canvas:hasSelection() then
      if selection_flip_x(ctx, canvas, layer) then
        selecting = false
        return
      end
    end
    if actions["selection.op.flip_y"] and canvas:hasSelection() then
      if selection_flip_y(ctx, canvas, layer) then
        selecting = false
        return
      end
    end
    if actions["selection.op.center"] and canvas:hasSelection() then
      if selection_center(ctx, canvas, layer, cols, rows) then
        selecting = false
        return
      end
    end
    if actions["selection.crop"] and canvas:hasSelection() then
      -- Prefer true crop (resize) via host command. Old hosts will ignore unknown commands.
      if ctx.out ~= nil then
        ctx.out[#ctx.out + 1] = { type = "canvas.crop_to_selection" }
        selecting = false
        return
      end
      -- Fallback: "crop contents" (does not resize geometry).
      if selection_crop_contents(ctx, canvas, layer, cols, rows) then
        selecting = false
        return
      end
    end

    return
  end

  -- Phase 1: mouse interactions.
  local cursor = ctx.cursor
  if not is_table(cursor) or not cursor.valid then return end

  local x = to_int(cursor.x, 0)
  local y = to_int(cursor.y, 0)

  local prev = cursor.p or {}
  local left = (cursor.left == true)
  local right = (cursor.right == true)
  local prev_left = (prev.left == true)
  local prev_right = (prev.right == true)

  local press_left = left and not prev_left
  local release_left = (not left) and prev_left
  local press_right = right and not prev_right

  -- Right-click: clear selection (if not actively moving).
  if press_right and not canvas:isMovingSelection() then
    canvas:clearSelection()
    selecting = false
    return
  end

  -- Moving an existing selection (floating preview).
  if canvas:isMovingSelection() then
    if left then
      canvas:updateMoveSelection(x, y)
    end
    if release_left then
      canvas:commitMoveSelection()
    end
    if press_right then
      canvas:cancelMoveSelection()
    end
    return
  end

  -- Begin marquee select or begin move on press.
  if press_left then
    if canvas:hasSelection() and canvas:selectionContains(x, y) then
      -- Ctrl-drag duplicates instead of moving.
      local duplicate = (mods.ctrl == true)
      canvas:beginMoveSelection(x, y, duplicate)
      selecting = false
    else
      selecting = true
      sel_x0 = x
      sel_y0 = y
      canvas:setSelection(sel_x0, sel_y0, sel_x0, sel_y0)
    end
  end

  -- Update marquee while dragging.
  if selecting and left then
    canvas:setSelection(sel_x0, sel_y0, x, y)
  end

  if selecting and release_left then
    selecting = false
  end
end


