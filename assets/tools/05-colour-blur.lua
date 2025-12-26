settings = {
  id = "11-colour-blur",
  icon = "🌈",
  label = "Colour Blur",

  -- Tool parameters (host renders UI; values are available under ctx.params.*)
  params = {
    size = { type = "int", label = "Size", ui = "slider", section = "Brush", primary = true, order = 0, min = 1, max = 31, step = 1, default = 7, width = 180 },
    flow = { type = "float", label = "Flow", ui = "slider", section = "Brush", primary = true, order = 1, min = 0.0, max = 1.0, step = 0.01, default = 0.65, inline = true, width = 180 },
    kernel = { type = "int", label = "Kernel", ui = "segmented", section = "Sampling", primary = true, order = 2, min = 1, max = 3, step = 1, default = 1, tooltip = "1=3x3, 2=5x5, 3=7x7", inline = true }, -- 1 => 3x3, 2 => 5x5, 3 => 7x7
    sample = { type = "enum", label = "Sample", ui = "segmented", section = "Sampling", primary = true, order = 3, items = { "composite", "layer" }, default = "composite", inline = true },

    affectChar = { type = "bool", label = "Char", ui = "toggle", section = "Affect", default = false },
    affectFg = { type = "bool", label = "FG", ui = "toggle", section = "Affect", default = true, inline = true },
    affectBg = { type = "bool", label = "BG", ui = "toggle", section = "Affect", default = true, inline = true },
    snapPalette = { type = "bool", label = "Snap to palette", ui = "toggle", section = "Affect", default = true, inline = true },
  },
}

-- IMPORTANT:
-- This file is executed in two very different contexts:
-- 1) Tool discovery (C++ ToolPalette): runs the script in a plain Lua state to read `settings`.
--    In that context, `require("ansl")` is NOT available, so we must not require it at top-level.
-- 2) Tool runtime (AnslScriptEngine): provides global `ansl` (native module) and calls render().
--
-- So: only access `ansl` lazily at runtime.
local function get_ansl()
  if type(_G) == "table" and type(_G.ansl) == "table" then
    return _G.ansl
  end
  local ok, mod = pcall(function() return require("ansl") end)
  if ok and type(mod) == "table" then
    return mod
  end
  return nil
end

local function clamp(v, a, b)
  if v < a then return a end
  if v > b then return b end
  return v
end

local function to_int(v, def)
  v = tonumber(v)
  if v == nil then return def end
  return math.floor(v)
end

local function norm_glyph(s)
  if type(s) ~= "string" or #s == 0 then return " " end
  return s
end

local function is_blank_glyph(s)
  s = norm_glyph(s)
  return s == " " or s == "\0" or s == " "
end

local function press_edge(cursor)
  if type(cursor) ~= "table" or cursor.valid ~= true then return false end
  local p = cursor.p or {}
  local l_edge = (cursor.left == true) and (p.left ~= true)
  local r_edge = (cursor.right == true) and (p.right ~= true)
  return l_edge or r_edge
end

local function moved_cell(cursor)
  if type(cursor) ~= "table" or cursor.valid ~= true then return false end
  local p = cursor.p or {}
  local px = to_int(p.x, to_int(cursor.x, 0))
  local py = to_int(p.y, to_int(cursor.y, 0))
  return (to_int(cursor.x, 0) ~= px) or (to_int(cursor.y, 0) ~= py)
end

local function any_down(cursor)
  if type(cursor) ~= "table" or cursor.valid ~= true then return false end
  return (cursor.left == true) or (cursor.right == true)
end

local function get_palette(ctx)
  if type(ctx) ~= "table" then return nil end
  local p = ctx.params or {}
  if p.snapPalette == false then return nil end
  local pal = ctx.palette
  if type(pal) ~= "table" then return nil end
  if #pal <= 0 then return nil end
  return pal
end

local function idx_to_rgb(idx)
  if type(idx) ~= "number" then return 0, 0, 0, false end
  local a = get_ansl()
  if not a or not a.colour or not a.colour.rgb_of then return 0, 0, 0, false end
  local r, g, b = a.colour.rgb_of(idx)
  return tonumber(r) or 0, tonumber(g) or 0, tonumber(b) or 0, true
end

