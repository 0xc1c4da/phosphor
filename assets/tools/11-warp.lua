settings = {
  id = "11-warp",
  icon = "⟿",
  label = "Warp",

  -- Warp Transform (cell-grid analogue of GIMP's Warp Transform tool).
  --
  -- Model:
  -- - We capture a source region (selection if present; else full allocated canvas).
  -- - We maintain a per-cell displacement field u(x,y) = (dx,dy) in cell units.
  -- - Strokes update u() using a brush (size/hardness/strength + behavior).
  -- - Preview applies the warp by sampling the captured source at (x+dx, y+dy) (nearest).
  --
  -- UX:
  -- - Hold mouse button and drag to paint the warp.
  -- - ENTER commits (ends the warp session).
  -- - ESC cancels (restores original region) and ends the session.
  --
  -- Notes:
  -- - This is implemented entirely in Lua and is intended for modest regions.
  --   Prefer using a selection for large canvases.
  params = {
    behavior = {
      type = "enum",
      label = "Behavior",
      items = { "move", "grow", "shrink", "swirl_cw", "swirl_ccw", "erase", "smooth" },
      default = "move",
      order = 0,
    },
    scope = {
      type = "enum",
      label = "Scope",
      items = { "selection_if_any", "selection_only", "active_layer" },
      default = "selection_if_any",
      order = 1,
    },

    size = { type = "int", label = "Size", min = 1, max = 61, step = 1, default = 9, order = 10 },
    hardness = { type = "int", label = "Hardness", min = 0, max = 100, step = 1, default = 60, order = 11 },
    strength = { type = "float", label = "Strength", min = 0.0, max = 1.0, step = 0.01, default = 0.40, order = 12 },

    strokeSpacing = { type = "float", label = "Spacing (%)", min = 1.0, max = 100.0, step = 1.0, default = 15.0, order = 20 },
    strokeDuringMotion = { type = "bool", label = "Stroke during motion", default = true, order = 21 },
    strokePeriodically = { type = "bool", label = "Stroke periodically", default = false, order = 22 },
    strokeRate = { type = "float", label = "Periodic rate (%)", min = 1.0, max = 100.0, step = 1.0, default = 50.0, order = 23, sameLine = true },

    realTimePreview = { type = "bool", label = "Real-time preview", default = true, order = 30 },
  },

  -- Optional keybinding actions (host may expose these in Settings -> Key Bindings).
  actions = {
    { id = "warp.commit", title = "Warp: Commit", category = "Tool" },
    { id = "warp.cancel", title = "Warp: Cancel", category = "Tool" },
  },
}

-- IMPORTANT:
-- This file is executed in two contexts:
-- 1) Tool discovery (plain Lua; no native `ansl` module)
-- 2) Tool runtime (AnslScriptEngine; provides ctx + layer + ctx.canvas userdata)
-- Keep top-level free of require("ansl").

local function is_table(t) return type(t) == "table" end

local function to_int(v, def)
  v = tonumber(v)
  if v == nil then return def end
  return math.floor(v)
end

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

local function norm_glyph(s)
  if type(s) ~= "string" or #s == 0 then return " " end
  return s
end

local function press_edge(cursor, which)
  if not is_table(cursor) or cursor.valid ~= true then return false end
  local p = cursor.p or {}
  if which == "left" then
    return (cursor.left == true) and (p.left ~= true)
  end
  if which == "right" then
    return (cursor.right == true) and (p.right ~= true)
  end
  return false
end

local function any_down(cursor)
  if not is_table(cursor) or cursor.valid ~= true then return false end
  return (cursor.left == true) or (cursor.right == true)
end

local function moved_cell(cursor)
  if not is_table(cursor) or cursor.valid ~= true then return false end
  local p = cursor.p or {}
  local px = to_int(p.x, to_int(cursor.x, 0))
  local py = to_int(p.y, to_int(cursor.y, 0))
  return (to_int(cursor.x, 0) ~= px) or (to_int(cursor.y, 0) ~= py)
end

local function dist2(ax, ay, bx, by)
  local dx = ax - bx
  local dy = ay - by
  return dx * dx + dy * dy
end

-- Persistent warp session state (Lua state persists across frames).
local session = {
  active = false,

  -- Bounds in CANVAS coords:
  x0 = 0,
  y0 = 0,
  w = 0,
  h = 0,

  -- Captured source cells (row-major): {ch=string, fg=number|nil, bg=number|nil}
  src = nil,
  -- Current preview buffer (same format as src).
  preview = nil,
  -- Displacement field arrays (row-major): floats in cell units.
  dx = nil,
  dy = nil,

  -- Stroke state
  last_x = 0.0,
  last_y = 0.0,
  total_dist = 0.0,
  last_stroke_time_ms = nil,
}

