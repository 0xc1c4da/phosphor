settings = {
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

local function paint(ctx, layer, x, y)
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

  -- Right-click: swap fg/bg for non-shade modes (icy-draw style).
  if secondary and mode ~= "shade" and fg ~= nil and bg ~= nil then
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
      ch = secondary and "▄" or "▀"
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
      local px = tonumber(cursor.p and cursor.p.x) or tonumber(cursor.x) or caret.x
      local py = tonumber(cursor.p and cursor.p.y) or tonumber(cursor.y) or caret.y
      local prev_left = (cursor.p and cursor.p.left) == true
      local prev_right = (cursor.p and cursor.p.right) == true

      -- IMPORTANT: tools are executed every UI frame while dragging.
      -- To avoid "shade" instantly ramping to █ while the cursor sits still,
      -- only paint when:
      -- - we entered a new cell, or
      -- - the button transitioned from up->down (press edge)
      local moved_cell = (tonumber(cursor.x) ~= px) or (tonumber(cursor.y) ~= py)
      local pressed_edge = (cursor.left and not prev_left) or (cursor.right and not prev_right)

      if moved_cell or pressed_edge then
        caret.x = clamp(tonumber(cursor.x) or caret.x, 0, cols - 1)
        caret.y = math.max(0, tonumber(cursor.y) or caret.y)
        paint(ctx, layer, caret.x, caret.y)
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