local function snap_rgb_to_palette(r, g, b, pal)
  r = clamp(to_int(r, 0), 0, 255)
  g = clamp(to_int(g, 0), 0, 255)
  b = clamp(to_int(b, 0), 0, 255)

  local a = get_ansl()
  if not a or not a.colour or not a.colour.rgb then
    return nil
  end

  -- If no palette is provided, let the host pick the closest index.
  if type(pal) ~= "table" or #pal == 0 then
    return a.colour.rgb(r, g, b)
  end

  local best_idx = nil
  local best_d2 = nil
  for i = 1, #pal do
    local idx = pal[i]
    if type(idx) == "number" then
      local pr, pg, pb, ok = idx_to_rgb(idx)
      if ok then
        local dr = pr - r
        local dg = pg - g
        local db = pb - b
        local d2 = dr * dr + dg * dg + db * db
        if best_d2 == nil or d2 < best_d2 then
          best_d2 = d2
          best_idx = idx
        end
      end
    end
  end

  if best_idx == nil then
    return a.colour.rgb(r, g, b)
  end
  return best_idx
end

local function lerp(a, b, t)
  return a + (b - a) * t
end

local function lerp_rgb(r0, g0, b0, r1, g1, b1, t)
  return lerp(r0, r1, t), lerp(g0, g1, t), lerp(b0, b1, t)
end

local function sample_at(ctx, layer, x, y, sample_mode)
  local canvas = ctx and ctx.canvas or nil
  if canvas and canvas.getCell then
    return canvas:getCell(x, y, sample_mode)
  end
  return layer:get(x, y)
end

local function paint_cell(layer, x, y, ch, fg, bg)
  if fg == nil and bg == nil then
    layer:set(x, y, ch)
  else
    layer:set(x, y, ch, fg, bg)
  end
end

local function resolve_channel(tr, tg, tb, cur_idx, flow, pal)
  if flow >= 0.999 then
    return snap_rgb_to_palette(tr, tg, tb, pal)
  end

  -- Blend against current colour if present; otherwise blend against target itself (no-op).
  local cr, cg, cb, ok = idx_to_rgb(cur_idx)
  if not ok then
    cr, cg, cb = tr, tg, tb
  end
  local rr, rg, rb = lerp_rgb(cr, cg, cb, tr, tg, tb, flow)
  return snap_rgb_to_palette(rr, rg, rb, pal)
end

-- Persistent stroke state (Lua state persists across frames).
local stroke = {
  active = false,
  last_x = 0,
  last_y = 0,
}

local function end_stroke()
  stroke.active = false
end

