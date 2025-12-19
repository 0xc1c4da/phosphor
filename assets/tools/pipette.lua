settings = {
  id = "pipette",
  icon = "⟇",
  label = "Pipette",

  -- Tool parameters (host renders UI; values are available under ctx.params.*)
  params = {
    sample = { type = "enum", label = "Sample", items = { "composite", "layer" }, default = "composite" },
    pickChar = { type = "bool", label = "Pick Char", default = true },
    pickFg = { type = "bool", label = "Pick FG", default = true },
    pickBg = { type = "bool", label = "Pick BG", default = true },
    returnToPrev = { type = "bool", label = "Return to previous tool (left click)", default = true },
  },
}

local function is_table(t) return type(t) == "table" end

local function emit(ctx, cmd)
  if type(ctx) ~= "table" or type(cmd) ~= "table" then return end
  local out = ctx.out
  if type(out) ~= "table" then return end
  out[#out + 1] = cmd
end

local function to_int(v, def)
  v = tonumber(v)
  if v == nil then return def end
  return math.floor(v)
end

local function clamp(v, a, b)
  if v < a then return a end
  if v > b then return b end
  return v
end

local function press_edge(cursor, which)
  if not is_table(cursor) or not cursor.valid then return false end
  local prev = cursor.p or {}
  if which == "left" then
    return (cursor.left == true) and (prev.left ~= true)
  end
  if which == "right" then
    return (cursor.right == true) and (prev.right ~= true)
  end
  return false
end

local function moved_cell(cursor)
  if not is_table(cursor) or not cursor.valid then return false end
  local p = cursor.p or {}
  local px = to_int(p.x, to_int(cursor.x, 0))
  local py = to_int(p.y, to_int(cursor.y, 0))
  return (to_int(cursor.x, 0) ~= px) or (to_int(cursor.y, 0) ~= py)
end

local function any_down(cursor)
  if not is_table(cursor) or not cursor.valid then return false end
  return (cursor.left == true) or (cursor.right == true)
end

local function sample_at(ctx, layer, x, y)
  local p = ctx.params or {}
  local mode = p.sample
  if type(mode) ~= "string" then mode = "composite" end

  local ch, fg, bg, cp
  local canvas = ctx.canvas
  if canvas and canvas.getCell then
    ch, fg, bg, cp = canvas:getCell(x, y, mode)
  else
    -- Fallback: layer sample only (older hosts).
    ch, fg, bg, cp = layer:get(x, y)
  end

  local mods = ctx.mods or {}
  local pickChar = (p.pickChar ~= false)
  local pickFg = (p.pickFg ~= false)
  local pickBg = (p.pickBg ~= false)

  -- Modifiers:
  -- - Shift: pick char only
  -- - Ctrl: pick colors only
  if mods.shift == true then
    pickFg = false
    pickBg = false
  end
  if mods.ctrl == true then
    pickChar = false
  end

  if pickChar and type(cp) == "number" then
    emit(ctx, { type = "brush.set", cp = to_int(cp, 0) })
  end

  local pal = { type = "palette.set" }
  local any = false
  if pickFg and type(fg) == "number" then
    pal.fg = to_int(fg, 0)
    any = true
  end
  if pickBg and type(bg) == "number" then
    pal.bg = to_int(bg, 0)
    any = true
  end
  if any then
    emit(ctx, pal)
  end
  return true
end

function render(ctx, layer)
  if not ctx or not layer then return end
  if not ctx.focused then return end

  local cols = tonumber(ctx.cols) or 0
  if cols <= 0 then return end

  local caret = ctx.caret
  if not is_table(caret) then return end
  caret.x = clamp(tonumber(caret.x) or 0, 0, cols - 1)
  caret.y = math.max(0, tonumber(caret.y) or 0)

  local phase = to_int(ctx.phase, 0)
  local keys = ctx.keys or {}
  local cursor = ctx.cursor or {}

  -- Phase 1: mouse -> caret (caret-based pipette, consistent with edit.lua)
  if phase == 1 then
    if is_table(cursor) and cursor.valid and any_down(cursor) then
      local pressed_left = press_edge(cursor, "left")
      local pressed_right = press_edge(cursor, "right")
      local do_sample = moved_cell(cursor) or pressed_left or pressed_right

      caret.x = clamp(to_int(cursor.x, caret.x), 0, cols - 1)
      caret.y = math.max(0, to_int(cursor.y, caret.y))

      if do_sample then
        sample_at(ctx, layer, caret.x, caret.y)
      end

      -- Left-click is the "commit" (pick + optionally return), right-click is "keep sampling".
      if pressed_left and ((ctx.params or {}).returnToPrev ~= false) then
        emit(ctx, { type = "tool.activate_prev" })
      end
    end
    return
  end

  -- Phase 0: keyboard navigation moves caret with wrap (same as edit.lua).
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

  -- Keyboard-driven sampling: when the caret moves, update the pick selection
  -- (consistent with the mouse path).
  if moved then
    sample_at(ctx, layer, caret.x, caret.y)
  end
end