local function idx_of(w, x, y)
  return (y * w) + x + 1
end

local function clear_session()
  session.active = false
  session.src = nil
  session.preview = nil
  session.dx = nil
  session.dy = nil
  session.w = 0
  session.h = 0
  session.total_dist = 0.0
  session.last_stroke_time_ms = nil
end

local function get_bounds(ctx, canvas)
  local cols = to_int(ctx.cols, 0)
  local rows = to_int(ctx.rows, 0)
  if cols <= 0 then return nil end
  if rows <= 0 then rows = 1 end

  local p = ctx.params or {}
  local scope = p.scope
  if type(scope) ~= "string" then scope = "selection_if_any" end

  local has_sel = (canvas and canvas.hasSelection and canvas:hasSelection()) or false
  if scope == "selection_only" then
    if not has_sel then return nil end
  end

  if has_sel and (scope == "selection_if_any" or scope == "selection_only") then
    local x, y, w, h = canvas:getSelection()
    x = to_int(x, 0); y = to_int(y, 0); w = to_int(w, 0); h = to_int(h, 0)
    if w <= 0 or h <= 0 then return nil end
    -- Clamp to canvas extents (rows are allocated; y can exceed but selection should be valid).
    x = clamp(x, 0, cols - 1)
    if y < 0 then y = 0 end
    if x + w > cols then w = cols - x end
    if w <= 0 then return nil end
    if h <= 0 then return nil end
    return x, y, w, h
  end

  -- active_layer: use full allocated canvas area.
  return 0, 0, cols, rows
end

local function capture_source(layer, x0, y0, w, h)
  local src = {}
  local i = 1
  for yy = 0, h - 1 do
    for xx = 0, w - 1 do
      local ch, fg, bg = layer:get(x0 + xx, y0 + yy)
      ch = norm_glyph(ch)
      if type(fg) ~= "number" then fg = nil end
      if type(bg) ~= "number" then bg = nil end
      src[i] = { ch = ch, fg = fg, bg = bg }
      i = i + 1
    end
  end
  return src
end

local function clone_cells(src)
  if type(src) ~= "table" then return {} end
  local dst = {}
  for i = 1, #src do
    local c = src[i]
    if type(c) == "table" then
      dst[i] = { ch = c.ch, fg = c.fg, bg = c.bg }
    else
      dst[i] = { ch = " ", fg = nil, bg = nil }
    end
  end
  return dst
end

local function init_field(w, h)
  local n = w * h
  local dx = {}
  local dy = {}
  for i = 1, n do
    dx[i] = 0.0
    dy[i] = 0.0
  end
  return dx, dy
end

local function apply_cell(layer, x, y, cell)
  if not cell then return end
  local ch = norm_glyph(cell.ch)
  local fg = cell.fg
  local bg = cell.bg
  if type(fg) ~= "number" then fg = nil end
  if type(bg) ~= "number" then bg = nil end

  if fg == nil and bg == nil then
    layer:set(x, y, ch)
    layer:clearStyle(x, y)
    return
  end

  layer:set(x, y, ch, fg, bg)
end

local function apply_rect_to_layer(layer, x0, y0, w, h, cells, rx0, ry0, rx1, ry1)
  -- rect is in LOCAL coords inclusive.
  rx0 = clamp(rx0, 0, w - 1)
  ry0 = clamp(ry0, 0, h - 1)
  rx1 = clamp(rx1, 0, w - 1)
  ry1 = clamp(ry1, 0, h - 1)
  if rx1 < rx0 or ry1 < ry0 then return end

  for yy = ry0, ry1 do
    for xx = rx0, rx1 do
      local i = idx_of(w, xx, yy)
      apply_cell(layer, x0 + xx, y0 + yy, cells[i])
    end
  end
end

