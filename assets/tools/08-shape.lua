settings = {
  id = "08-shape",
  icon = "⌺",
  label = "Shape",

  -- Tool parameters (host renders UI; values are available under ctx.params.*)
  params = {
    shape = { type = "enum", label = "Shape", items = { "line", "rectangle", "ellipse", "triangle" }, default = "line" },
    fill = { type = "enum", label = "Fill", items = { "outline", "filled" }, default = "outline" },

    -- Rendering options
    resolution = { type = "enum", label = "Resolution", items = { "cell", "half" }, default = "cell" },
    mode = { type = "enum", label = "Mode", items = { "char", "colorize" }, default = "char" },
    style = { type = "enum", label = "Style", items = { "brush", "box_single", "box_double", "rounded", "ascii", "block" }, default = "brush" },
    dash = { type = "enum", label = "Dash", items = { "solid", "dashed", "dotted" }, default = "solid" },
    size = { type = "int", label = "Size", min = 1, max = 20, step = 1, default = 1 },
    useFg = { type = "bool", label = "Use FG", default = true },
    useBg = { type = "bool", label = "Use BG", default = true },

    -- UX
    selectAfter = { type = "bool", label = "Select result", default = true },
  },
}

-- -----------------------------------------------------------------------------
-- Utilities (mostly lifted from other tools, especially `font.lua`)
-- -----------------------------------------------------------------------------

local function clamp_int(v, lo, hi)
  v = math.floor(tonumber(v) or 0)
  lo = math.floor(tonumber(lo) or 0)
  hi = math.floor(tonumber(hi) or 0)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

local function to_int(v, def)
  v = tonumber(v)
  if v == nil then return def end
  return math.floor(v)
end

local function set_cell(layer, x, y, cp_or_glyph, fg, bg)
  if fg == nil and bg == nil then
    layer:set(x, y, cp_or_glyph)
    layer:clearStyle(x, y)
  else
    layer:set(x, y, cp_or_glyph, fg, bg)
  end
end

local function effective_rect(ctx, x, y, w, h)
  local cols = tonumber((ctx and ctx.cols) or 0) or 0
  local rows = tonumber((ctx and ctx.rows) or 0) or 0
  x = math.floor(tonumber(x) or 0)
  y = math.floor(tonumber(y) or 0)
  w = math.floor(tonumber(w) or 0)
  h = math.floor(tonumber(h) or 0)
  if x < 0 then x = 0 end
  if y < 0 then y = 0 end
  if w < 0 then w = 0 end
  if h < 0 then h = 0 end

  if cols > 0 then
    x = clamp_int(x, 0, cols - 1)
    local max_w = cols - x
    if max_w < 0 then max_w = 0 end
    if w > max_w then w = max_w end
  end
  if rows > 0 then
    y = clamp_int(y, 0, rows - 1)
    local max_h = rows - y
    if max_h < 0 then max_h = 0 end
    if h > max_h then h = max_h end
  end

  if w <= 0 or h <= 0 then
    return x, y, 0, 0
  end
  return x, y, w, h
end

local function capture_backup(ctx, layer, x, y, w, h)
  if not ctx or not layer then return nil end
  x, y, w, h = effective_rect(ctx, x, y, w, h)
  if w <= 0 or h <= 0 then return nil end

  -- Safety: allow larger than font tool (shapes are often bigger), but still cap.
  local max_cells = 80000
  if (w * h) > max_cells then
    return nil
  end

  local b = { x = x, y = y, w = w, h = h, cp = {}, fg = {}, bg = {} }
  for j = 0, h - 1 do
    for i = 0, w - 1 do
      local _, fg, bg, cp = layer:get(x + i, y + j)
      local idx = (j * w) + i + 1
      b.cp[idx] = tonumber(cp) or 32
      b.fg[idx] = (type(fg) == "number") and math.floor(fg) or nil
      b.bg[idx] = (type(bg) == "number") and math.floor(bg) or nil
    end
  end
  return b
end

