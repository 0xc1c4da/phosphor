settings = {
  id = "10-format",
  icon = "¶",
  label = "Format",

  -- Tool parameters (host renders UI; values are available under ctx.params.*)
  params = {
    size = { type = "int", label = "Size", ui = "slider", section = "Brush", primary = true, order = 0, min = 1, max = 20, step = 1, default = 1, width = 180 },
    mode = {
      type = "enum",
      label = "Op",
      ui = "segmented",
      section = "Format",
      primary = true,
      order = 1,
      items = { "set", "clear", "toggle", "replace" },
      default = "set",
    },
    applyTo = {
      type = "enum",
      label = "Apply To",
      ui = "segmented",
      section = "Format",
      primary = true,
      order = 2,
      items = { "paint", "selection" },
      default = "paint",
      inline = true,
    },
    clipToSelection = { type = "bool", label = "Clip to selection", ui = "toggle", section = "Brush", primary = true, order = 3, default = false, inline = true },

    -- Break attributes across two rows to avoid insane width.
    bold = { type = "bool", label = "Bold", ui = "toggle", section = "Attributes", primary = true, order = 10, default = false },
    dim = { type = "bool", label = "Dim", ui = "toggle", section = "Attributes", primary = true, order = 11, default = false, inline = true },
    italic = { type = "bool", label = "Italic", ui = "toggle", section = "Attributes", primary = true, order = 12, default = false, inline = true },
    underline = { type = "bool", label = "Underline", ui = "toggle", section = "Attributes", primary = true, order = 13, default = false, inline = true },

    blink = { type = "bool", label = "Blink", ui = "toggle", section = "Attributes", primary = true, order = 20, default = false },
    reverse = { type = "bool", label = "Reverse", ui = "toggle", section = "Attributes", primary = true, order = 21, default = false, inline = true },
    strike = { type = "bool", label = "Strike", ui = "toggle", section = "Attributes", primary = true, order = 22, default = false, inline = true },

    apply = { type = "button", label = "Apply", ui = "action", section = "Actions", primary = true, order = 99 },
  },
}

-- Attr bit mapping matches AnsiCanvas::Attr_* in src/core/canvas.h.
local ATTR_BOLD = 1
local ATTR_DIM = 2
local ATTR_ITALIC = 4
local ATTR_UNDERLINE = 8
local ATTR_BLINK = 16
local ATTR_REVERSE = 32
local ATTR_STRIKE = 64

-- LuaJIT uses the `bit` library for bitwise ops (Lua 5.1 compatible).
local bitlib = bit
if bitlib == nil then
  -- Some hosts may not predefine global `bit`.
  bitlib = require("bit")
end

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

local function to_int(v, def)
  v = tonumber(v)
  if v == nil then return def end
  return math.floor(v)
end

local function mask_from_params(p)
  local m = 0
  if p.bold then m = m + ATTR_BOLD end
  if p.dim then m = m + ATTR_DIM end
  if p.italic then m = m + ATTR_ITALIC end
  if p.underline then m = m + ATTR_UNDERLINE end
  if p.blink then m = m + ATTR_BLINK end
  if p.reverse then m = m + ATTR_REVERSE end
  if p.strike then m = m + ATTR_STRIKE end
  return m
end

local function apply_op(old_attrs, mask, op)
  old_attrs = to_int(old_attrs, 0)
  mask = to_int(mask, 0)
  if mask < 0 then mask = 0 end

  if op == "replace" then
    return mask
  elseif op == "toggle" then
    return bitlib.bxor(old_attrs, mask)
  elseif op == "clear" then
    return bitlib.band(old_attrs, bitlib.bnot(mask))
  else
    -- default: set
    return bitlib.bor(old_attrs, mask)
  end
end

local function in_selection(canvas, x, y)
  if not canvas or not canvas:hasSelection() then return true end
  return canvas:selectionContains(x, y)
end

local function mutate_cell(layer, x, y, op, mask, clip_ok)
  if not clip_ok then return end

  -- layer:get returns: ch, fg?, bg?, cp, attrs
  local _, fg, bg, cp, attrs = layer:get(x, y)
  local new_attrs = apply_op(attrs, mask, op)
  if to_int(new_attrs, 0) == to_int(attrs, 0) then
    return
  end
  layer:set(x, y, cp, fg, bg, new_attrs)
end

