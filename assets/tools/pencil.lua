settings = {
  id = "pencil",
  icon = "🖉",
  label = "Pencil",

  -- Tool parameters (host renders UI; values are available under ctx.params.*)
  params = {
    size = { type = "int", label = "Size", min = 1, max = 20, step = 1, default = 1 },
    mode = { type = "enum", label = "Mode", items = { "char", "colorize", "half", "block", "shade" }, default = "char" },
    useFg = { type = "bool", label = "Use FG", default = true },
    useBg = { type = "bool", label = "Use BG", default = true },
  },
}

local function clamp(v, a, b)
  if v < a then return a end
  if v > b then return b end
  return v
end

local function paint(ctx, layer, x, y, half_y_override)
  if not ctx or not layer then return end
  local p = ctx.params or {}

  local cols = tonumber(ctx.cols) or 0
  if cols <= 0 then return end
  if x < 0 or x >= cols then return end
  if y < 0 then return end

  local size = tonumber(p.size) or 1
  if size < 1 then size = 1 end
  if size > 100 then size = 100 end
  local r = math.floor(size / 2)

  local mode = p.mode
  if type(mode) ~= "string" then mode = "char" end

  local useFg = (p.useFg ~= false)
  local useBg = (p.useBg ~= false)

  local fg = ctx.fg
  if not useFg or type(fg) ~= "number" then fg = nil end
  local bg = ctx.bg
  if not useBg or type(bg) ~= "number" then bg = nil end

  local cursor = ctx.cursor or {}
  local secondary = cursor.right == true
  local cursor_half_y = nil
  if type(cursor) == "table" and type(cursor.half_y) == "number" then
    cursor_half_y = cursor.half_y
  end
  if type(half_y_override) == "number" then
    cursor_half_y = half_y_override
  end

  -- Right-click: swap fg/bg for most modes (icy-draw style).
  -- NOTE: In "half" mode, right-click selects the *other half*, so swapping colors here
  -- would be a double meaning and breaks half-block painting semantics.
  if secondary and mode ~= "shade" and mode ~= "half" and fg ~= nil and bg ~= nil then
    fg, bg = bg, fg
  end

  -- Shade stepping (match IcyDraw brush semantics):
  -- - ramp is: ░ -> ▒ -> ▓ -> █
  -- - primary click "tones up"
  -- - right click "tones down" (falls back to space below ░)
  local function shade_step(ch, down)
    local ramp = { "░", "▒", "▓", "█" }

    if down then
      if ch == ramp[1] then
        return " "
      end
      for i = #ramp, 2, -1 do
        if ch == ramp[i] then
          return ramp[i - 1]
        end
      end
      return " "
    end

    -- tone up
    if ch == ramp[#ramp] then
      return ramp[#ramp]
    end
    for i = 1, #ramp - 1 do
      if ch == ramp[i] then
        return ramp[i + 1]
      end
    end
    return ramp[1]
  end

  local function paint_cell(px, py)
    if px < 0 or px >= cols then return end
    if py < 0 then return end

    local ch
    if mode == "shade" then
      local cur = layer:get(px, py)
      if type(cur) ~= "string" or #cur == 0 then cur = " " end
      ch = shade_step(cur, secondary)
    elseif mode == "block" then
      ch = "█"
    elseif mode == "half" then
      -- Half-block painting is not just "write ▀/▄":
      -- it needs to preserve the *other half* by encoding it in bg/fg depending on the glyph.
      -- This matches the approach used by Moebius (doc.set_half_block) and other ANSI editors.

      -- Choose paint color:
      -- - left button paints using current FG (if enabled)
      -- - right button paints using current BG (if enabled)
      -- with fallbacks if only one of them is enabled.
      local primary_col = useFg and fg or (useBg and bg or nil)
      local secondary_col = useBg and bg or (useFg and fg or nil)
      local col = secondary and secondary_col or primary_col
      if type(col) ~= "number" then
        return
      end

      -- Read current cell and its colors (nil means "unset" in the layer).
      local cur_ch, cur_fg, cur_bg = layer:get(px, py)
      if type(cur_ch) ~= "string" or #cur_ch == 0 then cur_ch = " " end
      if type(cur_fg) ~= "number" then cur_fg = nil end
      if type(cur_bg) ~= "number" then cur_bg = nil end

      -- Fallback "background" color to use when the existing cell has no usable bg/fg.
      local fallback_bg = (type(bg) == "number" and bg) or (type(fg) == "number" and fg) or 0

      -- Decode current cell into (upper_color, lower_color) if it's "blocky".
      local is_blocky = false
      local upper = nil
      local lower = nil
      if cur_ch == "▄" then
        -- lower uses fg; upper uses bg
        upper = cur_bg
        lower = cur_fg
        is_blocky = true
      elseif cur_ch == "▀" then
        -- upper uses fg; lower uses bg
        upper = cur_fg
        lower = cur_bg
        is_blocky = true
      elseif cur_ch == "█" then
        upper = cur_fg
        lower = cur_fg
        is_blocky = true
      elseif cur_ch == " " then
        if cur_bg ~= nil then
          upper = cur_bg
          lower = cur_bg
          is_blocky = true
        end
      end

      -- If the cell isn't explicitly blocky, we still treat it as blocky if its fg/bg match.
      if not is_blocky and cur_fg ~= nil and cur_bg ~= nil and cur_fg == cur_bg then
        upper = cur_fg
        lower = cur_fg
        is_blocky = true
      end

      -- Ensure we have something reasonable for the other-half color.
      if upper == nil then upper = (cur_fg ~= nil and cur_fg) or (cur_bg ~= nil and cur_bg) or fallback_bg end
      if lower == nil then lower = (cur_bg ~= nil and cur_bg) or (cur_fg ~= nil and cur_fg) or fallback_bg end

      -- Pick which half to paint from mouse position inside the cell.
      -- If host does not provide cursor.half_y, fall back to legacy behavior.
      local paint_top = (cursor_half_y ~= nil) and ((cursor_half_y % 2) == 0) or true
      if is_blocky then
        -- If the other half already matches the paint color, collapse to a full block.
        if (paint_top and lower == col) or ((not paint_top) and upper == col) then
          layer:set(px, py, "█", col, 0)
          return
        end
        if paint_top then
          layer:set(px, py, "▀", col, lower)
        else
          layer:set(px, py, "▄", col, upper)
        end
      else
        -- Non-blocky cell: preserve the existing background (or fallback) for the other half.
        local base_bg = (cur_bg ~= nil) and cur_bg or fallback_bg
        if paint_top then
          layer:set(px, py, "▀", col, base_bg)
        else
          layer:set(px, py, "▄", col, base_bg)
        end
      end
      return
    elseif mode == "colorize" then
      -- Preserve glyph, only modify fg/bg.
      if fg == nil and bg == nil then
        return -- truly "colorize only": nothing to do if no colors are enabled
      end
      ch = layer:get(px, py)
      if type(ch) ~= "string" or #ch == 0 then ch = " " end
    else
      ch = ctx.brush
      if type(ch) ~= "string" or #ch == 0 then ch = " " end
    end

    if fg == nil and bg == nil then
      layer:set(px, py, ch)
    else
      layer:set(px, py, ch, fg, bg)
    end
  end

  for dy = -r, r do
    for dx = -r, r do
      paint_cell(x + dx, y + dy)
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

  -- Phase 1: mouse drag painting (left click+hold).
  if phase == 1 then
    local cursor = ctx.cursor
    if type(cursor) == "table" and cursor.valid and (cursor.left or cursor.right) then
      local px = tonumber(cursor.p and cursor.p.x)
      local py = tonumber(cursor.p and cursor.p.y)
      local prev_left = (cursor.p and cursor.p.left) == true
      local prev_right = (cursor.p and cursor.p.right) == true
      local prev_half_y = tonumber(cursor.p and cursor.p.half_y)
      local half_y = tonumber(cursor.half_y)
      local x1 = tonumber(cursor.x) or caret.x
      local y1 = tonumber(cursor.y) or caret.y

      -- IMPORTANT: tools are executed every UI frame while dragging.
      -- To avoid "shade" instantly ramping to █ while the cursor sits still,
      -- only paint when:
      -- - we entered a new cell, or
      -- - we crossed a half-row boundary within the cell (half blocks), or
      -- - the button transitioned from up->down (press edge)
      local moved_cell = (px ~= nil and py ~= nil) and ((x1 ~= px) or (y1 ~= py)) or true
      local moved_half = (half_y ~= nil and prev_half_y ~= nil and half_y ~= prev_half_y)
      local pressed_edge = (cursor.left and not prev_left) or (cursor.right and not prev_right)

      if moved_cell or moved_half or pressed_edge then
        caret.x = clamp(x1, 0, cols - 1)
        caret.y = math.max(0, y1)

        -- If we're in half-block mode and have half-row info, interpolate in half-row space
        -- to avoid gaps when dragging quickly.
        local mode = (ctx.params and ctx.params.mode) or nil
        if mode == "half" and type(half_y) == "number" then
          local x0 = tonumber(px)
          local hy0 = tonumber(prev_half_y)
          if type(x0) ~= "number" then x0 = caret.x end
          if type(hy0) ~= "number" then
            -- If we don't have a previous half_y (older config assets), fall back to top-half of prev row.
            local y0 = tonumber(py)
            if type(y0) ~= "number" then y0 = caret.y end
            hy0 = y0 * 2
          end
          local hy1 = half_y

          local function bresenham(xa, ya, xb, yb, fn)
            xa, ya, xb, yb = math.floor(xa), math.floor(ya), math.floor(xb), math.floor(yb)
            local dx = math.abs(xb - xa)
            local sx = (xa < xb) and 1 or -1
            local dy = -math.abs(yb - ya)
            local sy = (ya < yb) and 1 or -1
            local err = dx + dy
            while true do
              fn(xa, ya)
              if xa == xb and ya == yb then break end
              local e2 = 2 * err
              if e2 >= dy then err = err + dy; xa = xa + sx end
              if e2 <= dx then err = err + dx; ya = ya + sy end
            end
          end

          bresenham(x0, hy0, caret.x, hy1, function(ix, ihy)
            ix = clamp(ix, 0, cols - 1)
            local iy = math.floor(ihy / 2)
            if iy < 0 then iy = 0 end
            paint(ctx, layer, ix, iy, ihy)
          end)
        else
          -- Default behavior: paint current cell only.
          paint(ctx, layer, caret.x, caret.y)
        end
      end
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