local function colour_blur_step(ctx, layer, x, y)
  local cols = tonumber(ctx.cols) or 0
  if cols <= 0 then return end
  if x < 0 or x >= cols then return end
  if y < 0 then return end

  local p = ctx.params or {}
  local size = clamp(to_int(p.size, 7), 1, 200)
  local r = math.floor(size / 2)
  local flow = tonumber(p.flow) or 0.65
  flow = clamp(flow, 0.0, 1.0)
  if flow <= 0.0001 then return end

  local k = clamp(to_int(p.kernel, 1), 1, 6)
  local sample_mode = (type(p.sample) == "string") and p.sample or "composite"
  local affectChar = (p.affectChar == true)
  local affectFg = (p.affectFg ~= false)
  local affectBg = (p.affectBg ~= false)
  local pal = get_palette(ctx)

  for dy = -r, r do
    for dx = -r, r do
      local tx = x + dx
      local ty = y + dy
      if tx >= 0 and tx < cols and ty >= 0 then
        local fg_sum_r, fg_sum_g, fg_sum_b, fg_w = 0.0, 0.0, 0.0, 0.0
        local bg_sum_r, bg_sum_g, bg_sum_b, bg_w = 0.0, 0.0, 0.0, 0.0
        local best_ch = nil
        local best_count = 0
        local counts = {}

        for ny = -k, k do
          for nx = -k, k do
            local ch, fg, bg = sample_at(ctx, layer, tx + nx, ty + ny, sample_mode)
            ch = norm_glyph(ch)

            if affectChar then
              counts[ch] = (counts[ch] or 0) + 1
              if counts[ch] > best_count then
                best_count = counts[ch]
                best_ch = ch
              end
            end

            if affectFg and type(fg) == "number" then
              local rr, gg, bb, ok = idx_to_rgb(fg)
              if ok then
                fg_sum_r = fg_sum_r + rr
                fg_sum_g = fg_sum_g + gg
                fg_sum_b = fg_sum_b + bb
                fg_w = fg_w + 1.0
              end
            end
            if affectBg and type(bg) == "number" then
              local rr, gg, bb, ok = idx_to_rgb(bg)
              if ok then
                bg_sum_r = bg_sum_r + rr
                bg_sum_g = bg_sum_g + gg
                bg_sum_b = bg_sum_b + bb
                bg_w = bg_w + 1.0
              end
            end
          end
        end

        local cur_ch, cur_fg, cur_bg = layer:get(tx, ty)
        cur_ch = norm_glyph(cur_ch)
        if type(cur_fg) ~= "number" then cur_fg = nil end
        if type(cur_bg) ~= "number" then cur_bg = nil end

        local out_ch = affectChar and (best_ch or cur_ch) or cur_ch
        local out_fg = cur_fg
        local out_bg = cur_bg

        if affectFg and fg_w > 0.0 then
          local tr = fg_sum_r / fg_w
          local tg = fg_sum_g / fg_w
          local tb = fg_sum_b / fg_w
          out_fg = resolve_channel(tr, tg, tb, cur_fg, flow, pal)
        end
        if affectBg and bg_w > 0.0 then
          local tr = bg_sum_r / bg_w
          local tg = bg_sum_g / bg_w
          local tb = bg_sum_b / bg_w
          out_bg = resolve_channel(tr, tg, tb, cur_bg, flow, pal)
        end

        -- Avoid writing "blank majority" glyph unless requested.
        if affectChar and is_blank_glyph(out_ch) then
          out_ch = cur_ch
        end

        paint_cell(layer, tx, ty, out_ch, out_fg, out_bg)
      end
    end
  end
end

function render(ctx, layer)
  if type(ctx) ~= "table" or not layer then return end
  if not ctx.focused then
    end_stroke()
    return
  end

  local cols = tonumber(ctx.cols) or 0
  if cols <= 0 then
    end_stroke()
    return
  end

  local caret = ctx.caret
  if type(caret) ~= "table" then
    end_stroke()
    return
  end

  caret.x = clamp(to_int(caret.x, 0), 0, cols - 1)
  caret.y = math.max(0, to_int(caret.y, 0))

  -- Cancel stroke (Escape / tool cancel hotkey).
  local keys = ctx.keys or {}
  local hotkeys = ctx.hotkeys or {}
  if keys.escape == true or hotkeys.cancel == true then
    end_stroke()
    return
  end

  local phase = to_int(ctx.phase, 0)

  -- Brush size preview (host overlay; transient).
  do
    local p = ctx.params or {}
    local size = clamp(to_int(p.size, 7), 1, 200)
    local r = math.floor(size / 2)
    if ctx.out ~= nil then
      ctx.out[#ctx.out + 1] = { type = "brush.preview", anchor = "cursor", rx = r, ry = r }
    end
  end

  if phase ~= 1 then
    -- Mouse-only for now.
    if not any_down(ctx.cursor or {}) then
      end_stroke()
    end
    return
  end

  local cursor = ctx.cursor or {}
  if cursor.valid ~= true then
    end_stroke()
    return
  end

  if not any_down(cursor) then
    end_stroke()
    return
  end

  local x1 = to_int(cursor.x, caret.x)
  local y1 = to_int(cursor.y, caret.y)
  x1 = clamp(x1, 0, cols - 1)
  y1 = math.max(0, y1)

  caret.x = x1
  caret.y = y1

  local edge = press_edge(cursor)
  if edge or (not stroke.active) then
    stroke.active = true
    stroke.last_x = caret.x
    stroke.last_y = caret.y
    colour_blur_step(ctx, layer, caret.x, caret.y)
    return
  end

  if not moved_cell(cursor) then
    return
  end

  -- Interpolate between previous and current cell to avoid gaps.
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

  bresenham(stroke.last_x, stroke.last_y, caret.x, caret.y, function(ix, iy)
    ix = clamp(ix, 0, cols - 1)
    if iy < 0 then iy = 0 end
    colour_blur_step(ctx, layer, ix, iy)
  end)

  stroke.last_x = caret.x
  stroke.last_y = caret.y
end


