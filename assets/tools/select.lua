settings = {
  icon = "▭",
  label = "Select",

  -- Tool parameters (host renders UI; values are available under ctx.params.*)
  params = {
    copyMode = { type = "enum", label = "Copy Mode", items = { "layer", "composite" }, default = "layer" },
    pasteMode = { type = "enum", label = "Paste Mode", items = { "both", "char", "color" }, default = "both" },
    transparentSpaces = { type = "bool", label = "Paste: Transparent spaces", default = false },
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

function render(ctx, layer)
  if not ctx or not layer then return end
  if not ctx.focused then return end

  local canvas = ctx.canvas
  if canvas == nil then return end

  local phase = to_int(ctx.phase, 0)
  local cols = to_int(ctx.cols, 0)
  local rows = to_int(ctx.rows, 0)
  local caret = ctx.caret or {}
  local keys = ctx.keys or {}
  local mods = ctx.mods or {}
  local hotkeys = ctx.hotkeys or {}
  local actions = ctx.actions or {}
  local p = ctx.params or {}

  -- Phase 0: keyboard shortcuts.
  if phase == 0 then
    -- Cancel / clear.
    if hotkeys.cancel or keys.escape then
      if canvas:isMovingSelection() then
        canvas:cancelMoveSelection()
      else
        canvas:clearSelection()
      end
      selecting = false
      return
    end

    -- Select all.
    if (hotkeys.selectAll or (mods.ctrl and keys.a)) and cols > 0 and rows > 0 then
      canvas:setSelection(0, 0, cols - 1, rows - 1)
      selecting = false
      return
    end

    -- Clipboard operations.
    if hotkeys.copy or (mods.ctrl and keys.c) then
      local mode = p.copyMode
      if type(mode) ~= "string" then mode = "layer" end
      canvas:copySelection(mode)
      return
    end
    if hotkeys.cut or (mods.ctrl and keys.x) then
      -- Cut is always per-layer (destructive). Copy mode doesn't apply.
      canvas:cutSelection()
      selecting = false
      return
    end
    if hotkeys.paste or (mods.ctrl and keys.v) then
      local x = to_int(caret.x, 0)
      local y = to_int(caret.y, 0)
      local mode = p.pasteMode
      if type(mode) ~= "string" then mode = "both" end
      local transparent = (p.transparentSpaces == true)
      canvas:pasteClipboard(x, y, nil, mode, transparent)
      selecting = false
      return
    end

    -- Delete selection contents.
    if (hotkeys.deleteSelection or actions["selection.delete"] or keys["delete"]) and canvas:hasSelection() then
      canvas:deleteSelection()
      selecting = false
      return
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