local function update_preview_rect(layer, rx0, ry0, rx1, ry1)
  if not session.active then return end
  local w = session.w
  local h = session.h
  local src = session.src
  local prev = session.preview
  local dx = session.dx
  local dy = session.dy
  if not (w > 0 and h > 0 and src and prev and dx and dy) then return end

  rx0 = clamp(rx0, 0, w - 1)
  ry0 = clamp(ry0, 0, h - 1)
  rx1 = clamp(rx1, 0, w - 1)
  ry1 = clamp(ry1, 0, h - 1)
  if rx1 < rx0 or ry1 < ry0 then return end

  for yy = ry0, ry1 do
    for xx = rx0, rx1 do
      local di = idx_of(w, xx, yy)
      local sx = xx + (tonumber(dx[di]) or 0.0)
      local sy = yy + (tonumber(dy[di]) or 0.0)
      local ix = math.floor(sx + 0.5)
      local iy = math.floor(sy + 0.5)

      local cell
      if ix >= 0 and ix < w and iy >= 0 and iy < h then
        cell = src[idx_of(w, ix, iy)]
      end
      if type(cell) == "table" then
        prev[di] = { ch = cell.ch, fg = cell.fg, bg = cell.bg }
      else
        prev[di] = { ch = " ", fg = nil, bg = nil }
      end
    end
  end

  apply_rect_to_layer(layer, session.x0, session.y0, w, h, prev, rx0, ry0, rx1, ry1)
end

local function recompute_full_preview(layer)
  if not session.active then return end
  update_preview_rect(layer, 0, 0, session.w - 1, session.h - 1)
end

local function brush_weight(d, r, hardness01)
  if r <= 0.0001 then
    return (d <= 0.0001) and 1.0 or 0.0
  end
  if d >= r then
    return 0.0
  end
  local hr = r * clamp(hardness01, 0.0, 1.0)
  if d <= hr then
    return 1.0
  end
  local denom = (r - hr)
  if denom <= 0.0001 then
    return 0.0
  end
  local t = (d - hr) / denom -- 0..1
  local w = 1.0 - t
  return w * w
end

local function apply_brush_step(ctx, layer, cx, cy, dx_step, dy_step, do_preview)
  -- cx,cy are LOCAL float coordinates.
  local p = (type(ctx) == "table" and ctx.params) or {}
  local behavior = p.behavior
  if type(behavior) ~= "string" then behavior = "move" end

  local size = clamp(to_int(p.size, 9), 1, 9999)
  local hardness = clamp(to_int(p.hardness, 60), 0, 100) / 100.0
  local strength = tonumber(p.strength) or 0.40
  strength = clamp(strength, 0.0, 1.0)

  local r = size * 0.5
  local ir = math.ceil(r)

  local w = session.w
  local h = session.h
  local fx = session.dx
  local fy = session.dy
  if not (w > 0 and h > 0 and fx and fy) then return end

  local minx = math.floor(cx - r)
  local maxx = math.ceil(cx + r)
  local miny = math.floor(cy - r)
  local maxy = math.ceil(cy + r)
  if maxx < 0 or maxy < 0 or minx > w - 1 or miny > h - 1 then
    return
  end
  minx = clamp(minx, 0, w - 1)
  maxx = clamp(maxx, 0, w - 1)
  miny = clamp(miny, 0, h - 1)
  maxy = clamp(maxy, 0, h - 1)

  if behavior == "smooth" then
    -- Smooth uses a two-pass update for the brush rect to avoid in-place bias.
    local newdx = {}
    local newdy = {}
    for yy = miny, maxy do
      for xx = minx, maxx do
        local di = idx_of(w, xx, yy)
        local px = xx - cx
        local py = yy - cy
        local d = math.sqrt(px * px + py * py)
        local bw = brush_weight(d, r, hardness)
        if bw > 0.0 then
          local sumx, sumy, cnt = 0.0, 0.0, 0.0
          for ny = -1, 1 do
            for nx = -1, 1 do
              local sx = xx + nx
              local sy = yy + ny
              if sx >= 0 and sx < w and sy >= 0 and sy < h then
                local si = idx_of(w, sx, sy)
                sumx = sumx + (tonumber(fx[si]) or 0.0)
                sumy = sumy + (tonumber(fy[si]) or 0.0)
                cnt = cnt + 1.0
              end
            end
          end
          if cnt > 0.0 then
            local ax = sumx / cnt
            local ay = sumy / cnt
            local t = strength * bw
            local ox = tonumber(fx[di]) or 0.0
            local oy = tonumber(fy[di]) or 0.0
            newdx[di] = ox + (ax - ox) * t
            newdy[di] = oy + (ay - oy) * t
          end
        end
      end
    end
    for di, v in pairs(newdx) do fx[di] = v end
    for di, v in pairs(newdy) do fy[di] = v end
    if do_preview then
      update_preview_rect(layer, minx, miny, maxx, maxy)
    end
    return
  end

  for yy = miny, maxy do
    for xx = minx, maxx do
      local px = xx - cx
      local py = yy - cy
      local d = math.sqrt(px * px + py * py)
      local bw = brush_weight(d, r, hardness)
      if bw > 0.0 then
        local di = idx_of(w, xx, yy)
        local ux = tonumber(fx[di]) or 0.0
        local uy = tonumber(fy[di]) or 0.0
        local t = strength * bw

        if behavior == "move" then
          ux = ux + (dx_step * t)
          uy = uy + (dy_step * t)
        elseif behavior == "grow" or behavior == "shrink" then
          local nd = math.sqrt(px * px + py * py)
          if nd > 0.0001 then
            local nx = px / nd
            local ny = py / nd
            if behavior == "grow" then
              ux = ux + nx * t
              uy = uy + ny * t
            else
              ux = ux - nx * t
              uy = uy - ny * t
            end
          end
        elseif behavior == "swirl_cw" or behavior == "swirl_ccw" then
          local nd = math.sqrt(px * px + py * py)
          if nd > 0.0001 then
            local nx = px / nd
            local ny = py / nd
            -- Tangent vector (unit): CW = (ny, -nx), CCW = (-ny, nx)
            local tx, ty
            if behavior == "swirl_cw" then
              tx, ty = ny, -nx
            else
              tx, ty = -ny, nx
            end
            ux = ux + tx * t
            uy = uy + ty * t
          end
        elseif behavior == "erase" then
          local k = 1.0 - t
          if k < 0.0 then k = 0.0 end
          ux = ux * k
          uy = uy * k
        end

        fx[di] = ux
        fy[di] = uy
      end
    end
  end

  if do_preview then
    update_preview_rect(layer, minx, miny, maxx, maxy)
  end
