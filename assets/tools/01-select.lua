settings = {
  id = "01-select",
  icon = "⬚",
  label = "Select",
  -- PabloDraw selection tool = Alt+E
  shortcut = "Alt+E",

  -- Action routing hints (used by host Action Router).
  -- - When active, this tool handles selection ops + clipboard + selection transforms.
  -- - When inactive, we still want selection/clipboard actions to work as a fallback.
  handles = {
    -- When active: Enter is used for keyboard-driven selection start/end (not create a new line).
    { action = "editor.new_line", when = "active" },
    { action = "selection.clear_or_cancel", when = "active" },
    { action = "selection.clear", when = "active" },
    { action = "selection.delete_destructive", when = "active" },
    { action = "selection.shift_delete", when = "active" },
    { action = "selection.remove_row_shift_up", when = "active" },
    { action = "selection.remove_col_shift_left", when = "active" },
    { action = "selection.insert_row_shift_down", when = "active" },
    { action = "selection.insert_col_shift_right", when = "active" },
    { action = "selection.select_row", when = "active" },
    { action = "selection.select_column", when = "active" },
    { action = "selection.select_row_to_start", when = "active" },
    { action = "selection.select_row_to_end", when = "active" },
    { action = "selection.select_column_to_top", when = "active" },
    { action = "selection.select_column_to_bottom", when = "active" },
    { action = "edit.select_all", when = "active" },
    { action = "edit.copy", when = "active" },
    { action = "edit.cut", when = "active" },
    { action = "edit.paste", when = "active" },
    { action = "selection.op.rotate_cw", when = "active" },
    { action = "selection.op.flip_x", when = "active" },
    { action = "selection.op.flip_y", when = "active" },
    { action = "selection.op.center", when = "active" },
    { action = "selection.crop", when = "active" },
    -- Selection verb language (Moebius/PabloDraw-style).
    { action = "selection.op.move", when = "active" },
    { action = "selection.op.copy", when = "active" },
    { action = "selection.op.fill", when = "active" },
    { action = "selection.op.erase", when = "active" },
    { action = "selection.op.stamp", when = "active" },
    { action = "selection.op.place", when = "active" },

    { action = "selection.clear_or_cancel", when = "inactive" },
    { action = "selection.clear", when = "inactive" },
    { action = "selection.delete_destructive", when = "inactive" },
    { action = "selection.shift_delete", when = "inactive" },
    { action = "selection.remove_row_shift_up", when = "inactive" },
    { action = "selection.remove_col_shift_left", when = "inactive" },
    { action = "selection.insert_row_shift_down", when = "inactive" },
    { action = "selection.insert_col_shift_right", when = "inactive" },
    { action = "selection.select_row", when = "inactive" },
    { action = "selection.select_column", when = "inactive" },
    { action = "selection.select_row_to_start", when = "inactive" },
    { action = "selection.select_row_to_end", when = "inactive" },
    { action = "selection.select_column_to_top", when = "inactive" },
    { action = "selection.select_column_to_bottom", when = "inactive" },
    { action = "edit.select_all", when = "inactive" },
    { action = "edit.copy", when = "inactive" },
    { action = "edit.cut", when = "inactive" },
    { action = "edit.paste", when = "inactive" },

    -- Selection transforms should also work as fallback handlers (Moebius/PabloDraw feel):
    -- invoke transforms without switching tools, as long as a selection exists.
    { action = "selection.op.rotate_cw", when = "inactive" },
    { action = "selection.op.flip_x", when = "inactive" },
    { action = "selection.op.flip_y", when = "inactive" },
    { action = "selection.op.center", when = "inactive" },
    { action = "selection.crop", when = "inactive" },
    -- Selection verb language as fallback handlers too. Note: move/copy typically auto-switches
    -- to Select tool so arrow-key nudging is handled here (see host router).
    { action = "selection.op.move", when = "inactive" },
    { action = "selection.op.copy", when = "inactive" },
    { action = "selection.op.fill", when = "inactive" },
    { action = "selection.op.erase", when = "inactive" },
    { action = "selection.op.stamp", when = "inactive" },
    { action = "selection.op.place", when = "inactive" },
  },

  -- Tool parameters (host renders UI; values are available under ctx.params.*)
  params = {
    -- Stable ordering for host UI.
    copyMode = { type = "enum", label = "Copy", ui = "segmented", section = "Clipboard", placement = "quick", items = { "layer", "composite" }, default = "layer" },
    pasteMode = { type = "enum", label = "Paste", ui = "segmented", section = "Clipboard", placement = "quick", items = { "both", "char", "colour" }, default = "both" },
    transparentSpaces = { type = "bool", label = "Transparent spaces", ui = "toggle", section = "Clipboard", placement = "quick", default = false, inline = true },

    -- Selection transforms (direct action buttons)
    rotateCW = { type = "button", label = "Rotate", ui = "action", section = "Transform", placement = "quick" },
    flipX = { type = "button", label = "Flip X", ui = "action", section = "Transform", placement = "quick", inline = true },
    flipY = { type = "button", label = "Flip Y", ui = "action", section = "Transform", placement = "quick", inline = true },
    center = { type = "button", label = "Center", ui = "action", section = "Transform", placement = "quick", inline = true },
    crop = { type = "button", label = "Crop", ui = "action", section = "Transform", placement = "quick", inline = true },

    -- Selection operation options (rect fill/erase).
    -- Note: this is distinct from the Fill tool (05-fill.lua), which is a flood-fill.
    verbFillMode = { type = "enum", label = "Rect fill", ui = "segmented", section = "Selection Ops", placement = "quick", items = { "both", "char", "colour" }, default = "both" },
    verbUseFg = { type = "bool", label = "FG", ui = "toggle", section = "Selection Ops", placement = "quick", default = true, inline = true },
    verbUseBg = { type = "bool", label = "BG", ui = "toggle", section = "Selection Ops", placement = "quick", default = true, inline = true },
    verbUseAttrs = { type = "bool", label = "Attrs", ui = "toggle", section = "Selection Ops", placement = "quick", default = true, inline = true },
  },
}