local function restore_backup(layer, b)
  if not layer or type(b) ~= "table" then return end
  local x = tonumber(b.x) or 0
  local y = tonumber(b.y) or 0
  local w = tonumber(b.w) or 0
  local h = tonumber(b.h) or 0
  if w <= 0 or h <= 0 then return end
  local cp = b.cp or {}
  local fg = b.fg or {}
  local bg = b.bg or {}
  for j = 0, h - 1 do
    for i = 0, w - 1 do
      local idx = (j * w) + i + 1
      local cpi = tonumber(cp[idx]) or 32
      local fgi = fg[idx]
      local bgi = bg[idx]
      set_cell(layer, x + i, y + j, cpi, fgi, bgi)
    end
  end
end

local function reorientate(a, b)
  if a <= b then return a, b end
  return b, a
end

local function bresenham(x0, y0, x1, y1, fn)
  x0, y0, x1, y1 = math.floor(x0), math.floor(y0), math.floor(x1), math.floor(y1)
  local dx = math.abs(x1 - x0)
  local sx = (x0 < x1) and 1 or -1
  local dy = -math.abs(y1 - y0)
  local sy = (y0 < y1) and 1 or -1
  local err = dx + dy
  while true do
    fn(x0, y0)
    if x0 == x1 and y0 == y1 then break end
    local e2 = 2 * err
    if e2 >= dy then err = err + dy; x0 = x0 + sx end
    if e2 <= dx then err = err + dx; y0 = y0 + sy end
  end
end

-- -----------------------------------------------------------------------------
-- Half-block paint helpers (vertical high-res), based on Pencil/Fill logic
-- -----------------------------------------------------------------------------

local function decode_half(cur_ch, cur_fg, cur_bg)
  if type(cur_ch) ~= "string" or #cur_ch == 0 then cur_ch = " " end
  if type(cur_fg) ~= "number" then cur_fg = nil end
  if type(cur_bg) ~= "number" then cur_bg = nil end

  local is_blocky = false
  local upper = nil
  local lower = nil

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
  end

  if not is_blocky and cur_fg ~= nil and cur_bg ~= nil and cur_fg == cur_bg then
    upper = cur_fg
    lower = cur_fg
    is_blocky = true
  end
  return is_blocky, upper, lower, cur_fg, cur_bg, cur_ch
end

local function set_half_color(ctx, layer, x, half_y, col)
  if type(col) ~= "number" then return end
  local rows = tonumber((ctx and ctx.rows) or 0) or 0
  if rows <= 0 then return end

  local hy = math.floor(tonumber(half_y) or 0)
  if hy < 0 then hy = 0 end
  if hy >= rows * 2 then hy = rows * 2 - 1 end
  local y = math.floor(hy / 2)
  if y < 0 or y >= rows then return end
  local paint_top = (hy % 2) == 0

  local cur_ch, cur_fg, cur_bg = layer:get(x, y)
  local is_blocky, upper, lower, fg0, bg0 = decode_half(cur_ch, cur_fg, cur_bg)

  local fallback_bg = (type(ctx.bg) == "number" and math.floor(ctx.bg))
    or (type(ctx.fg) == "number" and math.floor(ctx.fg))
    or 0

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

-- -----------------------------------------------------------------------------
-- Shape drawing (cell + half resolution)
-- -----------------------------------------------------------------------------

local function get_paint(ctx, secondary)
  local p = ctx.params or {}
  local useFg = (p.useFg ~= false)
  local useBg = (p.useBg ~= false)
  local fg = (useFg and type(ctx.fg) == "number") and math.floor(ctx.fg) or nil
  local bg = (useBg and type(ctx.bg) == "number") and math.floor(ctx.bg) or nil

  -- Right button: swap colors (icy-draw style) for cell workflows.
  if secondary and fg ~= nil and bg ~= nil then
    fg, bg = bg, fg
  end
  return fg, bg
end

local function paint_cell(ctx, layer, x, y, glyph, fg, bg)
  local cols = tonumber((ctx and ctx.cols) or 0) or 0
  local rows = tonumber((ctx and ctx.rows) or 0) or 0
  if cols > 0 and (x < 0 or x >= cols) then return end
  if rows > 0 and (y < 0 or y >= rows) then return end
  if y < 0 then return end

  local p = ctx.params or {}
  local mode = p.mode
  if type(mode) ~= "string" then mode = "char" end

  if mode == "colorize" then
    if fg == nil and bg == nil then return end
    local ch = layer:get(x, y)
    if type(ch) ~= "string" or #ch == 0 then ch = " " end
    set_cell(layer, x, y, ch, fg, bg)
    return
  end

  if type(glyph) ~= "string" or #glyph == 0 then glyph = " " end
  set_cell(layer, x, y, glyph, fg, bg)