end

local function begin_warp(ctx, layer)
  local canvas = ctx.canvas
  local bx, by, bw, bh = get_bounds(ctx, canvas)
  if not bx then
    return false
  end

  session.active = true
  session.x0 = bx
  session.y0 = by
  session.w = bw
  session.h = bh

  session.src = capture_source(layer, bx, by, bw, bh)
  session.preview = clone_cells(session.src)
  session.dx, session.dy = init_field(bw, bh)
  session.total_dist = 0.0
  session.last_stroke_time_ms = nil
  return true
end

local function cancel_warp(layer)
  if not session.active then return end
  apply_rect_to_layer(layer, session.x0, session.y0, session.w, session.h, session.src, 0, 0, session.w - 1, session.h - 1)
  clear_session()
end

local function commit_warp(ctx, layer)
  if not session.active then return end
  local p = ctx.params or {}
  if p.realTimePreview ~= true then
    -- If we weren't previewing live, apply once now.
    recompute_full_preview(layer)
  end
  clear_session()
end

local function local_cursor_pos(ctx, cols)
  local caret = ctx.caret or {}
  local cursor = ctx.cursor or {}
  local x = to_int(cursor.x, to_int(caret.x, 0))
  local y = to_int(cursor.y, to_int(caret.y, 0))
  x = clamp(x, 0, cols - 1)
  if y < 0 then y = 0 end
  return x, y
end

local function stroke_step_to(ctx, layer, x_canvas, y_canvas, is_first)
  if not session.active then return end

  -- Convert to local space.
  local lx = x_canvas - session.x0
  local ly = y_canvas - session.y0

  -- If outside active region, ignore.
  if lx < 0 or lx >= session.w or ly < 0 or ly >= session.h then
    return
  end

  local px = session.last_x
  local py = session.last_y

  local dx_step = (lx - px)
  local dy_step = (ly - py)
  if is_first then
    dx_step = 0.0
    dy_step = 0.0
  end

  session.last_x = lx
  session.last_y = ly
  local do_preview = ((ctx.params or {}).realTimePreview == true)
  apply_brush_step(ctx, layer, lx, ly, dx_step, dy_step, do_preview)
end

local function stroke_motion(ctx, layer, x_canvas, y_canvas)
  local p = ctx.params or {}
  local size = clamp(to_int(p.size, 9), 1, 9999)
  local spacing_pct = tonumber(p.strokeSpacing) or 15.0
  spacing_pct = clamp(spacing_pct, 1.0, 100.0)

  local step = (size * spacing_pct) / 100.0
  if step < 0.25 then step = 0.25 end

  local lx = x_canvas - session.x0
  local ly = y_canvas - session.y0
  if lx < 0 or lx >= session.w or ly < 0 or ly >= session.h then
    -- Don't update total_dist if outside.
    return
  end

  local ox = session.last_x
  local oy = session.last_y

  local dx = lx - ox
  local dy = ly - oy
  local dist = math.sqrt(dx * dx + dy * dy)
  if dist <= 0.0001 then
    return
  end

  local total = session.total_dist
  while total + dist >= step do
    local diff = step - total
    local t = diff / dist
    ox = ox + dx * t
    oy = oy + dy * t

    -- Remaining segment
    dx = lx - ox
    dy = ly - oy
    dist = math.sqrt(dx * dx + dy * dy)
    total = 0.0

    -- Step at (ox,oy) in local coords.
    stroke_step_to(ctx, layer, session.x0 + ox, session.y0 + oy, false)
  end
  session.total_dist = total + dist
