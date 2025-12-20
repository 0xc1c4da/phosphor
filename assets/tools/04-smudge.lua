settings = {
  id = "04-smudge",
  icon = "≋",
  label = "Smudge",

  -- Tool parameters (host renders UI; values are available under ctx.params.*)
  params = {
    size = { type = "int", label = "Size", min = 1, max = 25, step = 1, default = 3 },
    rate = { type = "float", label = "Rate", min = 0.0, max = 1.0, step = 0.01, default = 0.75 },
    flow = { type = "float", label = "Flow", min = 0.0, max = 1.0, step = 0.01, default = 0.65 },
    sample = { type = "enum", label = "Sample", items = { "composite", "layer" }, default = "composite" },
    mode = { type = "enum", label = "Mode", items = { "smudge", "blur" }, default = "smudge" },

    affectChar = { type = "bool", label = "Affect Char", default = true },
    affectFg = { type = "bool", label = "Affect FG", default = true },
    affectBg = { type = "bool", label = "Affect BG", default = true },
    snapPalette = { type = "bool", label = "Snap to palette", default = true },
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
  -- Best-effort fallback (in case runtime doesn't publish global `ansl` for some reason).
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
  if not a or not a.color or not a.color.rgb_of then return 0, 0, 0, false end
  local r, g, b = a.color.rgb_of(idx)
  return tonumber(r) or 0, tonumber(g) or 0, tonumber(b) or 0, true
end

local function snap_rgb_to_palette(r, g, b, pal)
  r = clamp(to_int(r, 0), 0, 255)
  g = clamp(to_int(g, 0), 0, 255)
  b = clamp(to_int(b, 0), 0, 255)

  local a = get_ansl()
  if not a or not a.color or not a.color.rgb then
    return nil
  end

  if type(pal) ~= "table" or #pal == 0 then
    return a.color.rgb(r, g, b)
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
    return a.color.rgb(r, g, b)
  end
  return best_idx
end

local function lerp(a, b, t)
  return a + (b - a) * t
end

local function lerp_rgb(r0, g0, b0, r1, g1, b1, t)
  return lerp(r0, r1, t), lerp(g0, g1, t), lerp(b0, b1, t)
end

local function sample_at(ctx, layer, x, y, mode)
  local canvas = ctx and ctx.canvas or nil
  if canvas and canvas.getCell then
    return canvas:getCell(x, y, mode)
  end
  -- Fallback: layer sample only (older hosts).
  return layer:get(x, y)
end

local function make_accum_cell()
  return {
    fg = { r = 0.0, g = 0.0, b = 0.0, w = 0.0 },
    bg = { r = 0.0, g = 0.0, b = 0.0, w = 0.0 },
    ch = " ",
    conf = 0.0,
  }
end

local function accum_from_sample(acc, ch, fg, bg)
  acc.ch = norm_glyph(ch)
  acc.conf = is_blank_glyph(acc.ch) and 0.0 or 1.0

  local function init_chan(dst, idx)
    if type(idx) == "number" then
      local r, g, b, ok = idx_to_rgb(idx)
      if ok then
        dst.r, dst.g, dst.b, dst.w = r, g, b, 1.0
        return
      end
    end
    dst.r, dst.g, dst.b, dst.w = 0.0, 0.0, 0.0, 0.0
  end

  init_chan(acc.fg, fg)
  init_chan(acc.bg, bg)
end

local function accum_mix_chan(dst, sr, sg, sb, sw, rate)
  -- dst = rate*dst + (1-rate)*sample
  local ir = tonumber(sr) or 0.0
  local ig = tonumber(sg) or 0.0
  local ib = tonumber(sb) or 0.0
  local iw = tonumber(sw) or 0.0
  dst.r = (dst.r * rate) + (ir * (1.0 - rate))
  dst.g = (dst.g * rate) + (ig * (1.0 - rate))
  dst.b = (dst.b * rate) + (ib * (1.0 - rate))
  dst.w = (dst.w * rate) + (iw * (1.0 - rate))
end

local function accum_mix_char(acc, sample_ch, rate)
  sample_ch = norm_glyph(sample_ch)

  -- Prefer dragging non-blank glyphs over empty space (smear behavior).
  local sample_blank = is_blank_glyph(sample_ch)
  local same = (sample_ch == acc.ch)

  if same then
    acc.conf = acc.conf + (1.0 - acc.conf) * (1.0 - rate)
    return
  end

  -- Different char: decay confidence.
  acc.conf = acc.conf * rate

  -- Adopt new glyph only if the old one is weak.
  -- Make it harder to adopt blanks so we don't quickly "erase" the smear.
  local adopt_thresh = sample_blank and 0.10 or 0.35
  if acc.conf < adopt_thresh then
    acc.ch = sample_ch
    acc.conf = sample_blank and 0.10 or 0.55
  end
end

local function paint_cell(layer, x, y, ch, fg, bg)
  if fg == nil and bg == nil then
    layer:set(x, y, ch)
  else
    layer:set(x, y, ch, fg, bg)
  end
end

local function resolve_channel(acc_chan, cur_idx, flow, pal)
  -- acc_chan: {r,g,b,w} where rgb is weighted sum, w in [0,1]
  local w = tonumber(acc_chan.w) or 0.0
  if w <= 0.001 then
    return nil
  end

  local tr = acc_chan.r / w
  local tg = acc_chan.g / w
  local tb = acc_chan.b / w

  if flow >= 0.999 then
    return snap_rgb_to_palette(tr, tg, tb, pal)
  end

  -- Blend against current color if present; otherwise blend against target itself (no-op).
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
  r = 1,
  size = 1,
  last_x = 0,
  last_y = 0,
  prev_x = 0,
  prev_y = 0,
  mode = "smudge",
  sample = "composite",
  accum = {}, -- flat array length (2r+1)^2
}

local function begin_stroke(ctx, layer, x, y)
  local p = ctx.params or {}
  local size = clamp(to_int(p.size, 3), 1, 200)
  local r = math.floor(size / 2)
  local rate = tonumber(p.rate) or 0.75
  rate = clamp(rate, 0.0, 1.0)

  stroke.active = true
  stroke.size = size
  stroke.r = r
  stroke.last_x = x
  stroke.last_y = y
  stroke.prev_x = x
  stroke.prev_y = y
  stroke.mode = (type(p.mode) == "string") and p.mode or "smudge"
  stroke.sample = (type(p.sample) == "string") and p.sample or "composite"
  stroke.accum = {}

  local diam = (r * 2) + 1
  for dy = -r, r do
    for dx = -r, r do
      local ch, fg, bg = sample_at(ctx, layer, x + dx, y + dy, stroke.sample)
      local acc = make_accum_cell()
      accum_from_sample(acc, ch, fg, bg)
      stroke.accum[(dy + r) * diam + (dx + r) + 1] = acc
    end
  end
end

local function end_stroke()
  stroke.active = false
  stroke.accum = {}
end

local function smudge_step(ctx, layer, x, y, from_x, from_y)
  local cols = tonumber(ctx.cols) or 0
  if cols <= 0 then return end
  if x < 0 or x >= cols then return end
  if y < 0 then return end

  local p = ctx.params or {}
  local flow = tonumber(p.flow) or 0.65
  flow = clamp(flow, 0.0, 1.0)
  if flow <= 0.0001 then return end

  local rate = tonumber(p.rate) or 0.75
  rate = clamp(rate, 0.0, 1.0)

  local affectChar = (p.affectChar ~= false)
  local affectFg = (p.affectFg ~= false)
  local affectBg = (p.affectBg ~= false)

  local pal = get_palette(ctx)

  local r = stroke.r
  local diam = (r * 2) + 1

  -- Directional smudge: shift the accumulator footprint by the movement delta, so
  -- contents "drag" along the stroke direction (more like classic paint smudge).
  if stroke.mode == "smudge" and type(from_x) == "number" and type(from_y) == "number" then
    local mdx = math.floor(x - from_x)
    local mdy = math.floor(y - from_y)
    if mdx ~= 0 or mdy ~= 0 then
      -- Clamp shift to brush radius so large jumps don't explode indexing.
      if mdx > r then mdx = r end
      if mdx < -r then mdx = -r end
      if mdy > r then mdy = r end
      if mdy < -r then mdy = -r end

      local new_acc = {}
      for dy = -r, r do
        for dx = -r, r do
          local src_dx = dx - mdx
          local src_dy = dy - mdy
          local dst_i = (dy + r) * diam + (dx + r) + 1
          if src_dx >= -r and src_dx <= r and src_dy >= -r and src_dy <= r then
            local src_i = (src_dy + r) * diam + (src_dx + r) + 1
            new_acc[dst_i] = stroke.accum[src_i]
          else
            -- Fill uncovered cells with fresh samples from the current position.
            local ch, fg, bg = sample_at(ctx, layer, x + dx, y + dy, stroke.sample)
            local acc = make_accum_cell()
            accum_from_sample(acc, ch, fg, bg)
            new_acc[dst_i] = acc
          end
        end
      end
      stroke.accum = new_acc
    end
  end

  -- Blur mode: per-step local neighborhood average (no direction / no persistent smear).
  if stroke.mode == "blur" then
    for dy = -r, r do
      for dx = -r, r do
        local tx = x + dx
        local ty = y + dy
        if tx >= 0 and tx < cols and ty >= 0 then
          -- Box average in a 3x3 neighborhood (fixed) for performance.
          local fg_sum_r, fg_sum_g, fg_sum_b, fg_w = 0.0, 0.0, 0.0, 0.0
          local bg_sum_r, bg_sum_g, bg_sum_b, bg_w = 0.0, 0.0, 0.0, 0.0
          local best_ch = nil
          local best_count = 0
          local counts = {}

          for ny = -1, 1 do
            for nx = -1, 1 do
              local ch, fg, bg = sample_at(ctx, layer, tx + nx, ty + ny, stroke.sample)
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
          local out_fg = nil
          local out_bg = nil
          if affectFg and fg_w > 0.0 then
            local tr = fg_sum_r / fg_w
            local tg = fg_sum_g / fg_w
            local tb = fg_sum_b / fg_w
            out_fg = resolve_channel({ r = tr, g = tg, b = tb, w = 1.0 }, cur_fg, flow, pal)
          else
            out_fg = cur_fg
          end
          if affectBg and bg_w > 0.0 then
            local tr = bg_sum_r / bg_w
            local tg = bg_sum_g / bg_w
            local tb = bg_sum_b / bg_w
            out_bg = resolve_channel({ r = tr, g = tg, b = tb, w = 1.0 }, cur_bg, flow, pal)
          else
            out_bg = cur_bg
          end

          paint_cell(layer, tx, ty, out_ch, out_fg, out_bg)
        end
      end
    end
    return
  end

  -- Smudge mode: GIMP-inspired accumulator under the brush footprint.
  for dy = -r, r do
    for dx = -r, r do
      local idx = (dy + r) * diam + (dx + r) + 1
      local acc = stroke.accum[idx]
      if acc then
        local ch, fg, bg = sample_at(ctx, layer, x + dx, y + dy, stroke.sample)
        ch = norm_glyph(ch)

        -- Mix in current sample (Accum = rate*Accum + (1-rate)*I).
        if type(fg) == "number" then
          local rr, gg, bb, ok = idx_to_rgb(fg)
          if ok then
            accum_mix_chan(acc.fg, rr, gg, bb, 1.0, rate)
          else
            accum_mix_chan(acc.fg, 0.0, 0.0, 0.0, 0.0, rate)
          end
        else
          accum_mix_chan(acc.fg, 0.0, 0.0, 0.0, 0.0, rate)
        end
        if type(bg) == "number" then
          local rr, gg, bb, ok = idx_to_rgb(bg)
          if ok then
            accum_mix_chan(acc.bg, rr, gg, bb, 1.0, rate)
          else
            accum_mix_chan(acc.bg, 0.0, 0.0, 0.0, 0.0, rate)
          end
        else
          accum_mix_chan(acc.bg, 0.0, 0.0, 0.0, 0.0, rate)
        end

        accum_mix_char(acc, ch, rate)
      end
    end
  end

  -- Apply paint: Paint = lerp(Current, Accum, flow) (text-mode analogue to GIMP flow).
  for dy = -r, r do
    for dx = -r, r do
      local tx = x + dx
      local ty = y + dy
      if tx >= 0 and tx < cols and ty >= 0 then
        local idx = (dy + r) * diam + (dx + r) + 1
        local acc = stroke.accum[idx]
        if acc then
          local cur_ch, cur_fg, cur_bg = layer:get(tx, ty)
          cur_ch = norm_glyph(cur_ch)
          if type(cur_fg) ~= "number" then cur_fg = nil end
          if type(cur_bg) ~= "number" then cur_bg = nil end

          local out_ch = cur_ch
          if affectChar then
            -- Lower threshold: smudging should visibly drag glyphs.
            if flow >= 0.25 then
              out_ch = acc.ch
            end
          end

          local out_fg = cur_fg
          local out_bg = cur_bg
          if affectFg then
            out_fg = resolve_channel(acc.fg, cur_fg, flow, pal)
          end
          if affectBg then
            out_bg = resolve_channel(acc.bg, cur_bg, flow, pal)
          end

          paint_cell(layer, tx, ty, out_ch, out_fg, out_bg)
        end
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
  if phase ~= 1 then
    -- Smudge is mouse-only for now.
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
    begin_stroke(ctx, layer, caret.x, caret.y)
    smudge_step(ctx, layer, caret.x, caret.y, caret.x, caret.y)
    stroke.last_x = caret.x
    stroke.last_y = caret.y
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
    local fx = stroke.prev_x
    local fy = stroke.prev_y
    if type(fx) ~= "number" then fx = ix end
    if type(fy) ~= "number" then fy = iy end
    ix = clamp(ix, 0, cols - 1)
    if iy < 0 then iy = 0 end
    smudge_step(ctx, layer, ix, iy, fx, fy)
    stroke.prev_x = ix
    stroke.prev_y = iy
  end)

  stroke.last_x = caret.x
  stroke.last_y = caret.y
end