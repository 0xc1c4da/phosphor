settings = {
  id = "02-brush",
  icon = "🖍",
  label = "Brush",

  -- Tool parameters (host renders UI; values are available under ctx.params.*)
  params = {
    mode = { type = "enum", label = "Mode", ui = "segmented", section = "Stamp", primary = true, order = 0, items = { "both", "char", "color" }, default = "both" },
    anchor = { type = "enum", label = "Anchor", ui = "segmented", section = "Stamp", primary = true, order = 1, inline = true, items = { "center", "top-left" }, default = "center" },
    transparent = { type = "bool", label = "Transparent", ui = "toggle", section = "Stamp", primary = true, order = 2, inline = true, default = true },

    -- Compact "Apply BG brush/current" style rows:
    useBg = { type = "bool", label = "BG", ui = "toggle", section = "Color", primary = true, order = 10, default = true },
    bgSource = { type = "enum", label = "Source", ui = "segmented", section = "Color", primary = true, order = 11, inline = true, enabled_if = "useBg", items = { "brush", "current" }, default = "brush" },

    useFg = { type = "bool", label = "FG", ui = "toggle", section = "Color", primary = true, order = 12, default = true },
    fgSource = { type = "enum", label = "Source", ui = "segmented", section = "Color", primary = true, order = 13, inline = true, enabled_if = "useFg", items = { "brush", "current" }, default = "brush" },
  },
}

local function clamp(v, a, b)
  if v < a then return a end
  if v > b then return b end
  return v
end

local function current_glyph(ctx)
  local g = ctx and ctx.glyph or nil
  if type(g) ~= "string" or #g == 0 then
    return " "
  end
  return g
end

local function brush_is_valid(brush)
  if type(brush) ~= "table" then return false end
  local w = tonumber(brush.w) or 0
  local h = tonumber(brush.h) or 0
  if w <= 0 or h <= 0 then return false end
  if type(brush.cells) ~= "table" then return false end
  local n = w * h
  -- We don't require exact array length, but must have at least one cell.
  return n > 0
end

local function should_skip_cell(cell, transparent_spaces)
  if not transparent_spaces then return false end
  if type(cell) ~= "table" then return true end
  local ch = cell.ch
  if type(ch) ~= "string" or #ch == 0 then ch = " " end
  if ch ~= " " then return false end
  -- If it's a space, still apply if it carries bg/fg/attrs (e.g. colored blocks).
  local fg = cell.fg
  local bg = cell.bg
  local attrs = cell.attrs
  local has_style = (type(fg) == "number") or (type(bg) == "number") or (type(attrs) == "number" and attrs ~= 0)
  return not has_style
end

local function apply_cell(layer, x, y, glyph_or_ch, fg, bg, attrs, mode)
  if mode == "char" then
    -- Glyph only: preserves existing style in the host.
    layer:set(x, y, glyph_or_ch)
    return
  end

  if mode == "color" then
    -- Color only: preserve glyph; write fg/bg/attrs by reusing the existing glyph.
    local cur_ch, _, _, _, _, cur_glyph = layer:get(x, y)
    if type(cur_ch) ~= "string" or #cur_ch == 0 then cur_ch = " " end
    local keep = cur_ch
    if type(cur_glyph) == "number" then
      keep = math.floor(cur_glyph)
    end
    if type(attrs) == "number" then
      layer:set(x, y, keep, fg, bg, attrs)
    else
      layer:set(x, y, keep, fg, bg)
    end
    return
  end

  -- mode == "both": write glyph + style.
  if type(attrs) == "number" then
    layer:set(x, y, glyph_or_ch, fg, bg, attrs)
  elseif type(fg) == "number" or type(bg) == "number" then
    layer:set(x, y, glyph_or_ch, fg, bg)
  else
    layer:set(x, y, glyph_or_ch)
  end
end

local function pick_color(ctx, cell, which, source, apply)
  if not apply then
    return nil -- preserve
  end
  -- NOTE: avoid Lua's `(cond and a or b)` ternary idiom here: if `a` is nil it will
  -- fall through to `b` and can silently "swap" channels when one is unset.
  local cur = nil
  if which == "fg" then
    cur = ctx.fg
  else
    cur = ctx.bg
  end
  if type(cur) ~= "number" then cur = nil end
  local cel = nil
  if type(cell) == "table" and type(cell[which]) == "number" then
    cel = math.floor(cell[which])
  end
  if source == "current" then
    return cur
  end
  return cel
end

