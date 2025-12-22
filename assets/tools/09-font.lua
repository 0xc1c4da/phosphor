settings = {
  id = "09-font",
  icon = "𝔉",
  label = "Font",

  -- Tool parameters (host renders UI; values are available under ctx.params.*)
  params = {
    font = { type = "enum", label = "Font", ui = "combo_filter", section = "Font", primary = true, items = { "(no fonts)" }, default = "(no fonts)" },
    place = { type = "button", label = "Place (Enter)", ui = "action", section = "Actions", primary = true, inline = true },
    clearText = { type = "button", label = "Clear", ui = "action", section = "Actions", primary = true, inline = true },

    useFontColors = { type = "bool", label = "Use font colors", ui = "toggle", section = "Color", default = true },
    useFg = { type = "bool", label = "Fallback: Use FG", ui = "toggle", section = "Color", default = true, inline = true },
    useBg = { type = "bool", label = "Fallback: Use BG", ui = "toggle", section = "Color", default = false, inline = true },

    editMode = { type = "bool", label = "Edit markers (outline)", ui = "toggle", section = "Outline", default = false },
    outlineStyle = { type = "int", label = "Outline style", ui = "slider", section = "Outline", min = 0, max = 18, step = 1, default = 0, inline = true, width = 180 },
  },
}

local font_items = {}
local font_label_to_id = {}