end

local function stroke_periodic(ctx, layer, x_canvas, y_canvas)
  local p = ctx.params or {}
  if p.strokePeriodically ~= true then return end

  local rate = tonumber(p.strokeRate) or 50.0
  rate = clamp(rate, 1.0, 100.0)

  -- Match GIMP's max timer fps (20) scaled by rate%.
  local max_fps = 20.0
  local fps = max_fps * (rate / 100.0)
  if fps < 1.0 then fps = 1.0 end
  local period_ms = 1000.0 / fps

  local now = tonumber(ctx.time) or 0.0
  local last = session.last_stroke_time_ms
  if last == nil then
    session.last_stroke_time_ms = now
    return
  end

  if (now - last) >= period_ms then
    session.last_stroke_time_ms = now
    stroke_step_to(ctx, layer, x_canvas, y_canvas, false)
  end
end

function render(ctx, layer)
  if type(ctx) ~= "table" or not layer then return end

  local canvas = ctx.canvas
  if canvas == nil then return end

  local cols = to_int(ctx.cols, 0)
  if cols <= 0 then
    clear_session()
    return
  end

  local phase = to_int(ctx.phase, 0)
  local keys = ctx.keys or {}
  local hotkeys = ctx.hotkeys or {}
  local actions = ctx.actions or {}

  -- End/cancel session if tool loses focus (prevents "stuck" transforms if user clicks elsewhere).
  if ctx.focused ~= true then
    -- Don't destructively cancel; just stop updating.
    return
  end

  -- Phase 0: commit/cancel via keyboard/actions.
  if phase == 0 then
    if keys.escape == true or hotkeys.cancel == true or actions["warp.cancel"] == true then
      cancel_warp(layer)
      return
    end
    if keys.enter == true or actions["warp.commit"] == true then
      commit_warp(ctx, layer)
      return
    end
    return
  end

  -- Phase 1: mouse-driven strokes.
  if phase ~= 1 then return end
  local cursor = ctx.cursor or {}
  if cursor.valid ~= true then return end

  -- While active: keep caret tracking cursor (like other tools).
  local caret = ctx.caret or {}
  local x_canvas = clamp(to_int(cursor.x, to_int(caret.x, 0)), 0, cols - 1)
  local y_canvas = math.max(0, to_int(cursor.y, to_int(caret.y, 0)))
  caret.x = x_canvas
  caret.y = y_canvas

  if not any_down(cursor) then
    -- Reset periodic timer baseline when not dragging.
    session.last_stroke_time_ms = nil
    return
  end

  local pressed = press_edge(cursor, "left") or press_edge(cursor, "right")
  local moved = moved_cell(cursor)

  -- Start warp session on first press.
  if pressed and not session.active then
    if not begin_warp(ctx, layer) then
      return
    end
    session.last_x = x_canvas - session.x0
    session.last_y = y_canvas - session.y0
    session.total_dist = 0.0
    session.last_stroke_time_ms = tonumber(ctx.time) or 0.0

    -- Always record the first point; preview writes are gated inside stroke_step_to().
    stroke_step_to(ctx, layer, x_canvas, y_canvas, true)
    return
  end

  if not session.active then
    return
  end

  -- Determine whether to update during motion.
  local p = ctx.params or {}
  local behavior = p.behavior
  if type(behavior) ~= "string" then behavior = "move" end

  if p.strokeDuringMotion == true and moved then
    stroke_motion(ctx, layer, x_canvas, y_canvas)
  end

  -- Periodic strokes (typically used for non-move behaviors).
  -- Match the GIMP exception: don't periodic-stroke move+motion (it would double-hit).
  local allow_periodic = not (behavior == "move" and p.strokeDuringMotion == true)
  if allow_periodic then
    stroke_periodic(ctx, layer, x_canvas, y_canvas)
  end

  -- If we are not doing real-time preview, avoid writing every frame.
  if p.realTimePreview ~= true then
    -- No-op: we updated dx/dy and preview buffer only via stroke_step_to.
    -- This mode is mainly for performance; user commits with ENTER.
  end
end


