settings = {
  id = "fill",
  icon = "🪣",
  label = "Fill",

  -- Tool parameters (host renders UI; values are available under ctx.params.*)
  params = {
    mode = { type = "enum", label = "Mode", items = { "char", "colorize", "both" }, default = "both" },
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

local function fill(ctx, layer, sx, sy)
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
  if secondary and fg ~= nil and bg ~= nil then
    fg, bg = bg, fg
  end

  local brush = ctx.brush
  if type(brush) ~= "string" or #brush == 0 then brush = " " end

  -- Read start cell (glyph + optional fg/bg).
  local base_ch, base_fg, base_bg = layer:get(sx, sy)
  local base = norm_glyph(base_ch)

  -- If we can't change anything, bail early.
  if mode == "colorize" then
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
        fill(ctx, layer, caret.x, caret.y)
      end
    end
    return
  end
end

