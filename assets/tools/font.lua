settings = {
  id = "font",
  icon = "𝔉",
  label = "Font",

  -- Tool parameters (host renders UI; values are available under ctx.params.*)
  params = {
    font = { type = "enum", label = "Font", items = { "(no fonts)" }, default = "(no fonts)" },
    useFontColors = { type = "bool", label = "Use font colors", default = true },
    useFg = { type = "bool", label = "Fallback: Use FG", default = true },
    useBg = { type = "bool", label = "Fallback: Use BG", default = false },
    editMode = { type = "bool", label = "Edit markers (outline)", default = false },
    outlineStyle = { type = "int", label = "Outline style", min = 0, max = 18, step = 1, default = 0 },
    place = { type = "button", label = "Place (Enter)", order = 50 },
    clearText = { type = "button", label = "Clear text", sameLine = true, order = 51 },
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

local function stamp_bitmap(ctx, layer, x0, y0, bmp)
  if not ctx or not layer or type(bmp) ~= "table" then return false end
  local w = tonumber(bmp.w) or 0
  local h = tonumber(bmp.h) or 0
  if w <= 0 or h <= 0 then return false end

  local cp = bmp.cp or {}
  local fg = bmp.fg or {}
  local bg = bmp.bg or {}

  local p = ctx.params or {}
  local use_font_colors = (p.useFontColors ~= false)
  local use_fallback_fg = (p.useFg ~= false)
  local use_fallback_bg = (p.useBg == true)

  local fallback_fg = (use_fallback_fg and type(ctx.fg) == "number") and math.floor(ctx.fg) or nil
  local fallback_bg = (use_fallback_bg and type(ctx.bg) == "number") and math.floor(ctx.bg) or nil

  for y = 0, h - 1 do
    for x = 0, w - 1 do
      local i = (y * w) + x + 1
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

      set_cell(layer, x0 + x, y0 + y, cpi, out_fg, out_bg)
    end
  end

  -- Select the stamped region so the user can move it (either via this tool or Select tool).
  local canvas = ctx.canvas
  if canvas ~= nil then
    canvas:setSelection(x0, y0, x0 + w - 1, y0 + h - 1)
  end
  return true
end

-- Live stamp tracking: if the user changes font/options while the last stamped region is
-- still selected, re-render and overwrite the region in-place.
local last_stamp = nil

local function render_key(ctx)
  local id = current_font_id(ctx)
  local p = (ctx and ctx.params) or {}
  local text = concat_chars(text_chars)
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

  if not stamp_bitmap(ctx, layer, x, y, bmp) then
    return false
  end

  last_stamp = {
    key = render_key(ctx),
    x = x, y = y,
    w = tonumber(bmp.w) or 0,
    h = tonumber(bmp.h) or 0,
    pending_rerender = false,
  }
  return true
end

local function try_begin_move(ctx)
  local canvas = ctx.canvas
  local cursor = ctx.cursor or {}
  local mods = ctx.mods or {}
  if canvas == nil or cursor.valid ~= true then return false end

  local pressed = (cursor.left == true) and ((cursor.p and cursor.p.left) ~= true)
  if not pressed then return false end

  local cx = tonumber(cursor.x) or 0
  local cy = tonumber(cursor.y) or 0
  if not canvas:hasSelection() then return false end
  if not canvas:selectionContains(cx, cy) then return false end

  local copy = (mods.shift == true)
  return canvas:beginMoveSelection(cx, cy, copy)
end

local function update_move(ctx)
  local canvas = ctx.canvas
  local cursor = ctx.cursor or {}
  if canvas == nil or cursor.valid ~= true then return end

  if canvas:isMovingSelection() then
    local cx = tonumber(cursor.x) or 0
    local cy = tonumber(cursor.y) or 0
    canvas:updateMoveSelection(cx, cy)

    local released = ((cursor.p and cursor.p.left) == true) and (cursor.left ~= true)
    if released then
      canvas:commitMoveSelection()

      -- If we were tracking a stamp, update its origin to the new selection rect.
      if last_stamp ~= nil and canvas:hasSelection() then
        local sx, sy, sw, sh = canvas:getSelection()
        sx = tonumber(sx) or last_stamp.x
        sy = tonumber(sy) or last_stamp.y
        sw = tonumber(sw) or last_stamp.w
        sh = tonumber(sh) or last_stamp.h
        last_stamp.x = sx
        last_stamp.y = sy
        last_stamp.w = sw
        last_stamp.h = sh
      end
    end
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
  local text = concat_chars(text_chars)
  if id == "" or text == "" then
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

  -- Clear old region, then stamp new (may be different size).
  clear_rect(layer, last_stamp.x, last_stamp.y, last_stamp.w, last_stamp.h)
  stamp_bitmap(ctx, layer, last_stamp.x, last_stamp.y, bmp)

  last_stamp.key = want_key
  last_stamp.w = tonumber(bmp.w) or last_stamp.w
  last_stamp.h = tonumber(bmp.h) or last_stamp.h
  last_stamp.pending_rerender = false
end

function render(ctx, layer)
  if not ctx or not layer then return end
  if not ctx.focused then return end

  local canvas = ctx.canvas
  local phase = tonumber(ctx.phase) or 0
  local keys = ctx.keys or {}
  local cursor = ctx.cursor or {}

  -- Phase 1: mouse-driven move (floating selection).
  if phase == 1 then
    update_move(ctx)

    -- If we deferred a rerender during move, apply it now (after commit).
    if last_stamp ~= nil and last_stamp.pending_rerender == true then
      maybe_rerender_selected_stamp(ctx, layer)
    end

    -- Click-to-place: if we clicked outside the current selection, stamp at click and drag immediately.
    local pressed = (cursor.valid == true) and (cursor.left == true) and ((cursor.p and cursor.p.left) ~= true)
    if pressed then
      local cx = tonumber(cursor.x) or 0
      local cy = tonumber(cursor.y) or 0
      local inside = (canvas ~= nil and canvas:hasSelection() and canvas:selectionContains(cx, cy))
      if not inside then
        if try_place(ctx, layer, cx, cy) and canvas ~= nil then
          canvas:beginMoveSelection(cx, cy, false)
        end
      end
    end
    return
  end

  -- Phase 0: keyboard + selection initiation.
  maybe_rerender_selected_stamp(ctx, layer)

  if keys.escape then
    if canvas ~= nil and canvas:isMovingSelection() then
      canvas:cancelMoveSelection()
    elseif canvas ~= nil then
      canvas:clearSelection()
    end
    return
  end

  if (ctx.params and ctx.params.clearText == true) then
    text_chars = {}
    -- Clearing text should also update any live stamp.
    if last_stamp ~= nil then
      last_stamp.key = ""
    end
  end

  push_typed(text_chars, ctx.typed)
  if keys.backspace then
    pop_char(text_chars)
  end

  -- Enter places at caret (and selects).
  local caret = ctx.caret or {}
  local place_pressed = (keys.enter == true) or (ctx.params and ctx.params.place == true)
  if place_pressed then
    if canvas ~= nil and canvas:isMovingSelection() then
      canvas:commitMoveSelection()
      return
    end
    local x = tonumber(caret.x) or 0
    local y = tonumber(caret.y) or 0
    try_place(ctx, layer, x, y)
    return
  end

  -- If a selection exists, allow click-to-move like the Select tool.
  if canvas ~= nil and not canvas:isMovingSelection() then
    try_begin_move(ctx)
  end
end