local function stamp(ctx, layer, cx, cy)
  local cols = tonumber(ctx.cols) or 0
  if cols <= 0 then return end
  if cx < 0 or cx >= cols then return end
  if cy < 0 then return end

  local p = ctx.params or {}
  local mode = p.mode
  if type(mode) ~= "string" then mode = "both" end
  local anchor = p.anchor
  if type(anchor) ~= "string" then anchor = "center" end
  local transparent = (p.transparent ~= false)
  local fgSource = p.fgSource
  if type(fgSource) ~= "string" then fgSource = "brush" end
  local bgSource = p.bgSource
  if type(bgSource) ~= "string" then bgSource = "brush" end
  local useFg = (p.useFg ~= false)
  local useBg = (p.useBg ~= false)

  local brush = ctx.brush
  if brush_is_valid(brush) then
    local w = tonumber(brush.w) or 0
    local h = tonumber(brush.h) or 0
    if w <= 0 or h <= 0 then return end

    local ox, oy = cx, cy
    if anchor == "center" then
      ox = cx - math.floor(w / 2)
      oy = cy - math.floor(h / 2)
    end

    local cells = brush.cells
    local i = 1
    for y = 0, h - 1 do
      for x = 0, w - 1 do
        local cell = cells[i]
        i = i + 1

        if not should_skip_cell(cell, transparent) then
          local px = ox + x
          local py = oy + y
          if px >= 0 and px < cols and py >= 0 then
            local glyph = (type(cell) == "table") and cell.glyph or nil
            local ch = (type(cell) == "table") and cell.ch or nil
            if type(ch) ~= "string" or #ch == 0 then ch = " " end
            -- Colors are indices in the active canvas palette (or nil).
            -- nil means "preserve" (host-side).
            local fg = pick_color(ctx, cell, "fg", fgSource, useFg)
            local bg = pick_color(ctx, cell, "bg", bgSource, useBg)
            local attrs = (type(cell) == "table" and type(cell.attrs) == "number") and math.floor(cell.attrs) or nil
            if attrs ~= nil and attrs < 0 then attrs = 0 end
            local arg = ch
            if type(glyph) == "number" then
              arg = math.floor(glyph)
            end
            apply_cell(layer, px, py, arg, fg, bg, attrs, mode)
          end
        end
      end
    end
    return
  end

  -- No multi-cell brush selected: do nothing.
  -- This avoids surprising "single glyph" stamping behavior (and tofu/fallback glyphs)
  -- when the user expected the Brush tool to require a captured brush.
  return
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

  -- Brush preview (host overlay; transient).
  -- Show the exact stamp footprint for the current brush (from brush palette / selection capture).
  do
    local p = ctx.params or {}
    local anchor = p.anchor
    if type(anchor) ~= "string" then anchor = "center" end

    local cursor = ctx.cursor or {}
    local use_cursor = (type(cursor) == "table" and cursor.valid == true)
    local cx = use_cursor and tonumber(cursor.x) or tonumber(caret.x)
    local cy = use_cursor and tonumber(cursor.y) or tonumber(caret.y)
    cx = tonumber(cx) or caret.x
    cy = tonumber(cy) or caret.y

    local brush = ctx.brush
    local w = 1
    local h = 1
    if brush_is_valid(brush) then
      w = tonumber(brush.w) or 1
      h = tonumber(brush.h) or 1
      if w < 1 then w = 1 end
      if h < 1 then h = 1 end
    end

    local x0 = cx
    local y0 = cy
    if anchor == "center" then
      x0 = cx - math.floor(w / 2)
      y0 = cy - math.floor(h / 2)
    end
    local x1 = x0 + w - 1
    local y1 = y0 + h - 1

    if ctx.out ~= nil then
      ctx.out[#ctx.out + 1] = { type = "brush.preview", x0 = x0, y0 = y0, x1 = x1, y1 = y1 }
    end
  end

  -- Phase 1: mouse stamping (click+drag).
  if phase == 1 then
    local cursor = ctx.cursor
    if type(cursor) == "table" and cursor.valid and (cursor.left or cursor.right) then
      local px = tonumber(cursor.p and cursor.p.x)
      local py = tonumber(cursor.p and cursor.p.y)
      local prev_left = (cursor.p and cursor.p.left) == true
      local prev_right = (cursor.p and cursor.p.right) == true
      local x1 = tonumber(cursor.x) or caret.x
      local y1 = tonumber(cursor.y) or caret.y

      local moved = (px ~= nil and py ~= nil) and ((x1 ~= px) or (y1 ~= py)) or true
      local pressed_edge = (cursor.left and not prev_left) or (cursor.right and not prev_right)
      if moved or pressed_edge then
        caret.x = clamp(x1, 0, cols - 1)
        caret.y = math.max(0, y1)
        stamp(ctx, layer, caret.x, caret.y)
      end
    end
    return
  end

  -- Phase 0: keyboard navigation + stamp on Enter.
  local keys = ctx.keys or {}
  local moved = false
  if keys.left then
    if caret.x > 0 then caret.x = caret.x - 1 end
    moved = true
  end
  if keys.right then
    if caret.x < cols - 1 then caret.x = caret.x + 1 end
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
  if keys.home then
    caret.x = 0
    moved = true
  end
  if keys["end"] then
    caret.x = cols - 1
    moved = true
  end

  -- Stamp on Enter (or keypad Enter).
  if keys.enter then
    stamp(ctx, layer, caret.x, caret.y)
  end
end