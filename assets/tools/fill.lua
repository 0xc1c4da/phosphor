settings = {
  id = "fill",
  icon = "🪣",
  label = "Fill",

  -- Tool parameters (host renders UI; values are available under ctx.params.*)
  params = {
    mode = { type = "enum", label = "Mode", items = { "char", "colorize", "both", "half" }, default = "both" },
    exact = { type = "bool", label = "Exact match", default = true },
    useFg = { type = "bool", label = "Use FG", default = true },
    useBg = { type = "bool", label = "Use BG", default = true },
    maxCells = { type = "int", label = "Max cells", min = 1000, max = 500000, step = 1000, default = 200000 },
  },
}

local function clamp(v, a, b)
  if v < a then return a end
  if v > b then return b end
  return v
end

local function norm_glyph(s)
  if type(s) ~= "string" or #s == 0 then return " " end
  return s
end

local function fill(ctx, layer, sx, sy, shalfy)
  if not ctx or not layer then return end
  local p = ctx.params or {}

  local cols = tonumber(ctx.cols) or 0
  local rows = tonumber(ctx.rows) or 0
  if cols <= 0 then return end
  if rows <= 0 then return end
  if sx < 0 or sx >= cols then return end
  if sy < 0 or sy >= rows then return end

  local mode = p.mode
  if type(mode) ~= "string" then mode = "both" end
  local exact = (p.exact ~= false)

  local useFg = (p.useFg ~= false)
  local useBg = (p.useBg ~= false)
  local fg = ctx.fg
  if not useFg or type(fg) ~= "number" then fg = nil end
  local bg = ctx.bg
  if not useBg or type(bg) ~= "number" then bg = nil end

  local cursor = ctx.cursor or {}
  local secondary = cursor.right == true
  -- Right-click: swap fg/bg (icy-draw style) when both are present.
  -- NOTE: In "half" mode, right-click is used as "paint with BG" (like Pencil),
  -- so swapping here would be a double meaning.
  if secondary and mode ~= "half" and fg ~= nil and bg ~= nil then
    fg, bg = bg, fg
  end

  local brush = ctx.brush
  if type(brush) ~= "string" or #brush == 0 then brush = " " end

  -- ---------------------------------------------------------------------------
  -- Half-block helpers (▀/▄/█ + fg/bg) in half-row space.
  -- This mirrors the composition logic used by Pencil half mode.
  -- ---------------------------------------------------------------------------
  local function decode_half(cur_ch, cur_fg, cur_bg)
    cur_ch = norm_glyph(cur_ch)
    if type(cur_fg) ~= "number" then cur_fg = nil end
    if type(cur_bg) ~= "number" then cur_bg = nil end

    local is_blocky = false
    local is_vertically_blocky = false
    local upper = nil
    local lower = nil
    local left = nil
    local right = nil

    if cur_ch == "▄" then
      upper = cur_bg
      lower = cur_fg
      is_blocky = true
    elseif cur_ch == "▀" then
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
    elseif cur_ch == "▌" then
      -- CP437 221: left half = fg, right half = bg
      left = cur_fg
      right = cur_bg
      is_vertically_blocky = true
    elseif cur_ch == "▐" then
      -- CP437 222: left half = bg, right half = fg
      left = cur_bg
      right = cur_fg
      is_vertically_blocky = true
    end

    if not is_blocky and cur_fg ~= nil and cur_bg ~= nil and cur_fg == cur_bg then
      upper = cur_fg
      lower = cur_fg
      is_blocky = true
    end

    return is_blocky, is_vertically_blocky, upper, lower, left, right, cur_fg, cur_bg, cur_ch
  end

  local function half_key(x, hy)
    return hy * cols + x
  end

  local function get_half_color(x, hy)
    local y = math.floor(hy / 2)
    if y < 0 or y >= rows then return nil end
    local is_top = (hy % 2) == 0
    local cur_ch, cur_fg, cur_bg = layer:get(x, y)
    local is_blocky, _, upper, lower = decode_half(cur_ch, cur_fg, cur_bg)
    if not is_blocky then
      return nil
    end
    return is_top and upper or lower
  end

  local function set_half_color(x, hy, col)
    local y = math.floor(hy / 2)
    if y < 0 then y = 0 end
    if y >= rows then return end
    if type(col) ~= "number" then return end
    local paint_top = (hy % 2) == 0

    local cur_ch, cur_fg, cur_bg = layer:get(x, y)
    local is_blocky, _, upper, lower, _, _, fg0, bg0 = decode_half(cur_ch, cur_fg, cur_bg)
    local fallback_bg = (type(bg) == "number" and bg) or (type(fg) == "number" and fg) or 0
    if upper == nil then upper = (fg0 ~= nil and fg0) or (bg0 ~= nil and bg0) or fallback_bg end
    if lower == nil then lower = (bg0 ~= nil and bg0) or (fg0 ~= nil and fg0) or fallback_bg end

    if is_blocky then
      if (paint_top and lower == col) or ((not paint_top) and upper == col) then
        layer:set(x, y, "█", col, 0)
        return
      end
      if paint_top then
        layer:set(x, y, "▀", col, lower)
      else
        layer:set(x, y, "▄", col, upper)
      end
      return
    end

    local base_bg = (bg0 ~= nil) and bg0 or fallback_bg
    if paint_top then
      layer:set(x, y, "▀", col, base_bg)
    else
      layer:set(x, y, "▄", col, base_bg)
    end
  end

  -- Read start cell (glyph + optional fg/bg).
  local base_ch, base_fg, base_bg = layer:get(sx, sy)
  local base = norm_glyph(base_ch)

  -- If we can't change anything, bail early.
  if mode == "half" then
    -- half fill needs a paint color
    if fg == nil and bg == nil then return end
  elseif mode == "colorize" then
    if fg == nil and bg == nil then return end
  elseif mode == "char" then
    -- If brush and colors would result in a no-op, bail.
    if brush == base and fg == nil and bg == nil then return end
  else -- both
    if brush == base and fg == nil and bg == nil then return end
  end

  local maxCells = tonumber(p.maxCells) or 200000
  if maxCells < 1000 then maxCells = 1000 end
  if maxCells > 2000000 then maxCells = 2000000 end

  local visited = {}
  local stack_x = { sx }
  local stack_y = { sy }
  local n = 1
  local filled = 0

  local function key(x, y)
    return y * cols + x
  end

  -- Half-block fill: flood fill on a (cols x rows*2) grid, matching half colors.
  if mode == "half" then
    local primary_col = useFg and fg or (useBg and bg or nil)
    local secondary_col = useBg and bg or (useFg and fg or nil)
    local paint_col = secondary and secondary_col or primary_col
    if type(paint_col) ~= "number" then return end

    local start_half_y = tonumber(shalfy)
    if type(start_half_y) ~= "number" then
      -- fallback: pick top half of the clicked cell
      start_half_y = sy * 2
    end
    start_half_y = math.floor(start_half_y)
    if start_half_y < 0 then start_half_y = 0 end
    if start_half_y >= rows * 2 then start_half_y = rows * 2 - 1 end

    local base_col = get_half_color(sx, start_half_y)
    -- Match Moebius behavior: only start half-fill if we clicked a "blocky" half.
    if type(base_col) ~= "number" then return end
    local target_color = base_col
    if paint_col == target_color then return end

    local visited_h = {}
    local stack_xh = { sx }
    local stack_yh = { start_half_y }
    local stack_fx = { sx }
    local stack_fy = { start_half_y }
    local nh = 1
    local filled_h = 0

    while nh > 0 do
      local x = stack_xh[nh]
      local hy = stack_yh[nh]
      local fx = stack_fx[nh]
      local fhy = stack_fy[nh]
      stack_xh[nh] = nil
      stack_yh[nh] = nil
      stack_fx[nh] = nil
      stack_fy[nh] = nil
      nh = nh - 1

      if x >= 0 and x < cols and hy >= 0 and hy < rows * 2 then
        local k = half_key(x, hy)
        if not visited_h[k] then
          visited_h[k] = true
          local y = math.floor(hy / 2)
          if y >= 0 and y < rows then
            local cur_ch, cur_fg, cur_bg = layer:get(x, y)
            local is_blocky, is_vblocky, upper, lower, left, right = decode_half(cur_ch, cur_fg, cur_bg)
            local is_top = (hy % 2) == 0

            if is_blocky then
              local cur_col = is_top and upper or lower
              if cur_col == target_color then
                set_half_color(x, hy, paint_col)
                filled_h = filled_h + 1
                if filled_h >= maxCells then
                  return
                end
                nh = nh + 1; stack_xh[nh] = x - 1; stack_yh[nh] = hy;     stack_fx[nh] = x; stack_fy[nh] = hy
                nh = nh + 1; stack_xh[nh] = x + 1; stack_yh[nh] = hy;     stack_fx[nh] = x; stack_fy[nh] = hy
                nh = nh + 1; stack_xh[nh] = x;     stack_yh[nh] = hy - 1; stack_fx[nh] = x; stack_fy[nh] = hy
                nh = nh + 1; stack_xh[nh] = x;     stack_yh[nh] = hy + 1; stack_fx[nh] = x; stack_fy[nh] = hy
              end
            elseif is_vblocky then
              -- Match Moebius' fill behavior for vertically-blocky cells (CP437 221/222),
              -- using the direction we entered the cell to decide which half to replace.
              if type(left) ~= "number" then left = nil end
              if type(right) ~= "number" then right = nil end
              if type(fx) ~= "number" then fx = x end
              if type(fhy) ~= "number" then fhy = hy end

              local function set_left()
                if left == target_color then
                  layer:set(x, y, "▌", paint_col, right or 0)
                end
              end
              local function set_right()
                if right == target_color then
                  layer:set(x, y, "▐", paint_col, left or 0)
                end
              end

              if fhy == hy - 1 then
                -- entered from above
                if left == target_color then
                  layer:set(x, y, "▌", paint_col, right or 0)
                elseif right == target_color then
                  layer:set(x, y, "▐", paint_col, left or 0)
                end
              elseif fhy == hy + 1 then
                -- entered from below
                if right == target_color then
                  layer:set(x, y, "▐", paint_col, left or 0)
                elseif left == target_color then
                  layer:set(x, y, "▌", paint_col, right or 0)
                end
              elseif fx == x - 1 then
                -- entered from left
                set_left()
              elseif fx == x + 1 then
                -- entered from right
                set_right()
              end
            end
          end
        end
      end
    end
    return
  end

  local function matches(x, y)
    local cur_ch, cur_fg, cur_bg = layer:get(x, y)
    local cur = norm_glyph(cur_ch)
    if exact then
      return (cur == base) and (cur_fg == base_fg) and (cur_bg == base_bg)
    end
    -- Non-exact: match glyph only.
    return cur == base
  end

  local function apply_at(x, y)
    if mode == "colorize" then
      local ch = norm_glyph((layer:get(x, y)))
      layer:set(x, y, ch, fg, bg)
    elseif mode == "char" then
      if fg == nil and bg == nil then
        layer:set(x, y, brush)
      else
        layer:set(x, y, brush, fg, bg)
      end
    else -- both
      layer:set(x, y, brush, fg, bg)
    end
  end

  while n > 0 do
    local x = stack_x[n]
    local y = stack_y[n]
    stack_x[n] = nil
    stack_y[n] = nil
    n = n - 1

    if x >= 0 and x < cols and y >= 0 and y < rows then
      local k = key(x, y)
      if not visited[k] then
        visited[k] = true
        if matches(x, y) then
          apply_at(x, y)
          filled = filled + 1
          if filled >= maxCells then
            return
          end
          n = n + 1; stack_x[n] = x - 1; stack_y[n] = y
          n = n + 1; stack_x[n] = x + 1; stack_y[n] = y
          n = n + 1; stack_x[n] = x;     stack_y[n] = y - 1
          n = n + 1; stack_x[n] = x;     stack_y[n] = y + 1
        end
      end
    end
  end
