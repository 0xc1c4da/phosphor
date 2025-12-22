settings = {
  id = "02-pencil",
  icon = "🖉",
  label = "Pencil",

  -- Tool parameters (host renders UI; values are available under ctx.params.*)
  params = {
    size = { type = "int", label = "Size", ui = "slider", section = "Brush", primary = true, order = 0, min = 1, max = 20, step = 1, default = 1, width = 160 },
    mode = {
      type = "enum",
      label = "Mode",
      ui = "segmented",
      section = "Brush",
      primary = true,
      order = 1,
      inline = true,
      items = { "char", "spray", "colorize", "recolour", "half", "block", "shade" },
      default = "char",
      tooltip = "Right click swaps FG/BG in most modes. Half mode uses half-cell vertical resolution.",
    },
    useBg = { type = "bool", label = "BG", ui = "toggle", section = "Brush", primary = true, order = 2, default = true },
    useFg = { type = "bool", label = "FG", ui = "toggle", section = "Brush", primary = true, order = 3, default = true, inline = true },
  },
}

-- Keyboard navigation drawing toggle (does not affect mouse painting).
local keyboard_draw_enabled = true

-- RNG init (for spray mode).
local rng_seeded = false

-- Simple double-click detector for canvas clicks (Lua-side; host cursor state has no dblclick flag).
local last_click_ms = nil
local last_click_x = nil
local last_click_y = nil
local last_click_btn = nil -- "left" | "right"
local dblclick_threshold_ms = 350

local function clamp(v, a, b)
  if v < a then return a end
  if v > b then return b end
  return v
end