local selecting = false
local selecting_mode = nil -- "keyboard" | "mouse" | nil
local sel_x0 = 0
local sel_y0 = 0
local prev_enter_down = false
local last_hover_x = nil
local last_hover_y = nil

-- Selection "verb language" state (for stamp/place workflows).
-- We keep this minimal and derived from canvas state where possible.
local move_mode = nil -- "move" | "copy" | nil
local move_src_x = nil
local move_src_y = nil
local move_src_w = nil
local move_src_h = nil

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

-- Return (min, max) ordering for selection bounds.
local function reorientate(a, b)
  if a <= b then return a, b end
  return b, a
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

local function get_brush_state(ctx)
  if not ctx then return " ", nil, nil, 0 end

  local fg = ctx.fg
  if type(fg) ~= "number" then fg = nil end
  local bg = ctx.bg
  if type(bg) ~= "number" then bg = nil end
  local attrs = ctx.attrs
  if type(attrs) ~= "number" then attrs = 0 end
  attrs = math.floor(attrs)
  if attrs < 0 then attrs = 0 end

  local brush = ctx.glyph
  if type(brush) ~= "string" or #brush == 0 then brush = " " end
  local brush_glyph_id = ctx.glyphId
  local brush_arg = brush
  if type(brush_glyph_id) == "number" and brush_glyph_id >= 0x80000000 then
    brush_arg = brush_glyph_id
  end

  return brush_arg, fg, bg, attrs
end

local function selection_erase(ctx, canvas, layer)
  if not canvas or not layer or not canvas:hasSelection() then return false end
  commit_if_moving(canvas)
  local x, y, w, h = canvas:getSelection()
  x = to_int(x, 0); y = to_int(y, 0); w = to_int(w, 0); h = to_int(h, 0)
  if w <= 0 or h <= 0 then return false end
  clear_rect(layer, x, y, w, h)
  return true
end