local function rebuild_font_items()
  font_items = {}
  font_label_to_id = {}

  if ansl == nil or ansl.font == nil or ansl.font.list == nil then
    font_items[1] = "(no fonts)"
    font_label_to_id["(no fonts)"] = ""
    settings.params.font.items = font_items
    settings.params.font.default = font_items[1]
    return
  end

  local list = ansl.font.list() or {}
  local entries = {}
  for i = 1, #list do
    local e = list[i]
    if type(e) == "table" and type(e.label) == "string" and type(e.id) == "string" then
      local kind = e.kind
      if type(kind) ~= "string" then kind = "" end
      entries[#entries + 1] = { id = e.id, label = e.label, kind = kind }
    end
  end

  -- Sort so FIGlet fonts are easy to find (they're a minority vs TDF).
  table.sort(entries, function(a, b)
    local ak = (a.kind == "flf") and 0 or 1
    local bk = (b.kind == "flf") and 0 or 1
    if ak ~= bk then return ak < bk end
    return tostring(a.label) < tostring(b.label)
  end)

  local used = {}
  for i = 1, #entries do
    local e = entries[i]
    local label = e.label
    if used[label] ~= nil then
      used[label] = used[label] + 1
      label = label .. " (" .. tostring(used[label]) .. ")"
    else
      used[label] = 1
    end
    font_items[#font_items + 1] = label
    font_label_to_id[label] = e.id
  end

  if #font_items == 0 then
    font_items[1] = "(no fonts)"
    font_label_to_id["(no fonts)"] = ""
  end

  settings.params.font.items = font_items

  -- Prefer a FIGlet font as the default selection if one exists (otherwise first item).
  local def = font_items[1]
  for i = 1, #entries do
    if entries[i].kind == "flf" then
      def = font_items[i]
      break
    end
  end
  settings.params.font.default = def
end

rebuild_font_items()

local text_chars =
  (ansl and ansl.string and ansl.string.utf8chars and ansl.string.utf8chars("PHOSPHOR"))
  or { "P", "H", "O", "S", "P", "H", "O", "R" }

local function concat_chars(chars)
  if type(chars) ~= "table" then return "" end
  return table.concat(chars)
end

local function pop_char(chars)
  if type(chars) ~= "table" then return end
  if #chars <= 0 then return end
  chars[#chars] = nil
end

local function push_typed(chars, typed)
  if type(chars) ~= "table" then return end
  if type(typed) ~= "table" then return end
  for i = 1, #typed do
    local ch = typed[i]
    if type(ch) == "string" and #ch > 0 then
      if ch ~= "\n" and ch ~= "\r" then
        chars[#chars + 1] = ch
      end
    end
  end
end

local function current_font_id(ctx)
  local p = (ctx and ctx.params) or {}
  local label = p.font
  if type(label) ~= "string" then return "" end
  local id = font_label_to_id[label]
  if type(id) ~= "string" then return "" end
  return id
end

local function set_cell(layer, x, y, cp, fg, bg)
  -- Clear style if we don't have explicit colors.
  if fg == nil and bg == nil then
    layer:set(x, y, cp)
    layer:clearStyle(x, y)
  else
    layer:set(x, y, cp, fg, bg)
  end
end

local function clear_rect(layer, x, y, w, h)
  if not layer then return end
  w = tonumber(w) or 0
  h = tonumber(h) or 0
  if w <= 0 or h <= 0 then return end
  for j = 0, h - 1 do
    for i = 0, w - 1 do
      layer:set(x + i, y + j, " ")
      layer:clearStyle(x + i, y + j)
    end
  end
end

local function clamp_int(v, lo, hi)
  v = math.floor(tonumber(v) or 0)
  lo = math.floor(tonumber(lo) or 0)
  hi = math.floor(tonumber(hi) or 0)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

local function effective_rect(ctx, x, y, w, h)
  local cols = tonumber((ctx and ctx.cols) or 0) or 0
  x = math.floor(tonumber(x) or 0)
  y = math.floor(tonumber(y) or 0)
  w = math.floor(tonumber(w) or 0)
  h = math.floor(tonumber(h) or 0)
  if y < 0 then y = 0 end
  if w < 0 then w = 0 end
  if h < 0 then h = 0 end

  if cols > 0 then
    x = clamp_int(x, 0, cols - 1)
    local max_w = cols - x
    if max_w < 0 then max_w = 0 end
    if w > max_w then w = max_w end
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

  -- Safety: don't try to snapshot enormous regions (can freeze).
  local max_cells = 12000
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

local function stamp_bitmap(ctx, layer, x0, y0, bmp)
  if not ctx or not layer or type(bmp) ~= "table" then return false end
  -- Source bitmap dimensions/stride (DO NOT change these when clipping, or row-major indexing breaks).
  local src_w = tonumber(bmp.w) or 0
  local src_h = tonumber(bmp.h) or 0
  if src_w <= 0 or src_h <= 0 then return false end

  -- IMPORTANT (selection write clipping):
  -- The host now clips *tool-driven* writes to the current selection rect when a selection exists.
  -- So when we stamp/preview into a new location, or when rerendering produces a different size,
  -- we must update the selection bounds *before* writing any cells, otherwise the stamp will be
  -- cut off to the previous selection.
  local ox0 = math.floor(tonumber(x0) or 0)
  local oy0 = math.floor(tonumber(y0) or 0)
  local dx0, dy0, dw, dh = effective_rect(ctx, ox0, oy0, src_w, src_h)
  if dw <= 0 or dh <= 0 then return false end

  -- Map destination clip back into source bitmap coordinates.
  -- If effective_rect() clamped x/y up (e.g. negative origin), we skip those columns/rows in the source.
  local src_x0 = dx0 - ox0
  local src_y0 = dy0 - oy0
  if src_x0 < 0 then src_x0 = 0 end
  if src_y0 < 0 then src_y0 = 0 end

  local canvas = ctx.canvas
  if canvas ~= nil then
    canvas:setSelection(dx0, dy0, dx0 + dw - 1, dy0 + dh - 1)
    -- Read back actual clamped selection (x is clamped to columns by the host).
    local sx, sy, sw, sh = canvas:getSelection()
    if type(sx) == "number" and type(sy) == "number" and type(sw) == "number" and type(sh) == "number" then
      sx = math.floor(sx)
      sy = math.floor(sy)
      sw = math.floor(sw)
      sh = math.floor(sh)

      -- If selection was clamped further, adjust source offset accordingly.
      if sx ~= dx0 then src_x0 = src_x0 + (sx - dx0) end
      if sy ~= dy0 then src_y0 = src_y0 + (sy - dy0) end
      dx0 = sx
      dy0 = sy
      dw = sw
      dh = sh
    end
    if dw <= 0 or dh <= 0 then return false end
    if src_x0 < 0 then src_x0 = 0 end
    if src_y0 < 0 then src_y0 = 0 end
    if src_x0 >= src_w then return false end
    if src_y0 >= src_h then return false end
  end

  local cp = bmp.cp or {}
  local fg = bmp.fg or {}
  local bg = bmp.bg or {}

  local p = ctx.params or {}
  local use_font_colors = (p.useFontColors ~= false)
  local use_fallback_fg = (p.useFg ~= false)
  local use_fallback_bg = (p.useBg == true)

  local fallback_fg = (use_fallback_fg and type(ctx.fg) == "number") and math.floor(ctx.fg) or nil
  local fallback_bg = (use_fallback_bg and type(ctx.bg) == "number") and math.floor(ctx.bg) or nil

  -- Draw only the destination clip rect, but index into the original source stride.
  for y = 0, dh - 1 do
    for x = 0, dw - 1 do
      local si = src_x0 + x
      local sj = src_y0 + y
      if si < src_w and sj < src_h then
        local i = (sj * src_w) + si + 1
      local cpi = tonumber(cp[i]) or 32
      if cpi <= 0 then cpi = 32 end

      local out_fg = nil
      local out_bg = nil

      local fgi = tonumber(fg[i])
      local bgi = tonumber(bg[i])
      if use_font_colors then
        if fgi ~= nil and fgi >= 0 then out_fg = math.floor(fgi) end
        if bgi ~= nil and bgi >= 0 then out_bg = math.floor(bgi) end
      end
      if out_fg == nil then out_fg = fallback_fg end
      if out_bg == nil then out_bg = fallback_bg end

        set_cell(layer, dx0 + x, dy0 + y, cpi, out_fg, out_bg)
      end
    end
  end

  -- Selection was already set above; return the effective clamped rect.
  return { x = dx0, y = dy0, w = dw, h = dh }
end

-- Live stamp tracking: if the user changes font/options while the last stamped region is
-- still selected, re-render and overwrite the region in-place.
local last_stamp = nil
local last_text_cache = nil
local last_text_cache_key = nil

local function current_text()
  -- Cache concatenation so render_key() doesn't rebuild a big string every frame.
  -- Use table length + last char as a cheap cache key.
  local n = (type(text_chars) == "table") and #text_chars or 0
  local tail = ""
  if n > 0 and type(text_chars[n]) == "string" then tail = text_chars[n] end
  local k = tostring(n) .. "|" .. tail
  if last_text_cache_key == k and type(last_text_cache) == "string" then
    return last_text_cache
  end
  last_text_cache_key = k
  last_text_cache = concat_chars(text_chars)
  return last_text_cache
end

local function render_key(ctx)
  local id = current_font_id(ctx)
  local p = (ctx and ctx.params) or {}
  local text = current_text()
  local editMode = (p.editMode == true) and "1" or "0"
  local outlineStyle = tostring(tonumber(p.outlineStyle) or 0)
  local useFontColors = (p.useFontColors ~= false) and "1" or "0"
  local useFg = (p.useFg ~= false) and "1" or "0"
  local useBg = (p.useBg == true) and "1" or "0"
  return id .. "|" .. text .. "|" .. editMode .. "|" .. outlineStyle .. "|" .. useFontColors .. "|" .. useFg .. "|" .. useBg
end

local function try_place(ctx, layer, x, y)
  local id = current_font_id(ctx)
  if id == "" then return false end
  local text = concat_chars(text_chars)
  if text == "" then return false end

  local p = ctx.params or {}
  local opts = {
    editMode = (p.editMode == true),
    outlineStyle = tonumber(p.outlineStyle) or 0,
    useFontColors = (p.useFontColors ~= false),
    icecolors = true,
  }

  local bmp, err = ansl.font.render(id, text, opts)
  if bmp == nil then
    -- If we ever add a status line / toast command, we can surface err.
    return false
  end

  -- Non-destructive preview: snapshot underlying cells so we can restore until commit/cancel.
  local x0, y0, w0, h0 = effective_rect(ctx, x, y, bmp.w, bmp.h)
  local backup = capture_backup(ctx, layer, x0, y0, w0, h0)
  local sel = stamp_bitmap(ctx, layer, x0, y0, bmp)
  if not sel then
    return false
  end

  last_stamp = {
    key = render_key(ctx),
    x = tonumber(sel.x) or x,
    y = tonumber(sel.y) or y,
    w = tonumber(sel.w) or (tonumber(bmp.w) or 0),
    h = tonumber(sel.h) or (tonumber(bmp.h) or 0),
    pending_rerender = false,
    preview = (backup ~= nil), -- if backup failed (too big), we fall back to destructive behavior
    backup = backup,
    bmp = bmp,
    drag = false,
    grab_dx = 0,
    grab_dy = 0,
  }
  return true
end

local function preview_move_to(ctx, layer, nx, ny)
  if not ctx or not layer or last_stamp == nil or last_stamp.bmp == nil then return end
  if last_stamp.preview ~= true then return end

  nx = math.floor(tonumber(nx) or last_stamp.x or 0)
  ny = math.floor(tonumber(ny) or last_stamp.y or 0)
  if ny < 0 then ny = 0 end

  -- Clamp X so we don't fight the selection clamp behavior.
  local cols = tonumber((ctx and ctx.cols) or 0) or 0
  if cols > 0 then
    nx = clamp_int(nx, 0, cols - 1)
  else
    if nx < 0 then nx = 0 end
  end

  if nx == last_stamp.x and ny == last_stamp.y then return end

  -- Restore previous underlying content.
  if last_stamp.backup ~= nil then
    restore_backup(layer, last_stamp.backup)
  end

  -- Snapshot new destination region.
  local w = tonumber(last_stamp.bmp.w) or 0
  local h = tonumber(last_stamp.bmp.h) or 0
  local x0, y0, w0, h0 = effective_rect(ctx, nx, ny, w, h)
  local backup = capture_backup(ctx, layer, x0, y0, w0, h0)
  last_stamp.backup = backup
  last_stamp.preview = (backup ~= nil)

  -- Stamp preview at new location.
  local sel = stamp_bitmap(ctx, layer, x0, y0, last_stamp.bmp)
  if type(sel) == "table" then
    last_stamp.x = tonumber(sel.x) or x0
    last_stamp.y = tonumber(sel.y) or y0
    last_stamp.w = tonumber(sel.w) or w0
    last_stamp.h = tonumber(sel.h) or h0
  else
    last_stamp.x = x0
    last_stamp.y = y0
    last_stamp.w = w0
    last_stamp.h = h0
  end
end

local function maybe_rerender_selected_stamp(ctx, layer)
  local canvas = ctx.canvas
  if canvas == nil or last_stamp == nil then return end
  if canvas:isMovingSelection() then
    -- Defer until move commit.
    if last_stamp.key ~= render_key(ctx) then
      last_stamp.pending_rerender = true
    end
    return
  end

  -- If selection doesn't match our last stamp rect, don't auto-overwrite.
  if not canvas:hasSelection() then
    last_stamp = nil
    return
  end
  local sx, sy, sw, sh = canvas:getSelection()
  sx = tonumber(sx) or 0
  sy = tonumber(sy) or 0
  sw = tonumber(sw) or 0
  sh = tonumber(sh) or 0
  if sx ~= last_stamp.x or sy ~= last_stamp.y or sw ~= last_stamp.w or sh ~= last_stamp.h then
    return
  end

  local want_key = render_key(ctx)
  if want_key == last_stamp.key and last_stamp.pending_rerender ~= true then
    return
  end

  local id = current_font_id(ctx)
  local text = current_text()
  if id == "" or text == "" then
    return
  end

  -- Throttle live rerender to avoid freezing the UI on large fonts / rapid input repeats.
  local now = tonumber((ctx and ctx.time) or 0) or 0
  local last_t = tonumber(last_stamp.last_render_time or 0) or 0
  local min_dt = 0.06 -- ~16 fps max for live rerender
  if (now - last_t) < min_dt then
    last_stamp.pending_rerender = true
    return
  end

  local p = ctx.params or {}
  local opts = {
    editMode = (p.editMode == true),
    outlineStyle = tonumber(p.outlineStyle) or 0,
    useFontColors = (p.useFontColors ~= false),
    icecolors = true,
  }
  local bmp, _ = ansl.font.render(id, text, opts)
  if bmp == nil then
    return
  end

  local sel = nil
  if last_stamp.preview == true then
    -- Preview mode: restore underlying, resnapshot if size changed, then stamp.
    if last_stamp.backup ~= nil then
      restore_backup(layer, last_stamp.backup)
    end
    local x0, y0, w0, h0 = effective_rect(ctx, last_stamp.x, last_stamp.y, bmp.w, bmp.h)
    local backup = capture_backup(ctx, layer, x0, y0, w0, h0)
    last_stamp.backup = backup
    last_stamp.preview = (backup ~= nil)
    sel = stamp_bitmap(ctx, layer, x0, y0, bmp)
  else
    -- Committed mode: destructive overwrite.
    clear_rect(layer, last_stamp.x, last_stamp.y, last_stamp.w, last_stamp.h)
    sel = stamp_bitmap(ctx, layer, last_stamp.x, last_stamp.y, bmp)
  end

  last_stamp.key = want_key
  last_stamp.last_render_time = now
  last_stamp.bmp = bmp
  if type(sel) == "table" then
    last_stamp.x = tonumber(sel.x) or last_stamp.x
    last_stamp.y = tonumber(sel.y) or last_stamp.y
    last_stamp.w = tonumber(sel.w) or last_stamp.w
    last_stamp.h = tonumber(sel.h) or last_stamp.h
  else
    last_stamp.w = tonumber(bmp.w) or last_stamp.w
    last_stamp.h = tonumber(bmp.h) or last_stamp.h
  end
  last_stamp.pending_rerender = false
end

function render(ctx, layer)
  if not ctx or not layer then return end
  if not ctx.focused then return end

  local canvas = ctx.canvas
  local phase = tonumber(ctx.phase) or 0
  local keys = ctx.keys or {}
  local cursor = ctx.cursor or {}
  local mods = ctx.mods or {}

  -- Phase 1: mouse-driven move (floating selection).
  if phase == 1 then
    -- Click-to-place / click-to-drag preview (non-destructive until commit).
    local pressed = (cursor.valid == true) and (cursor.left == true) and ((cursor.p and cursor.p.left) ~= true)
    if pressed then
      local cx = tonumber(cursor.x) or 0
      local cy = tonumber(cursor.y) or 0
      local inside = (canvas ~= nil and canvas:hasSelection() and canvas:selectionContains(cx, cy))
      if not inside then
        if try_place(ctx, layer, cx, cy) then
          -- Start drag immediately after placing.
          if last_stamp ~= nil then
            last_stamp.drag = true
            last_stamp.grab_dx = clamp_int(cx - (last_stamp.x or 0), 0, math.max(0, (last_stamp.w or 1) - 1))
            last_stamp.grab_dy = clamp_int(cy - (last_stamp.y or 0), 0, math.max(0, (last_stamp.h or 1) - 1))
          end
        end
      else
        -- Begin drag existing preview selection.
        if last_stamp ~= nil then
          last_stamp.drag = true
          last_stamp.grab_dx = clamp_int(cx - (last_stamp.x or 0), 0, math.max(0, (last_stamp.w or 1) - 1))
          last_stamp.grab_dy = clamp_int(cy - (last_stamp.y or 0), 0, math.max(0, (last_stamp.h or 1) - 1))
        end
      end
    end

    -- While dragging, move preview (no commit on release).
    if last_stamp ~= nil and last_stamp.drag == true and cursor.valid == true then
      local cx = tonumber(cursor.x) or 0
      local cy = tonumber(cursor.y) or 0
      if cursor.left == true then
        preview_move_to(ctx, layer, cx - (last_stamp.grab_dx or 0), cy - (last_stamp.grab_dy or 0))
      else
        -- Mouse released: stop dragging but keep preview.
        last_stamp.drag = false
      end
    end

    -- If options/text change while preview is selected, rerender in-place.
    maybe_rerender_selected_stamp(ctx, layer)
    return
  end

  -- Phase 0: keyboard + selection initiation.
  if keys.escape then
    if last_stamp ~= nil and last_stamp.preview == true then
      -- Cancel preview: restore underlying and clear selection.
      if last_stamp.backup ~= nil then
        restore_backup(layer, last_stamp.backup)
      end
      last_stamp = nil
      if canvas ~= nil then canvas:clearSelection() end
      return
    end
    if canvas ~= nil then canvas:clearSelection() end
    return
  end

  if (ctx.params and ctx.params.clearText == true) then
    text_chars = {}
  end

  push_typed(text_chars, ctx.typed)
  if keys.backspace then
    pop_char(text_chars)
  end

  -- Multiline: Shift+Enter inserts a newline into the text buffer.
  if keys.enter == true and mods.shift == true then
    text_chars[#text_chars + 1] = "\n"
  end

  -- Now that text/options may have changed, update any live stamp in-place.
  maybe_rerender_selected_stamp(ctx, layer)

  -- Enter places at caret (and selects).
  local caret = ctx.caret or {}
  local place_pressed = ((keys.enter == true) and (mods.shift ~= true)) or (ctx.params and ctx.params.place == true)
  if place_pressed then
    -- If we have an active preview, "Place" commits it (stop restoring underlying).
    if last_stamp ~= nil and last_stamp.preview == true then
      last_stamp.preview = false
      last_stamp.backup = nil
      return
    end
    local x = tonumber(caret.x) or 0
    local y = tonumber(caret.y) or 0
    try_place(ctx, layer, x, y)
    return
  end
end