end

local function style_pack(style)
  if style == "box_single" then
    return { h = "─", v = "│", tl = "┌", tr = "┐", bl = "└", br = "┘", diag1 = "╱", diag2 = "╲", fill = nil }
  elseif style == "box_double" then
    return { h = "═", v = "║", tl = "╔", tr = "╗", bl = "╚", br = "╝", diag1 = "╱", diag2 = "╲", fill = nil }
  elseif style == "rounded" then
    return { h = "─", v = "│", tl = "╭", tr = "╮", bl = "╰", br = "╯", diag1 = "╱", diag2 = "╲", fill = nil }
  elseif style == "ascii" then
    return { h = "-", v = "|", tl = "+", tr = "+", bl = "+", br = "+", diag1 = "/", diag2 = "\\", fill = nil }
  elseif style == "block" then
    return { h = "█", v = "█", tl = "█", tr = "█", bl = "█", br = "█", diag1 = "█", diag2 = "█", fill = "█" }
  end
  return nil
end

local function paint_point_with_size(ctx, layer, x, y, glyph, fg, bg, size)
  size = tonumber(size) or 1
  if size < 1 then size = 1 end
  if size > 100 then size = 100 end
  local r = math.floor(size / 2)
  for dy = -r, r do
    for dx = -r, r do
      paint_cell(ctx, layer, x + dx, y + dy, glyph, fg, bg)
    end
  end
end

local function dash_ok(dash, i)
  if dash == "dashed" then
    return (i % 2) == 0
  elseif dash == "dotted" then
    return (i % 3) == 0
  end
  return true
end

local function draw_line_cell(ctx, layer, x0, y0, x1, y1, glyph, fg, bg)
  local p = ctx.params or {}
  local dash = p.dash
  if type(dash) ~= "string" then dash = "solid" end
  local size = tonumber(p.size) or 1
  local i = 0
  bresenham(x0, y0, x1, y1, function(x, y)
    i = i + 1
    if dash_ok(dash, i) then
      paint_point_with_size(ctx, layer, x, y, glyph, fg, bg, size)
    end
  end)
end

local function draw_rect_outline_cell(ctx, layer, x0, y0, x1, y1, st, brush, fg, bg)
  local p = ctx.params or {}
  local dash = p.dash
  if type(dash) ~= "string" then dash = "solid" end
  local size = tonumber(p.size) or 1

  local sx, dx = reorientate(x0, x1)
  local sy, dy = reorientate(y0, y1)

  local tl = brush
  local tr = brush
  local bl = brush
  local br = brush
  local hh = brush
  local vv = brush
  if st ~= nil and (p.mode == "char") then
    tl = st.tl or brush
    tr = st.tr or brush
    bl = st.bl or brush
    br = st.br or brush
    hh = st.h or brush
    vv = st.v or brush
  end

  -- Single row/col special cases:
  if sx == dx and sy == dy then
    paint_point_with_size(ctx, layer, sx, sy, tl, fg, bg, size)
    return
  end
  if sy == dy then
    draw_line_cell(ctx, layer, sx, sy, dx, sy, hh, fg, bg)
    return
  end
  if sx == dx then
    draw_line_cell(ctx, layer, sx, sy, sx, dy, vv, fg, bg)
    return
  end

  -- Top/bottom
  local i = 0
  for x = sx + 1, dx - 1 do
    i = i + 1
    if dash_ok(dash, i) then
      paint_point_with_size(ctx, layer, x, sy, hh, fg, bg, size)
      paint_point_with_size(ctx, layer, x, dy, hh, fg, bg, size)
    end
  end
  -- Sides
  i = 0
  for y = sy + 1, dy - 1 do
    i = i + 1
    if dash_ok(dash, i) then
      paint_point_with_size(ctx, layer, sx, y, vv, fg, bg, size)
      paint_point_with_size(ctx, layer, dx, y, vv, fg, bg, size)
    end
  end
  -- Corners (always draw)
  paint_point_with_size(ctx, layer, sx, sy, tl, fg, bg, size)
  paint_point_with_size(ctx, layer, dx, sy, tr, fg, bg, size)
  paint_point_with_size(ctx, layer, sx, dy, bl, fg, bg, size)
  paint_point_with_size(ctx, layer, dx, dy, br, fg, bg, size)