local function selection_fill(ctx, canvas, layer)
  if not canvas or not layer or not canvas:hasSelection() then return false end
  commit_if_moving(canvas)
  local x, y, w, h = canvas:getSelection()
  x = to_int(x, 0); y = to_int(y, 0); w = to_int(w, 0); h = to_int(h, 0)
  if w <= 0 or h <= 0 then return false end

  local brush_arg, fg, bg, attrs = get_brush_state(ctx)
  local p = (ctx and ctx.params) or {}
  local mode = p.verbFillMode
  if type(mode) ~= "string" then mode = "both" end
  local useFg = (p.verbUseFg ~= false)
  local useBg = (p.verbUseBg ~= false)
  local useAttrs = (p.verbUseAttrs ~= false)

  local fg_arg = (useFg and type(fg) == "number") and fg or nil
  local bg_arg = (useBg and type(bg) == "number") and bg or nil
  local attrs_arg = (useAttrs and type(attrs) == "number") and attrs or nil

  for j = 0, h - 1 do
    for i = 0, w - 1 do
      local px = x + i
      local py = y + j
      if mode == "colour" then
        -- Preserve glyph; apply only selected channels.
        local _, _, _, _, _, gid = layer:get(px, py)
        if type(gid) ~= "number" then
          -- Fallback: preserve as best-effort Unicode representative.
          local ch = layer:get(px, py)
          layer:set(px, py, ch, fg_arg, bg_arg, attrs_arg)
        else
          layer:set(px, py, gid, fg_arg, bg_arg, attrs_arg)
        end
      elseif mode == "char" then
        -- Apply glyph; preserve style unless a channel is explicitly enabled.
        layer:set(px, py, brush_arg, fg_arg, bg_arg, attrs_arg)
      else
        -- both
        layer:set(px, py, brush_arg, fg_arg, bg_arg, attrs_arg)
      end
    end
  end
  return true
end

local function begin_move_selection(canvas, copy)
  if not canvas or not canvas:hasSelection() then return false end
  if canvas:isMovingSelection() then return true end
  local x, y, w, h = canvas:getSelection()
  x = to_int(x, 0); y = to_int(y, 0); w = to_int(w, 0); h = to_int(h, 0)
  if w <= 0 or h <= 0 then return false end

  -- Grab at the top-left so UpdateMoveSelection(cursor_x,cursor_y) maps directly to dst_x/dst_y.
  if not canvas:beginMoveSelection(x, y, (copy == true)) then
    return false
  end

  move_mode = (copy == true) and "copy" or "move"
  move_src_x = x
  move_src_y = y
  move_src_w = w
  move_src_h = h
  return true
end

local function move_nudge(canvas, cols, rows, caret, keys)
  if not canvas or not canvas:isMovingSelection() then return false end
  local x, y, w, h = canvas:getSelection()
  x = to_int(x, 0); y = to_int(y, 0); w = to_int(w, 0); h = to_int(h, 0)
  if w <= 0 or h <= 0 then return false end

  local nx = x
  local ny = y
  local moved = false

  if keys.left then nx = nx - 1; moved = true end
  if keys.right then nx = nx + 1; moved = true end
  if keys.up then ny = ny - 1; moved = true end
  if keys.down then ny = ny + 1; moved = true end
  if not moved then return false end

  if type(cols) == "number" and cols > 0 then
    nx = clamp(nx, 0, math.max(0, cols - w))
  else
    if nx < 0 then nx = 0 end
  end
  if ny < 0 then ny = 0 end

  canvas:updateMoveSelection(nx, ny)
  if type(caret) == "table" then
    caret.x = nx
    caret.y = ny
  end
  return true
end

local function selection_place(canvas)
  if not canvas or not canvas:isMovingSelection() then return false end
  canvas:commitMoveSelection()
  move_mode = nil
  move_src_x = nil; move_src_y = nil; move_src_w = nil; move_src_h = nil
  return true
end

local function selection_stamp(canvas)
  if not canvas or not canvas:isMovingSelection() then return false end
  -- Stamp: commit a placement, but keep the move active (only meaningful for copy-mode).
  local dst_x, dst_y, _, _ = canvas:getSelection()
  dst_x = to_int(dst_x, 0); dst_y = to_int(dst_y, 0)

  canvas:commitMoveSelection()

  if move_mode ~= "copy" then
    -- For move-mode, "stamp" behaves like place.
    move_mode = nil
    move_src_x = nil; move_src_y = nil; move_src_w = nil; move_src_h = nil
    return true
  end
  if move_src_x == nil or move_src_y == nil or move_src_w == nil or move_src_h == nil then
    return true
  end

  -- Re-enter copy-move from the original source (which was never cleared), but keep the preview at the
  -- current destination so the user can keep stamping without teleporting back to the source.
  canvas:setSelection(move_src_x, move_src_y, move_src_x + move_src_w - 1, move_src_y + move_src_h - 1)
  if canvas:beginMoveSelection(move_src_x, move_src_y, true) then
    canvas:updateMoveSelection(dst_x, dst_y)
  end
  return true