end

function render(ctx, layer)
  if not ctx or not layer then return end
  if not ctx.focused then return end

  local cols = tonumber(ctx.cols) or 0
  local rows = tonumber(ctx.rows) or 0
  if cols <= 0 or rows <= 0 then return end

  local caret = ctx.caret
  if type(caret) ~= "table" then return end

  caret.x = clamp(tonumber(caret.x) or 0, 0, cols - 1)
  caret.y = clamp(tonumber(caret.y) or 0, 0, rows - 1)

  -- Only run on mouse press edge (one fill per click).
  if (tonumber(ctx.phase) or 0) == 1 then
    local cursor = ctx.cursor
    if type(cursor) == "table" and cursor.valid and (cursor.left or cursor.right) then
      local prev_left = (cursor.p and cursor.p.left) == true
      local prev_right = (cursor.p and cursor.p.right) == true
      local pressed_edge = (cursor.left and not prev_left) or (cursor.right and not prev_right)

      if pressed_edge then
        caret.x = clamp(tonumber(cursor.x) or caret.x, 0, cols - 1)
        caret.y = clamp(tonumber(cursor.y) or caret.y, 0, rows - 1)
        local shalfy = tonumber(cursor.half_y)
        fill(ctx, layer, caret.x, caret.y, shalfy)
      end
    end
    return
  end
end