end

local function draw_rect_filled_cell(ctx, layer, x0, y0, x1, y1, fill_glyph, fg, bg)
  -- For filled shapes, keep it cheap and deterministic.
  local sx, dx = reorientate(x0, x1)
  local sy, dy = reorientate(y0, y1)
  for y = sy, dy do
    for x = sx, dx do
      paint_cell(ctx, layer, x, y, fill_glyph, fg, bg)
    end
  end
end

local function ellipse_outline_points(from_x, from_y, to_x, to_y)
  -- Midpoint ellipse based on IcyDraw; returns list of {x=..., y=...}
  local pts = {}
  local rx = math.floor(math.abs(from_x - to_x) / 2)
  local ry = math.floor(math.abs(from_y - to_y) / 2)
  local xc = math.floor((from_x + to_x) / 2)
  local yc = math.floor((from_y + to_y) / 2)

  -- Degenerate cases: treat as line / point.
  if rx <= 0 and ry <= 0 then
    pts[#pts + 1] = { x = xc, y = yc }
    return pts
  end
  if rx <= 0 then
    for y = yc - ry, yc + ry do
      pts[#pts + 1] = { x = xc, y = y }
    end
    return pts
  end
  if ry <= 0 then
    for x = xc - rx, xc + rx do
      pts[#pts + 1] = { x = x, y = yc }
    end
    return pts
  end

  local x = 0
  local y = ry
  local d1 = (ry * ry) - (rx * rx * ry) + math.floor((rx * rx) / 4)
  local dx = 2 * ry * ry * x
  local dy = 2 * rx * rx * y

  while dx < dy do
    pts[#pts + 1] = { x = -x + xc, y = y + yc }
    pts[#pts + 1] = { x = x + xc, y = y + yc }
    pts[#pts + 1] = { x = -x + xc, y = -y + yc }
    pts[#pts + 1] = { x = x + xc, y = -y + yc }

    if d1 < 0 then
      x = x + 1
      dx = dx + 2 * ry * ry
      d1 = d1 + dx + (ry * ry)
    else
      x = x + 1
      y = y - 1
      dx = dx + 2 * ry * ry
      dy = dy - 2 * rx * rx
      d1 = d1 + dx - dy + (ry * ry)
    end
  end

  local d2 = ((ry * ry) * (x * x)) + ((rx * rx) * ((y - 1) * (y - 1))) - (rx * rx * ry * ry)
  while y >= 0 do
    pts[#pts + 1] = { x = -x + xc, y = y + yc }
    pts[#pts + 1] = { x = x + xc, y = y + yc }
    pts[#pts + 1] = { x = -x + xc, y = -y + yc }
    pts[#pts + 1] = { x = x + xc, y = -y + yc }

    if d2 > 0 then
      y = y - 1
      dy = dy - 2 * rx * rx
      d2 = d2 + (rx * rx) - dy
    else
      y = y - 1
      x = x + 1
      dx = dx + 2 * ry * ry
      dy = dy - 2 * rx * rx
      d2 = d2 + dx - dy + (rx * rx)
    end
  end
  return pts
end

local function draw_ellipse_outline_cell(ctx, layer, x0, y0, x1, y1, glyph, fg, bg)
  local p = ctx.params or {}
  local dash = p.dash
  if type(dash) ~= "string" then dash = "solid" end
  local size = tonumber(p.size) or 1

  local pts = ellipse_outline_points(x0, y0, x1, y1)
  local seen = {}
  local i = 0
  for _, pt in ipairs(pts) do
    local key = tostring(pt.x) .. "," .. tostring(pt.y)
    if not seen[key] then
      seen[key] = true
      i = i + 1
      if dash_ok(dash, i) then
        paint_point_with_size(ctx, layer, pt.x, pt.y, glyph, fg, bg, size)
      end
    end
  end
end

local function draw_ellipse_filled_cell(ctx, layer, x0, y0, x1, y1, glyph, fg, bg)
  local pts = ellipse_outline_points(x0, y0, x1, y1)
  if #pts == 0 then return end

  -- Build scanlines (y -> minx,maxx) from outline points.
  local minx = {}
  local maxx = {}
  for _, pt in ipairs(pts) do
    local y = pt.y
    local x = pt.x
    if minx[y] == nil or x < minx[y] then minx[y] = x end
    if maxx[y] == nil or x > maxx[y] then maxx[y] = x end
  end
  for y, xlo in pairs(minx) do
    local xhi = maxx[y]
    if xhi ~= nil then
      for x = xlo, xhi do
        paint_cell(ctx, layer, x, y, glyph, fg, bg)
      end
    end
  end
end

local function tri_vertices_from_drag(x0, y0, x1, y1, constrain)
  local sx, dx = reorientate(x0, x1)
  local sy, dy = reorientate(y0, y1)
  local midx = math.floor((sx + dx) / 2)
  local topy = sy
  local boty = dy

  -- Default: upright isosceles triangle in the drag AABB.
  local ax, ay = sx, boty
  local bx, by = dx, boty
  local cx, cy = midx, topy

  if constrain then
    -- Shift: keep it "more equilateral-ish" by forcing height ~= width*0.5 (coarse).
    local w = dx - sx
    local h = boty - topy
    local want_h = math.floor(w / 2)
    if want_h < 1 then want_h = 1 end
    if h > want_h then
      topy = boty - want_h
      if topy < 0 then topy = 0 end
      cy = topy
    end
  end

  return ax, ay, bx, by, cx, cy
end

local function fill_triangle_cell(ctx, layer, ax, ay, bx, by, cx, cy, glyph, fg, bg)
  -- Scanline fill by computing x extents per y from edges.
  local minx = {}
  local maxx = {}

  local function mark(x, y)
    if minx[y] == nil or x < minx[y] then minx[y] = x end
    if maxx[y] == nil or x > maxx[y] then maxx[y] = x end
  end

  bresenham(ax, ay, bx, by, mark)
  bresenham(bx, by, cx, cy, mark)
  bresenham(cx, cy, ax, ay, mark)

  for y, xlo in pairs(minx) do
    local xhi = maxx[y]
    if xhi ~= nil then
      for x = xlo, xhi do
        paint_cell(ctx, layer, x, y, glyph, fg, bg)
      end
    end
  end
end

local function draw_triangle_outline_cell(ctx, layer, ax, ay, bx, by, cx, cy, glyph, fg, bg)
  draw_line_cell(ctx, layer, ax, ay, bx, by, glyph, fg, bg)
  draw_line_cell(ctx, layer, bx, by, cx, cy, glyph, fg, bg)
  draw_line_cell(ctx, layer, cx, cy, ax, ay, glyph, fg, bg)
end

-- Half-resolution drawing functions: all coordinates are (x, half_y) with half_y in [0, rows*2).

local function draw_line_half(ctx, layer, x0, hy0, x1, hy1, col)
  local p = ctx.params or {}
  local dash = p.dash
  if type(dash) ~= "string" then dash = "solid" end
  local i = 0
  bresenham(x0, hy0, x1, hy1, function(x, hy)
    i = i + 1
    if dash_ok(dash, i) then
      set_half_color(ctx, layer, x, hy, col)
    end
  end)
end

local function draw_rect_outline_half(ctx, layer, x0, hy0, x1, hy1, col)
  local sx, dx = reorientate(x0, x1)
  local sy, dy = reorientate(hy0, hy1)
  draw_line_half(ctx, layer, sx, sy, dx, sy, col)
  if dy > sy then
    draw_line_half(ctx, layer, sx, dy, dx, dy, col)
    if dy > sy + 1 then
      draw_line_half(ctx, layer, sx, sy + 1, sx, dy - 1, col)
      draw_line_half(ctx, layer, dx, sy + 1, dx, dy - 1, col)
    end
  end
end

local function draw_rect_filled_half(ctx, layer, x0, hy0, x1, hy1, col)
  local sx, dx = reorientate(x0, x1)
  local sy, dy = reorientate(hy0, hy1)
  for hy = sy, dy do
    for x = sx, dx do
      set_half_color(ctx, layer, x, hy, col)
    end
  end
end

local function draw_ellipse_outline_half(ctx, layer, x0, hy0, x1, hy1, col)
  local pts = ellipse_outline_points(x0, hy0, x1, hy1)
  local seen = {}
  for _, pt in ipairs(pts) do
    local key = tostring(pt.x) .. "," .. tostring(pt.y)
    if not seen[key] then
      seen[key] = true
      set_half_color(ctx, layer, pt.x, pt.y, col)
    end
  end
end

local function draw_ellipse_filled_half(ctx, layer, x0, hy0, x1, hy1, col)
  local pts = ellipse_outline_points(x0, hy0, x1, hy1)
  if #pts == 0 then return end
  local minx = {}
  local maxx = {}
  for _, pt in ipairs(pts) do
    local y = pt.y
    local x = pt.x
    if minx[y] == nil or x < minx[y] then minx[y] = x end
    if maxx[y] == nil or x > maxx[y] then maxx[y] = x end
  end
  for hy, xlo in pairs(minx) do
    local xhi = maxx[hy]
    if xhi ~= nil then
      for x = xlo, xhi do
        set_half_color(ctx, layer, x, hy, col)
      end
    end
  end
end

local function draw_triangle_outline_half(ctx, layer, ax, ay, bx, by, cx, cy, col)
  draw_line_half(ctx, layer, ax, ay, bx, by, col)
  draw_line_half(ctx, layer, bx, by, cx, cy, col)
  draw_line_half(ctx, layer, cx, cy, ax, ay, col)
end

local function fill_triangle_half(ctx, layer, ax, ay, bx, by, cx, cy, col)
  local minx = {}
  local maxx = {}
  local function mark(x, y)
    if minx[y] == nil or x < minx[y] then minx[y] = x end
    if maxx[y] == nil or x > maxx[y] then maxx[y] = x end
  end
  bresenham(ax, ay, bx, by, mark)
  bresenham(bx, by, cx, cy, mark)
  bresenham(cx, cy, ax, ay, mark)
  for hy, xlo in pairs(minx) do
    local xhi = maxx[hy]
    if xhi ~= nil then
      for x = xlo, xhi do
        set_half_color(ctx, layer, x, hy, col)
      end
    end
  end
end

-- -----------------------------------------------------------------------------
-- Preview state machine (like `font.lua`)
-- -----------------------------------------------------------------------------

local active = nil

local function clear_active(ctx, layer, also_clear_selection)
  if active ~= nil and active.backup ~= nil and layer ~= nil then
    restore_backup(layer, active.backup)
  end
  active = nil
  if also_clear_selection and ctx and ctx.canvas ~= nil then
    ctx.canvas:clearSelection()
  end
end

local function compute_backup_rect(ctx, x0, y0, x1, y1, resolution)
  local cols = tonumber((ctx and ctx.cols) or 0) or 0
  local rows = tonumber((ctx and ctx.rows) or 0) or 0
  local p = ctx.params or {}
  local size = tonumber(p.size) or 1
  local r = math.floor(math.max(1, size) / 2)

  local sx, dx = reorientate(x0, x1)
  local sy, dy = reorientate(y0, y1)
  if resolution == "half" then
    -- y0/y1 are half-y; convert to cell rows for backup.
    sy = math.floor(sy / 2)
    dy = math.floor(dy / 2)
  end

  sx = sx - r
  sy = sy - r
  dx = dx + r
  dy = dy + r

  if cols > 0 then
    sx = clamp_int(sx, 0, cols - 1)
    dx = clamp_int(dx, 0, cols - 1)
  else
    if sx < 0 then sx = 0 end
    if dx < 0 then dx = 0 end
  end
  if rows > 0 then
    sy = clamp_int(sy, 0, rows - 1)
    dy = clamp_int(dy, 0, rows - 1)
  else
    if sy < 0 then sy = 0 end
    if dy < 0 then dy = 0 end
  end

  local w = dx - sx + 1
  local h = dy - sy + 1
  return sx, sy, w, h
end

local function redraw_preview(ctx, layer)
  if active == nil or not ctx or not layer then return end

  if active.preview == true and active.backup ~= nil then
    restore_backup(layer, active.backup)
  end

  local p = ctx.params or {}
  local resolution = p.resolution
  if type(resolution) ~= "string" then resolution = "cell" end

  -- Compute constrained draw endpoints first, so backup/selection always matches what we draw.
  local shape = p.shape
  if type(shape) ~= "string" then shape = "line" end
  local fill = p.fill
  if type(fill) ~= "string" then fill = "outline" end
  local style = p.style
  if type(style) ~= "string" then style = "brush" end

  local mods = ctx.mods or {}
  local constrain = (mods.shift == true)

  local draw_sx = active.sx
  local draw_sy = active.sy
  local draw_ex = active.ex
  local draw_ey = active.ey

  if constrain and shape == "line" then
    local dx = draw_ex - draw_sx
    local dy = draw_ey - draw_sy
    local adx = math.abs(dx)
    local ady = math.abs(dy)
    if adx > ady then
      dy = (dy < 0) and -adx or adx
    else
      dx = (dx < 0) and -ady or ady
    end
    draw_ex = draw_sx + dx
    draw_ey = draw_sy + dy
  end

  if constrain and (shape == "rectangle" or shape == "ellipse") then
    local dx = draw_ex - draw_sx
    local dy = draw_ey - draw_sy
    local s = math.max(math.abs(dx), math.abs(dy))
    draw_ex = draw_sx + ((dx < 0) and -s or s)
    draw_ey = draw_sy + ((dy < 0) and -s or s)
  end

  active.draw_sx = draw_sx
  active.draw_sy = draw_sy
  active.draw_ex = draw_ex
  active.draw_ey = draw_ey

  local bx, by, bw, bh = compute_backup_rect(ctx, draw_sx, draw_sy, draw_ex, draw_ey, resolution)
  local backup = capture_backup(ctx, layer, bx, by, bw, bh)
  active.backup = backup
  active.preview = (backup ~= nil)
  active.bx = bx; active.by = by; active.bw = bw; active.bh = bh

  local st = style_pack(style)
  local brush = ctx.glyph
  if type(brush) ~= "string" or #brush == 0 then brush = " " end
  if st ~= nil and st.fill ~= nil and fill == "filled" and p.mode == "char" then
    brush = st.fill
  end

  local secondary = (active.button == "right")
  local fg, bg = get_paint(ctx, secondary)

  if resolution == "half" then
    -- Half mode paints with a single color into half blocks.
    local useFg = (p.useFg ~= false)
    local useBg = (p.useBg ~= false)
    -- IMPORTANT: like Pencil half mode, right-click selects BG (no fg/bg swap semantics here).
    local fg0 = (useFg and type(ctx.fg) == "number") and math.floor(ctx.fg) or nil
    local bg0 = (useBg and type(ctx.bg) == "number") and math.floor(ctx.bg) or nil
    local primary_col = useFg and fg0 or (useBg and bg0 or nil)
    local secondary_col = useBg and bg0 or (useFg and fg0 or nil)
    local col = secondary and secondary_col or primary_col
    if type(col) ~= "number" then return end

    local x0 = draw_sx
    local y0 = draw_sy
    local x1 = draw_ex
    local y1 = draw_ey

    if shape == "line" then
      draw_line_half(ctx, layer, x0, y0, x1, y1, col)
    elseif shape == "rectangle" then
      if fill == "filled" then
        draw_rect_filled_half(ctx, layer, x0, y0, x1, y1, col)
      else
        draw_rect_outline_half(ctx, layer, x0, y0, x1, y1, col)
      end
    elseif shape == "ellipse" then
      if fill == "filled" then
        draw_ellipse_filled_half(ctx, layer, x0, y0, x1, y1, col)
      else
        draw_ellipse_outline_half(ctx, layer, x0, y0, x1, y1, col)
      end
    elseif shape == "triangle" then
      local ax, ay, bx2, by2, cx2, cy2 = tri_vertices_from_drag(x0, y0, x1, y1, constrain)
      if fill == "filled" then
        fill_triangle_half(ctx, layer, ax, ay, bx2, by2, cx2, cy2, col)
      else
        draw_triangle_outline_half(ctx, layer, ax, ay, bx2, by2, cx2, cy2, col)
      end
    end
  else
    -- Cell mode.
    local x0 = draw_sx
    local y0 = draw_sy
    local x1 = draw_ex
    local y1 = draw_ey

    if shape == "line" then
      local glyph = brush
      if st ~= nil and p.mode == "char" then
        if y0 == y1 and st.h ~= nil then glyph = st.h end
        if x0 == x1 and st.v ~= nil then glyph = st.v end
      end
      draw_line_cell(ctx, layer, x0, y0, x1, y1, glyph, fg, bg)
    elseif shape == "rectangle" then
      if fill == "filled" then
        draw_rect_filled_cell(ctx, layer, x0, y0, x1, y1, brush, fg, bg)
      else
        draw_rect_outline_cell(ctx, layer, x0, y0, x1, y1, st, brush, fg, bg)
      end
    elseif shape == "ellipse" then
      if fill == "filled" then
        draw_ellipse_filled_cell(ctx, layer, x0, y0, x1, y1, brush, fg, bg)
      else
        draw_ellipse_outline_cell(ctx, layer, x0, y0, x1, y1, brush, fg, bg)
      end
    elseif shape == "triangle" then
      local ax, ay, bx2, by2, cx2, cy2 = tri_vertices_from_drag(x0, y0, x1, y1, constrain)
      if fill == "filled" then
        fill_triangle_cell(ctx, layer, ax, ay, bx2, by2, cx2, cy2, brush, fg, bg)
      else
        draw_triangle_outline_cell(ctx, layer, ax, ay, bx2, by2, cx2, cy2, brush, fg, bg)
      end
    end
  end

  -- Select result region (like font tool), if requested.
  if (p.selectAfter == true) and ctx.canvas ~= nil then
    local x_min, x_max = reorientate(draw_sx, draw_ex)
    local y_min, y_max = reorientate(draw_sy, draw_ey)
    if resolution == "half" then
      y_min = math.floor(y_min / 2)
      y_max = math.floor(y_max / 2)
    end
    ctx.canvas:setSelection(x_min, y_min, x_max, y_max)
  end
end

-- -----------------------------------------------------------------------------
-- Tool entrypoint
-- -----------------------------------------------------------------------------

function render(ctx, layer)
  if not ctx or not layer then return end
  if not ctx.focused then return end

  local phase = to_int(ctx.phase, 0)
  local keys = ctx.keys or {}
  local cursor = ctx.cursor or {}

  -- Cancel preview (works even when dragging).
  if keys.escape == true then
    clear_active(ctx, layer, true)
    return
  end

  if phase ~= 1 then
    return
  end

  if type(cursor) ~= "table" or cursor.valid ~= true then return end

  local p = ctx.params or {}
  local resolution = p.resolution
  if type(resolution) ~= "string" then resolution = "cell" end

  local x = to_int(cursor.x, 0)
  local y = to_int(cursor.y, 0)
  local hy = to_int(cursor.half_y, y * 2)

  local prev = cursor.p or {}
  local left = (cursor.left == true)
  local right = (cursor.right == true)
  local prev_left = (prev.left == true)
  local prev_right = (prev.right == true)

  local press_left = left and not prev_left
  local press_right = right and not prev_right
  local release_left = (not left) and prev_left
  local release_right = (not right) and prev_right

  local any_down = left or right
  local any_release = release_left or release_right
  local pressed = press_left or press_right

  if pressed then
    -- Start a new shape preview.
    clear_active(ctx, layer, false)
    active = {
      preview = true,
      backup = nil,
      button = press_right and "right" or "left",
    }
    if resolution == "half" then
      active.sx = x
      active.sy = hy
      active.ex = x
      active.ey = hy
    else
      active.sx = x
      active.sy = y
      active.ex = x
      active.ey = y
    end
    redraw_preview(ctx, layer)
    return
  end

  if active ~= nil and any_down then
    -- Update preview while dragging.
    local moved = false
    if resolution == "half" then
      if active.ex ~= x or active.ey ~= hy then
        active.ex = x
        active.ey = hy
        moved = true
      end
    else
      if active.ex ~= x or active.ey ~= y then
        active.ex = x
        active.ey = y
        moved = true
      end
    end
    if moved then
      redraw_preview(ctx, layer)
    end
    return
  end

  if active ~= nil and any_release then
    -- Commit: stop preview mode (do not restore backup anymore).
    active.preview = false
    active.backup = nil
    active = nil
    return
  end
end
  