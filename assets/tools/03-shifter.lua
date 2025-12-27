settings = {
    id = "03-shifter",
    icon = "⇌",
    label = "Shifter",
    shortcut = "Alt+S"
}

-- Shifter tool (inspired by Moebius / IcyDraw):
-- Click to cycle between block glyph variants in-place:
-- - empty -> left/right half block
-- - half blocks -> empty or full block
-- - blocky glyphs (█ ▀ ▄) -> left/right half block
-- Shift+Click clears the cell.
--
-- This is mainly for quickly refining block art (especially left/right halves),
-- complementing Pencil's vertical half-block mode (▀/▄).

local function clamp(v, a, b)
  if v < a then return a end
  if v > b then return b end
  return v
end

local function clear_cell(layer, x, y)
  layer:set(x, y, " ")
  layer:clearStyle(x, y)
end

local function set_cell(layer, x, y, ch, fg, bg)
  if fg == nil and bg == nil then
    layer:set(x, y, ch)
    layer:clearStyle(x, y)
  else
    layer:set(x, y, ch, fg, bg)
  end
end

local function is_blank(ch)
  return ch == nil or ch == "" or ch == " " or ch == "\0" or ch == " "
end

local function shift_cell(ctx, layer, x, y, secondary, clear)
  if not ctx or not layer then return end
  local cols = tonumber(ctx.cols) or 0
  if cols <= 0 then return end
  if x < 0 or x >= cols then return end
  if y < 0 then return end

  if clear then
    clear_cell(layer, x, y)
    return
  end

  local ch, cur_fg, cur_bg = layer:get(x, y)
  if type(ch) ~= "string" or #ch == 0 then ch = " " end

  local fg = (type(cur_fg) == "number") and math.floor(cur_fg) or nil
  local bg = (type(cur_bg) == "number") and math.floor(cur_bg) or nil

  -- Use current palette fg/bg as a fallback for cells with unset style.
  local pfg = (type(ctx.fg) == "number") and math.floor(ctx.fg) or nil
  local pbg = (type(ctx.bg) == "number") and math.floor(ctx.bg) or nil

  if fg == nil then fg = pfg end
  if bg == nil then bg = pbg end

  -- Unicode block characters (editor is UTF-8):
  local FULL  = "█"
  local UHALF = "▀"
  local LHALF = "▄"
  local LVERT = "▌" -- left half block
  local RVERT = "▐" -- right half block

  -- Moebius-style cycling:
  -- - blank -> (primary) RVERT ; (secondary) LVERT
  -- - FULL/UHALF/LHALF -> (primary) LVERT ; (secondary) RVERT
  -- - LVERT -> (primary) blank ; (secondary) FULL
  -- - RVERT -> (primary) FULL ; (secondary) blank
  if is_blank(ch) then
    set_cell(layer, x, y, secondary and LVERT or RVERT, fg, bg)
    return
  end

  if ch == FULL or ch == UHALF or ch == LHALF then
    set_cell(layer, x, y, secondary and RVERT or LVERT, fg, bg)
    return
  end

  if ch == LVERT then
    if secondary then
      set_cell(layer, x, y, FULL, fg, bg)
    else
      clear_cell(layer, x, y)
    end
    return
  end

  if ch == RVERT then
    if secondary then
      clear_cell(layer, x, y)
    else
      set_cell(layer, x, y, FULL, fg, bg)
    end
    return
  end

  -- Moebius behavior: if it's not one of the recognized block glyphs, do nothing.
  -- (Our Pencil/Brush tools are the right place to overwrite arbitrary glyphs.)
  return
end

function render(ctx, layer)
  if not ctx or not layer then return end
  if not ctx.focused then return end

  local cols = tonumber(ctx.cols) or 0
  if cols <= 0 then return end

  local cursor = ctx.cursor or {}
  if cursor.valid ~= true then return end

  local x = clamp(tonumber(cursor.x) or 0, 0, cols - 1)
  local y = math.max(0, tonumber(cursor.y) or 0)

  local keys = ctx.keys or {}
  local mods = ctx.mods or {}

  local phase = tonumber(ctx.phase) or 0

  -- Mouse-driven editing (hold+drag across cells).
  if phase == 1 then
    local left = (cursor.left == true)
    local right = (cursor.right == true)
    if not left and not right then return end

    local px = tonumber(cursor.p and cursor.p.x)
    local py = tonumber(cursor.p and cursor.p.y)
    local prev_left = (cursor.p and cursor.p.left) == true
    local prev_right = (cursor.p and cursor.p.right) == true

    local moved_cell = (px ~= nil and py ~= nil) and ((x ~= px) or (y ~= py)) or true
    local pressed_edge = (left and not prev_left) or (right and not prev_right)
    if not moved_cell and not pressed_edge then return end

    local secondary = right
    local clear = (mods.shift == true) or (keys.delete == true)
    shift_cell(ctx, layer, x, y, secondary, clear)
    return
  end

  -- Keyboard support: Delete clears current cell.
  if keys.delete == true then
    shift_cell(ctx, layer, x, y, false, true)
  end
end