local function paint(ctx, canvas, layer, x, y, half_y_override)
  local p = ctx.params or {}
  local cols = to_int(ctx.cols, 0)
  if cols <= 0 then return end
  if x < 0 or x >= cols then return end
  if y < 0 then return end

  local size = to_int(p.size, 1)
  if size < 1 then size = 1 end
  if size > 100 then size = 100 end
  local r = math.floor(size / 2)

  local mask = mask_from_params(p)
  local op = p.mode
  if type(op) ~= "string" then op = "set" end

  local cursor = ctx.cursor or {}
  local secondary = cursor.right == true
  if secondary then
    -- Right click acts as a "format eraser": clear selected bits, or clear all if mask is empty.
    op = "clear"
    if mask == 0 then mask = 65535 end
  end

  for dy = -r, r do
    for dx = -r, r do
      local px = x + dx
      local py = y + dy
      if px >= 0 and px < cols and py >= 0 then
        local ok = true
        if p.clipToSelection == true and canvas and canvas:hasSelection() then
          ok = in_selection(canvas, px, py)
        end
        mutate_cell(layer, px, py, op, mask, ok)
      end
    end
  end
end

function render(ctx, layer)
  if not ctx or not layer then return end
  if not ctx.focused then return end

  local canvas = ctx.canvas
  local p = ctx.params or {}
  local cols = to_int(ctx.cols, 0)
  if cols <= 0 then return end

  -- Caret is tool-owned: if we want arrows/mouse to move it, we must implement it here.
  local caret = ctx.caret
  if type(caret) ~= "table" then return end
  caret.x = clamp(to_int(caret.x, 0), 0, cols - 1)
  caret.y = math.max(0, to_int(caret.y, 0))

  -- Brush size preview (host overlay; transient).
  do
    local size = to_int(p.size, 1)
    if size < 1 then size = 1 end
    if size > 100 then size = 100 end
    local r = math.floor(size / 2)
    local cursor = ctx.cursor or {}
    local anchor = (type(cursor) == "table" and cursor.valid == true) and "cursor" or "caret"
    if ctx.out ~= nil then
      ctx.out[#ctx.out + 1] = { type = "brush.preview", anchor = anchor, rx = r, ry = r }
    end
  end

  -- Phase 0: update host "current attrs" selection and handle selection-apply button.
  if to_int(ctx.phase, 0) == 0 then
    local mask = mask_from_params(p)
    if ctx.out ~= nil then
      ctx.out[#ctx.out + 1] = { type = "attrs.set", mask = mask }
    end

    if p.apply == true and p.applyTo == "selection" and canvas and canvas:hasSelection() then
      local x, y, w, h = canvas:getSelection()
      x = to_int(x, 0); y = to_int(y, 0); w = to_int(w, 0); h = to_int(h, 0)
      if w > 0 and h > 0 then
        local op = p.mode
        if type(op) ~= "string" then op = "set" end
        for j = 0, h - 1 do
          for i = 0, w - 1 do
            mutate_cell(layer, x + i, y + j, op, mask, true)
          end
        end
      end
    end

    -- Keyboard navigation (arrows/home/end). Keys are discrete presses.
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
      if caret.y > 0 then caret.y = caret.y - 1 end
      moved = true
    end
    if keys.down then
      caret.y = caret.y + 1
      moved = true
    end
    if keys.home then caret.x = 0; moved = true end
    if keys["end"] then caret.x = cols - 1; moved = true end

    -- Apply-at-caret on Enter (one-shot, predictable for toggle).
    if keys.enter and p.applyTo == "paint" then
      local mask2 = mask_from_params(p)
      local op2 = p.mode
      if type(op2) ~= "string" then op2 = "set" end
      local ok = true
      if p.clipToSelection == true and canvas and canvas:hasSelection() then
        ok = in_selection(canvas, caret.x, caret.y)
      end
      mutate_cell(layer, caret.x, caret.y, op2, mask2, ok)
      -- mimic editor-ish advance
      caret.x = caret.x + 1
      if caret.x >= cols then
        caret.x = 0
        caret.y = caret.y + 1
      end
    end

    return
  end

  -- Phase 1: paint on click/drag.
  if p.applyTo ~= "paint" then return end
  local cursor = ctx.cursor
  if type(cursor) ~= "table" or not cursor.valid then return end
  if not (cursor.left or cursor.right) then return end

  local x = to_int(cursor.x, 0)
  local y = to_int(cursor.y, 0)
  -- Only paint on press-edge or when we enter a new cell. This avoids:
  -- - toggle flipping back and forth while holding still
  -- - set/clear spam on idle frames
  local prev = cursor.p or {}
  local prev_x = tonumber(prev.x)
  local prev_y = tonumber(prev.y)
  local prev_left = (prev.left == true)
  local prev_right = (prev.right == true)
  local pressed_edge = (cursor.left and not prev_left) or (cursor.right and not prev_right)
  local moved_cell = (prev_x ~= nil and prev_y ~= nil) and ((x ~= prev_x) or (y ~= prev_y)) or true

  if pressed_edge or moved_cell then
    caret.x = clamp(x, 0, cols - 1)
    caret.y = math.max(0, y)
    paint(ctx, canvas, layer, caret.x, caret.y)
  end
end