local function current_brush(ctx)
  local b = ctx and ctx.glyph or nil
  if type(b) ~= "string" or #b == 0 then
    return " "
  end
  return b
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

  -- Current attribute bitmask (0 = none).
  local attrs = ctx.attrs
  if type(attrs) ~= "number" then attrs = 0 end
  attrs = math.floor(attrs)
  if attrs < 0 then attrs = 0 end

  local cursor = ctx.cursor or {}
  local secondary = cursor.right == true
  local cursor_half_y = nil
  -- Only trust half_y when the cursor is valid (otherwise it's often 0/default).
  if type(cursor) == "table" and cursor.valid and type(cursor.half_y) == "number" then
    cursor_half_y = cursor.half_y
  end
  if type(half_y_override) == "number" then
    cursor_half_y = half_y_override
  end

  -- Right-click: swap fg/bg for most modes (icy-draw style).
  -- NOTE: In "half" mode, the painted half is chosen by cursor.half_y (mouse position within
  -- the cell). Right-click is used as "paint with BG", so swapping here would be a double
  -- meaning and breaks half-block painting semantics.
  -- Also don't swap in "recolour" mode: Moebius semantics are explicitly to=FG, from=BG.
  if secondary and mode ~= "shade" and mode ~= "half" and mode ~= "recolour" and fg ~= nil and bg ~= nil then
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
      -- Half-block painting is handled in half-row space below so brush "size" is correct.
      return
    elseif mode == "recolour" then
      -- Option A / Moebius-style replace color:
      -- - to = current FG
      -- - from = current BG
      -- Replace either channel that matches 'from' with 'to', preserving glyph.
      if type(fg) ~= "number" or type(bg) ~= "number" then return end
      local to = fg
      local from = bg
      local cur_ch, cur_fg, cur_bg = layer:get(px, py)
      if type(cur_ch) ~= "string" or #cur_ch == 0 then cur_ch = " " end
      if type(cur_fg) ~= "number" then cur_fg = nil end
      if type(cur_bg) ~= "number" then cur_bg = nil end

      if cur_fg ~= from and cur_bg ~= from then
        return
      end
      local new_fg = (cur_fg == from) and to or cur_fg
      local new_bg = (cur_bg == from) and to or cur_bg

      if new_fg == cur_fg and new_bg == cur_bg then
        return
      end
      layer:set(px, py, cur_ch, new_fg, new_bg)
      return
    elseif mode == "colorize" then
      -- Preserve glyph, only modify fg/bg.
      if fg == nil and bg == nil then
        return -- truly "colorize only": nothing to do if no colors are enabled
      end
      ch = layer:get(px, py)
      if type(ch) ~= "string" or #ch == 0 then ch = " " end
    else
      ch = current_brush(ctx)
      if type(ch) ~= "string" or #ch == 0 then ch = " " end
    end

    if fg == nil and bg == nil then
      if attrs == 0 then
        layer:set(px, py, ch)
      else
        layer:set(px, py, ch, nil, nil, attrs)
      end
    else
      if attrs == 0 then
        layer:set(px, py, ch, fg, bg)
      else
        layer:set(px, py, ch, fg, bg, attrs)
      end
    end
  end

  local function paint_half_at(hx, hy)
    if hx < 0 or hx >= cols then return end
    if type(hy) ~= "number" then return end
    hy = math.floor(hy)
    if hy < 0 then return end

    local py = math.floor(hy / 2)
    if py < 0 then return end

    -- Choose paint color:
    -- - primary click paints with FG (if enabled)
    -- - right click paints with BG (if enabled)
    -- with fallbacks if only one is enabled.
    local primary_col = useFg and fg or (useBg and bg or nil)
    local secondary_col = useBg and bg or (useFg and fg or nil)
    local col = secondary and secondary_col or primary_col
    if type(col) ~= "number" then
      return
    end

    -- Read current cell and its colors (nil means "unset" in the layer).
    local cur_ch, cur_fg, cur_bg = layer:get(hx, py)
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

    local paint_top = (hy % 2) == 0
    if is_blocky then
      -- If the other half already matches the paint color, collapse to a full block.
      if (paint_top and lower == col) or ((not paint_top) and upper == col) then
        if attrs == 0 then
          layer:set(hx, py, "█", col, 0)
        else
          layer:set(hx, py, "█", col, 0, attrs)
        end
        return
      end
      if paint_top then
        if attrs == 0 then
          layer:set(hx, py, "▀", col, lower)
        else
          layer:set(hx, py, "▀", col, lower, attrs)
        end
      else
        if attrs == 0 then
          layer:set(hx, py, "▄", col, upper)
        else
          layer:set(hx, py, "▄", col, upper, attrs)
        end
      end
    else
      -- Non-blocky cell: preserve the existing background (or fallback) for the other half.
      local base_bg = (cur_bg ~= nil) and cur_bg or fallback_bg
      if paint_top then
        if attrs == 0 then
          layer:set(hx, py, "▀", col, base_bg)
        else
          layer:set(hx, py, "▀", col, base_bg, attrs)
        end
      else
        if attrs == 0 then
          layer:set(hx, py, "▄", col, base_bg)
        else
          layer:set(hx, py, "▄", col, base_bg, attrs)
        end
      end
    end
  end

  if mode == "half" then
    -- In half-block mode, brush size is applied in half-row coordinates so a "square" brush
    -- stays square at the doubled vertical resolution (matches Moebius/IcyDraw semantics).
    local base_hy = cursor_half_y
    if type(base_hy) ~= "number" then
      base_hy = y * 2 -- deterministic fallback (top half)
    end
    base_hy = math.floor(base_hy)
    for dh = -r, r do
      for dx = -r, r do
        paint_half_at(x + dx, base_hy + dh)
      end
    end
  elseif mode == "spray" then
    -- Spray is sample-based (not per-cell probability) so it doesn't "solid fill" at larger sizes.
    -- Each stroke step stamps a small number of random points inside the brush.
    local side = (2 * r) + 1
    local n = math.max(1, math.floor(side * 0.45))
    for i = 1, n do
      local dx = (r > 0) and math.random(-r, r) or 0
      local dy = (r > 0) and math.random(-r, r) or 0
      paint_cell(x + dx, y + dy)
    end
  else
    for dy = -r, r do
      for dx = -r, r do
        paint_cell(x + dx, y + dy)
      end
    end
  end
end

function render(ctx, layer)
  if not ctx or not layer then return end
  if not ctx.focused then return end

  -- Seed RNG once (spray mode). Prefer host time if available.
  if not rng_seeded then
    local t = tonumber(ctx.time)
    local seed = nil
    if type(t) == "number" then
      seed = math.floor(t * 1000)
    else
      -- Deterministic-ish fallback if host time isn't available.
      seed = math.floor((tonumber(ctx.phase) or 0) * 100000 + (tonumber(ctx.cols) or 0))
    end
    -- Keep seed in 32-bit signed range for Lua implementations that care.
    seed = seed % 2147483647
    if seed < 0 then seed = -seed end
    math.randomseed(seed)
    -- Discard a few first values (common practice with some RNGs).
    math.random(); math.random(); math.random()
    rng_seeded = true
  end

  local cols = tonumber(ctx.cols) or 0
  if cols <= 0 then return end

  local caret = ctx.caret
  if type(caret) ~= "table" then return end

  caret.x = clamp(tonumber(caret.x) or 0, 0, cols - 1)
  caret.y = math.max(0, tonumber(caret.y) or 0)

  local phase = tonumber(ctx.phase) or 0

  -- Brush size preview (host overlay; transient).
  do
    local p = ctx.params or {}
    local size = tonumber(p.size) or 1
    if size < 1 then size = 1 end
    if size > 100 then size = 100 end
    local r = math.floor(size / 2)
    if ctx.out ~= nil then
      ctx.out[#ctx.out + 1] = { type = "brush.preview", anchor = "cursor", rx = r, ry = r }
    end
  end

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
      local mode = (ctx.params and ctx.params.mode) or nil

      -- IMPORTANT: tools are executed every UI frame while dragging.
      -- To avoid "shade" instantly ramping to █ while the cursor sits still,
      -- only paint when:
      -- - we entered a new cell, or
      -- - we crossed a half-row boundary within the cell (half blocks), or
      -- - the button transitioned from up->down (press edge)
      local moved_cell = (px ~= nil and py ~= nil) and ((x1 ~= px) or (y1 ~= py)) or true
      local pressed_edge = (cursor.left and not prev_left) or (cursor.right and not prev_right)

      -- Half-row movement is only meaningful for half-block drawing; for other modes it can
      -- cause accidental “extra” paints (e.g. shade ramping too fast) when moving within a cell.
      local moved_half = (mode == "half") and (half_y ~= nil and prev_half_y ~= nil and half_y ~= prev_half_y)

      -- Detect a double click on the canvas (two press edges on the same cell within threshold).
      local is_double_click = false
      if pressed_edge then
        local now_ms = tonumber(ctx.time)
        local btn = cursor.right and "right" or "left"
        if type(now_ms) == "number" and last_click_ms ~= nil then
          if (btn == last_click_btn) and (x1 == last_click_x) and (y1 == last_click_y) and
             ((now_ms - last_click_ms) <= dblclick_threshold_ms) then
            is_double_click = true
          end
        end
        if type(now_ms) == "number" then
          last_click_ms = now_ms
          last_click_btn = btn
          last_click_x = x1
          last_click_y = y1
        end
      end

      if moved_cell or moved_half or pressed_edge then
        caret.x = clamp(x1, 0, cols - 1)
        caret.y = math.max(0, y1)

        local function bresenham(xa, ya, xb, yb, fn, skip_first)
          xa, ya, xb, yb = math.floor(xa), math.floor(ya), math.floor(xb), math.floor(yb)
          local dx = math.abs(xb - xa)
          local sx = (xa < xb) and 1 or -1
          local dy = -math.abs(yb - ya)
          local sy = (ya < yb) and 1 or -1
          local err = dx + dy
          local first = true
          while true do
            if not (skip_first and first) then
              fn(xa, ya)
            end
            first = false
            if xa == xb and ya == yb then break end
            local e2 = 2 * err
            if e2 >= dy then err = err + dy; xa = xa + sx end
            if e2 <= dx then err = err + dx; ya = ya + sy end
          end
        end

        -- Interpolate for ALL modes to avoid gaps when dragging quickly.
        -- - Half mode interpolates in half-row space (higher vertical resolution).
        -- - Other modes interpolate in cell space.
        -- Use skip_first when we truly moved, to avoid double-hitting the previous point
        -- (important for shade mode stepping).
        local skip_first = moved_cell or moved_half
        if mode == "half" and type(half_y) == "number" then
          local x0 = tonumber(px)
          if type(x0) ~= "number" then x0 = caret.x end
          local hy0 = tonumber(prev_half_y)
          if type(hy0) ~= "number" then
            hy0 = half_y
            skip_first = false
          end
          local hy1 = half_y

          bresenham(x0, hy0, caret.x, hy1, function(ix, ihy)
            ix = clamp(ix, 0, cols - 1)
            local iy = math.floor(ihy / 2)
            if iy < 0 then iy = 0 end
            paint(ctx, layer, ix, iy, ihy)
          end, skip_first)
        else
          local x0 = tonumber(px)
          local y0 = tonumber(py)
          if type(x0) ~= "number" or type(y0) ~= "number" then
            skip_first = false
            x0 = caret.x
            y0 = caret.y
          end
          bresenham(x0, y0, caret.x, caret.y, function(ix, iy)
            ix = clamp(ix, 0, cols - 1)
            if iy < 0 then iy = 0 end
            paint(ctx, layer, ix, iy)
          end, skip_first)
        end

        -- On double click, also stamp the current brush glyph at the clicked cell.
        -- This is intentionally independent of the current mode (colorize/recolour/etc).
        if is_double_click then
          local p = ctx.params or {}
          local useFg = (p.useFg ~= false)
          local useBg = (p.useBg ~= false)
          local fg = ctx.fg
          if not useFg or type(fg) ~= "number" then fg = nil end
          local bg = ctx.bg
          if not useBg or type(bg) ~= "number" then bg = nil end
          local attrs = ctx.attrs
          if type(attrs) ~= "number" then attrs = 0 end
          attrs = math.floor(attrs)
          if attrs < 0 then attrs = 0 end

          local brush = current_brush(ctx)

          -- Match icy-draw-style swap behavior for "char-ish" stamping.
          local secondary = cursor.right == true
          local mode = p.mode
          if type(mode) ~= "string" then mode = "char" end
          if secondary and mode ~= "half" and mode ~= "recolour" and type(fg) == "number" and type(bg) == "number" then
            fg, bg = bg, fg
          end

          local size = tonumber(p.size) or 1
          if size < 1 then size = 1 end
          if size > 100 then size = 100 end
          local r = math.floor(size / 2)

          local function stamp_cell(px, py)
            if px < 0 or px >= cols then return end
            if py < 0 then return end

            if fg == nil and bg == nil then
              if attrs == 0 then
                layer:set(px, py, brush)
              else
                layer:set(px, py, brush, nil, nil, attrs)
              end
            else
              if attrs == 0 then
                layer:set(px, py, brush, fg, bg)
              else
                layer:set(px, py, brush, fg, bg, attrs)
              end
            end
          end

          for dy = -r, r do
            for dx = -r, r do
              stamp_cell(caret.x + dx, caret.y + dy)
            end
          end
        end
      end
    end
    return
  end

  -- Phase 0: keyboard navigation paints after moving.
  local keys = ctx.keys or {}
  local mods = ctx.mods or {}
  local moved = false
  local x0 = caret.x
  local y0 = caret.y

  -- Toggle keyboard drawing on/off. (Mouse drawing is unaffected; runs in phase 1.)
  if keys.enter then
    keyboard_draw_enabled = not keyboard_draw_enabled
  end

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

  if moved and keyboard_draw_enabled then
    -- Keyboard movement has no half-row info. In half mode, draw in half-row space by
    -- rasterizing between the previous and new caret positions. This:
    -- - lets vertical moves paint both halves (via the intermediate half-row), matching
    --   Moebius/IcyDraw "half-res" semantics
    -- - avoids relying on mouse cursor.half_y (which may be stale/invalid in keyboard use)
    local mode = (ctx.params and ctx.params.mode) or nil
    if mode == "half" then
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

      -- Use Shift as a deterministic "lower half" selector for keyboard drawing.
      -- Default is top half.
      local parity = (mods.shift == true) and 1 or 0
      local hy0 = (y0 * 2) + parity
      local hy1 = (caret.y * 2) + parity

      bresenham(x0, hy0, caret.x, hy1, function(ix, ihy)
        ix = clamp(ix, 0, cols - 1)
        local iy = math.floor(ihy / 2)
        if iy < 0 then iy = 0 end
        paint(ctx, layer, ix, iy, ihy)
      end)
    else
      paint(ctx, layer, caret.x, caret.y)
    end
  end
end