end

local function selection_flip_x(ctx, canvas, layer)
  if not canvas:hasSelection() then return false end
  commit_if_moving(canvas)
  if ctx and ctx.out ~= nil then
    ctx.out[#ctx.out + 1] = { type = "canvas.selection.flip_x" }
    return true
  end
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
  if ctx and ctx.out ~= nil then
    ctx.out[#ctx.out + 1] = { type = "canvas.selection.flip_y" }
    return true
  end
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
  if ctx and ctx.out ~= nil then
    ctx.out[#ctx.out + 1] = { type = "canvas.selection.rotate_cw" }
    return true
  end
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
  if ctx and ctx.out ~= nil then
    ctx.out[#ctx.out + 1] = { type = "canvas.selection.center" }
    return true
  end
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

local function selection_select_row(canvas, cols, rows, caret)
  if not canvas or not caret or cols <= 0 then return false end
  rows = to_int(rows, 0)
  if rows <= 0 then rows = 1 end
  local y = to_int(caret.y, 0)
  y = clamp(y, 0, rows - 1)
  canvas:setSelection(0, y, cols - 1, y)
  return true
end

local function selection_select_row_to_start(canvas, cols, rows, caret)
  if not canvas or not caret or cols <= 0 then return false end
  rows = to_int(rows, 0)
  if rows <= 0 then rows = 1 end
  local y = to_int(caret.y, 0)
  y = clamp(y, 0, rows - 1)
  local x = to_int(caret.x, 0)
  x = clamp(x, 0, cols - 1)
  canvas:setSelection(0, y, x, y)
  return true
end

local function selection_select_row_to_end(canvas, cols, rows, caret)
  if not canvas or not caret or cols <= 0 then return false end
  rows = to_int(rows, 0)
  if rows <= 0 then rows = 1 end
  local y = to_int(caret.y, 0)
  y = clamp(y, 0, rows - 1)
  local x = to_int(caret.x, 0)
  x = clamp(x, 0, cols - 1)
  canvas:setSelection(x, y, cols - 1, y)
  return true
end

local function selection_select_column(canvas, cols, rows, caret)
  if not canvas or not caret or cols <= 0 then return false end
  rows = to_int(rows, 0)
  if rows <= 0 then rows = 1 end
  local x = to_int(caret.x, 0)
  x = clamp(x, 0, cols - 1)
  canvas:setSelection(x, 0, x, rows - 1)
  return true
end

local function selection_select_column_to_top(canvas, cols, rows, caret)
  if not canvas or not caret or cols <= 0 then return false end
  rows = to_int(rows, 0)
  if rows <= 0 then rows = 1 end
  local x = to_int(caret.x, 0)
  x = clamp(x, 0, cols - 1)
  local y = to_int(caret.y, 0)
  y = clamp(y, 0, rows - 1)
  canvas:setSelection(x, 0, x, y)
  return true
end

local function selection_select_column_to_bottom(canvas, cols, rows, caret)
  if not canvas or not caret or cols <= 0 then return false end
  rows = to_int(rows, 0)
  if rows <= 0 then rows = 1 end
  local x = to_int(caret.x, 0)
  x = clamp(x, 0, cols - 1)
  local y = to_int(caret.y, 0)
  y = clamp(y, 0, rows - 1)
  canvas:setSelection(x, y, x, rows - 1)
  return true
end

local function selection_shift_delete(ctx, canvas, cols, rows)
  if not canvas or not canvas:hasSelection() then return false end
  commit_if_moving(canvas)
  local x, y, w, h = canvas:getSelection()
  x = to_int(x, 0); y = to_int(y, 0); w = to_int(w, 0); h = to_int(h, 0)
  cols = to_int(cols, 0)
  rows = to_int(rows, 0)
  if w <= 0 or h <= 0 or cols <= 0 or rows <= 0 then return false end

  -- Eligibility:
  -- - full row: x==0, w==cols, h==1
  -- - full col: y==0, h==rows, w==1
  if x == 0 and w == cols and h == 1 then
    if ctx.out ~= nil then
      ctx.out[#ctx.out + 1] = { type = "canvas.remove_row_shift_up", y = y }
      return true
    end
    -- Old hosts (no tool commands): no-op (avoid reimplementing heavy shifts in Lua).
    return false
  end
  if y == 0 and h == rows and w == 1 then
    if ctx.out ~= nil then
      ctx.out[#ctx.out + 1] = { type = "canvas.remove_col_shift_left", x = x }
      return true
    end
    return false
  end

  -- Ineligible selection: no-op (host may later add status.message tool command for feedback).
  return false
end

local function selection_insert_row_shift_down(ctx, canvas, cols, rows, caret)
  if not canvas or not caret then return false end
  rows = to_int(rows, 0)
  cols = to_int(cols, 0)
  if rows <= 0 then rows = 1 end
  if cols <= 0 then return false end
  commit_if_moving(canvas)
  if not selection_select_row(canvas, cols, rows, caret) then return false end
  local y = to_int(caret.y, 0)
  y = clamp(y, 0, rows - 1)
  if ctx.out ~= nil then
    ctx.out[#ctx.out + 1] = { type = "canvas.insert_row_shift_down", y = y }
    return true
  end
  return false
end

local function selection_insert_col_shift_right(ctx, canvas, cols, rows, caret)
  if not canvas or not caret then return false end
  rows = to_int(rows, 0)
  cols = to_int(cols, 0)
  if rows <= 0 then rows = 1 end
  if cols <= 0 then return false end
  commit_if_moving(canvas)
  if not selection_select_column(canvas, cols, rows, caret) then return false end
  local x = to_int(caret.x, 0)
  x = clamp(x, 0, cols - 1)
  if ctx.out ~= nil then
    ctx.out[#ctx.out + 1] = { type = "canvas.insert_col_shift_right", x = x }
    return true
  end
  return false
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
    -- Normalize caret.
    caret.x = clamp(to_int(caret.x, 0), 0, cols - 1)
    caret.y = clamp(to_int(caret.y, 0), 0, rows - 1)

    -- If a move/copy operation is active, arrow keys nudge the floating selection instead of moving caret.
    -- (This is the core of the "selection verb language" workflow.)
    if canvas:isMovingSelection() then
      if move_nudge(canvas, cols, rows, caret, keys) then
        selecting = false
        selecting_mode = nil
        return
      end
    end

    -- Place/stamp (only when a floating selection is active).
    if actions["selection.op.place"] then
      if selection_place(canvas) then
        selecting = false
        selecting_mode = nil
        return
      end
    end
    if actions["selection.op.stamp"] then
      if selection_stamp(canvas) then
        selecting = false
        selecting_mode = nil
        return
      end
    end

    -- Start move/copy (create floating selection).
    if actions["selection.op.move"] and canvas:hasSelection() and (not canvas:isMovingSelection()) then
      if begin_move_selection(canvas, false) then
        selecting = false
        selecting_mode = nil
        return
      end
    end
    if actions["selection.op.copy"] and canvas:hasSelection() and (not canvas:isMovingSelection()) then
      if begin_move_selection(canvas, true) then
        selecting = false
        selecting_mode = nil
        return
      end
    end

    -- Fill/erase (in-place).
    if actions["selection.op.fill"] and canvas:hasSelection() then
      if selection_fill(ctx, canvas, layer) then
        selecting = false
        selecting_mode = nil
        return
      end
    end
    if actions["selection.op.erase"] and canvas:hasSelection() then
      if selection_erase(ctx, canvas, layer) then
        selecting = false
        selecting_mode = nil
        return
      end
    end

    -- Keyboard selection (rubber-band) using Enter:
    -- - first Enter: start selection at caret
    -- - move caret: live resize selection
    -- - second Enter: finish (keep selection)
    local enter_down = (keys.enter == true) or (actions["editor.new_line"] == true)
    if enter_down and not prev_enter_down then
      commit_if_moving(canvas)
      if not selecting then
        canvas:clearSelection()
        selecting = true
        selecting_mode = "keyboard"
        sel_x0 = to_int(caret.x, 0)
        sel_y0 = to_int(caret.y, 0)
        canvas:setSelection(sel_x0, sel_y0, sel_x0, sel_y0)
      else
        selecting = false
        selecting_mode = nil
      end
      prev_enter_down = enter_down
      return
    end
    prev_enter_down = enter_down

    -- Caret navigation (classic wrap rules; match Edit tool).
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
        if caret.y > rows - 1 then caret.y = rows - 1 end
        caret.x = 0
      end
      moved = true
    end
    if keys.up then
      if caret.y > 0 then caret.y = caret.y - 1 end
      moved = true
    end
    if keys.down then
      if caret.y < rows - 1 then caret.y = caret.y + 1 end
      moved = true
    end
    if keys.home then caret.x = 0; moved = true end
    if keys["end"] then caret.x = cols - 1; moved = true end

    -- Live resize keyboard selection while selecting.
    if moved and selecting then
      local x0 = clamp(to_int(sel_x0, 0), 0, cols - 1)
      local y0 = clamp(to_int(sel_y0, 0), 0, rows - 1)
      local x1 = clamp(to_int(caret.x, 0), 0, cols - 1)
      local y1 = clamp(to_int(caret.y, 0), 0, rows - 1)
      local x_min, x_max = reorientate(x0, x1)
      local y_min, y_max = reorientate(y0, y1)
      canvas:setSelection(x_min, y_min, x_max, y_max)
      return
    end

    -- UI-driven transforms (direct buttons).
    if canvas:hasSelection() then
      if p.rotateCW == true then
        selection_rotate_cw(ctx, canvas, layer, cols)
        selecting = false
        return
      elseif p.flipX == true then
        selection_flip_x(ctx, canvas, layer)
        selecting = false
        return
      elseif p.flipY == true then
        selection_flip_y(ctx, canvas, layer)
        selecting = false
        return
      elseif p.center == true then
        selection_center(ctx, canvas, layer, cols, rows)
        selecting = false
        return
      elseif p.crop == true then
        if ctx.out ~= nil then
          ctx.out[#ctx.out + 1] = { type = "canvas.crop_to_selection" }
        else
          selection_crop_contents(ctx, canvas, layer, cols, rows)
        end
        selecting = false
        return
      end
    end

    -- Cancel / clear.
    if hotkeys.cancel or actions["selection.clear_or_cancel"] then
      if canvas:isMovingSelection() then
        canvas:cancelMoveSelection()
      else
        canvas:clearSelection()
      end
      selecting = false
      selecting_mode = nil
      return
    end

    -- Select all.
    if (hotkeys.selectAll or actions["edit.select_all"]) and cols > 0 and rows > 0 then
      canvas:setSelection(0, 0, cols - 1, rows - 1)
      selecting = false
      selecting_mode = nil
      return
    end

    -- Select row/column (caret-based).
    if actions["selection.select_row"] then
      if selection_select_row(canvas, cols, rows, caret) then
        selecting = false
        return
      end
    end
    if actions["selection.select_column"] then
      if selection_select_column(canvas, cols, rows, caret) then
        selecting = false
        return
      end
    end

    -- Select row/column ranges (caret-based).
    if actions["selection.select_row_to_start"] then
      if selection_select_row_to_start(canvas, cols, rows, caret) then
        selecting = false
        return
      end
    end
    if actions["selection.select_row_to_end"] then
      if selection_select_row_to_end(canvas, cols, rows, caret) then
        selecting = false
        return
      end
    end
    if actions["selection.select_column_to_top"] then
      if selection_select_column_to_top(canvas, cols, rows, caret) then
        selecting = false
        return
      end
    end
    if actions["selection.select_column_to_bottom"] then
      if selection_select_column_to_bottom(canvas, cols, rows, caret) then
        selecting = false
        return
      end
    end

    -- Alt+Arrow delete row/col (caret-based): auto-select, then shift-delete, keep selection.
    if actions["selection.remove_row_shift_up"] then
      if selection_select_row(canvas, cols, rows, caret) and selection_shift_delete(ctx, canvas, cols, rows) then
        selecting = false
        return
      end
    end
    if actions["selection.remove_col_shift_left"] then
      if selection_select_column(canvas, cols, rows, caret) and selection_shift_delete(ctx, canvas, cols, rows) then
        selecting = false
        return
      end
    end

    -- Alt+Arrow insert row/col (caret-based): auto-select, insert, keep selection on inserted row/col.
    if actions["selection.insert_row_shift_down"] then
      if selection_insert_row_shift_down(ctx, canvas, cols, rows, caret) then
        selecting = false
        return
      end
    end
    if actions["selection.insert_col_shift_right"] then
      if selection_insert_col_shift_right(ctx, canvas, cols, rows, caret) then
        selecting = false
        return
      end
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
      selecting_mode = nil
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
      selecting_mode = nil
      return
    end

    -- Delete selection contents.
    -- - Backspace -> selection.clear (always clears in place)
    -- - Delete -> selection.delete_destructive (shift-delete only for full row/col selections)
    if actions["selection.delete_destructive"] and canvas:hasSelection() then
      if selection_shift_delete(ctx, canvas, cols, rows) then
        selecting = false
        selecting_mode = nil
        return
      end
      canvas:deleteSelection()
      selecting = false
      selecting_mode = nil
      return
    end
    if (hotkeys.deleteSelection or actions["selection.clear"]) and canvas:hasSelection() then
      canvas:deleteSelection()
      selecting = false
      selecting_mode = nil
      return
    end

    -- Shift-delete selection (row/column only).
    if actions["selection.shift_delete"] then
      if selection_shift_delete(ctx, canvas, cols, rows) then
        selecting = false
        selecting_mode = nil
        return
      end
    end

    -- Selection transforms (key-bindings/actions; tool-gated here in Select tool).
    if actions["selection.op.rotate_cw"] and canvas:hasSelection() then
      if selection_rotate_cw(ctx, canvas, layer, cols) then
        selecting = false
        selecting_mode = nil
        return
      end
    end
    if actions["selection.op.flip_x"] and canvas:hasSelection() then
      if selection_flip_x(ctx, canvas, layer) then
        selecting = false
        selecting_mode = nil
        return
      end
    end
    if actions["selection.op.flip_y"] and canvas:hasSelection() then
      if selection_flip_y(ctx, canvas, layer) then
        selecting = false
        selecting_mode = nil
        return
      end
    end
    if actions["selection.op.center"] and canvas:hasSelection() then
      if selection_center(ctx, canvas, layer, cols, rows) then
        selecting = false
        selecting_mode = nil
        return
      end
    end
    if actions["selection.crop"] and canvas:hasSelection() then
      -- Prefer true crop (resize) via host command. Old hosts will ignore unknown commands.
      if ctx.out ~= nil then
        ctx.out[#ctx.out + 1] = { type = "canvas.crop_to_selection" }
        selecting = false
        selecting_mode = nil
        return
      end
      -- Fallback: "crop contents" (does not resize geometry).
      if selection_crop_contents(ctx, canvas, layer, cols, rows) then
        selecting = false
        selecting_mode = nil
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

  -- Track whether the hover cell actually changed.
  local hover_moved = (last_hover_x == nil) or (last_hover_y == nil) or (x ~= last_hover_x) or (y ~= last_hover_y)
  last_hover_x = x
  last_hover_y = y

  local prev = cursor.p or {}
  local left = (cursor.left == true)
  local right = (cursor.right == true)
  local prev_left = (prev.left == true)
  local prev_right = (prev.right == true)

  local press_left = left and not prev_left
  local release_left = (not left) and prev_left
  local press_right = right and not prev_right

  -- Keyboard rubber-band selection mode (started via Enter):
  -- - ignore stationary mouse hover so it doesn't fight keyboard caret movement
  -- - if the mouse *moves to a new cell*, allow it to drive live resize without requiring a click
  if selecting and selecting_mode == "keyboard" and (not left) and (not right) then
    if hover_moved then
      -- Sync caret to mouse and update selection bounds.
      if type(caret) == "table" then
        caret.x = clamp(x, 0, cols - 1)
        caret.y = clamp(y, 0, rows - 1)
      end
      local x0 = clamp(to_int(sel_x0, 0), 0, cols - 1)
      local y0 = clamp(to_int(sel_y0, 0), 0, rows - 1)
      local x1 = clamp(x, 0, cols - 1)
      local y1 = clamp(y, 0, rows - 1)
      local x_min, x_max = reorientate(x0, x1)
      local y_min, y_max = reorientate(y0, y1)
      canvas:setSelection(x_min, y_min, x_max, y_max)
    end
    return
  end

  -- Keep tool caret in sync with mouse-driven target so keyboard navigation continues
  -- from the last mouse interaction.
  if type(caret) == "table" then
    caret.x = clamp(x, 0, cols - 1)
    caret.y = clamp(y, 0, rows - 1)
  end

  -- Right-click: clear selection (if not actively moving).
  if press_right and not canvas:isMovingSelection() then
    canvas:clearSelection()
    selecting = false
    selecting_mode = nil
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
      selecting_mode = nil
    else
      selecting = true
      selecting_mode = "mouse"
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
    selecting_mode = nil
  end
end


