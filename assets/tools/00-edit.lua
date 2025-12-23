settings = {
  id = "00-edit",
  icon = "⌶",
  label = "Edit"
  ,
  -- Action routing hints (used by host Action Router).
  -- This tool intentionally overrides selection-delete semantics: with an active selection,
  -- Backspace/Delete continue to behave like text editing (single-cell delete) rather than
  -- deleting the whole selection.
  handles = {
    { action = "editor.backspace", when = "active" },
    { action = "editor.new_line", when = "active" },
    { action = "selection.delete", when = "active" },
  },
}

local function clamp(v, a, b)
  if v < a then return a end
  if v > b then return b end
  return v
end

local function is_down_now(cursor)
  if not cursor or not cursor.valid then return false end
  return cursor.left or cursor.right
end

-- Tool contract: host calls render(ctx, layer) every frame.
-- This tool implements the editor's current basic behavior:
-- - mouse click/drag moves caret
-- - arrows/home/end move caret with wrap like a fixed-width editor
-- - typing overwrites cells and advances caret (wrapping at cols)
-- - enter moves to new line
-- - backspace moves left and clears cell (and style)
-- - delete clears cell (and style)
function render(ctx, layer)
  if not ctx or not layer then return end
  if not ctx.focused then return end

  local cols = tonumber(ctx.cols) or 0
  if cols <= 0 then return end

  local caret = ctx.caret
  if type(caret) ~= "table" then return end

  -- Current editor-selected colors (indices in the active canvas palette). nil means "unset".
  local fg = ctx.fg
  if type(fg) ~= "number" then fg = nil end
  local bg = ctx.bg
  if type(bg) ~= "number" then bg = nil end

  -- Current attribute bitmask (0 = none).
  local attrs = ctx.attrs
  if type(attrs) ~= "number" then attrs = 0 end
  attrs = math.floor(attrs)
  if attrs < 0 then attrs = 0 end

  -- Current brush glyph selection (from character picker/palette).
  local brush = ctx.glyph
  if type(brush) ~= "string" or #brush == 0 then brush = " " end

  -- Normalize caret.
  caret.x = clamp(tonumber(caret.x) or 0, 0, cols - 1)
  caret.y = math.max(0, tonumber(caret.y) or 0)

  -- Phase 1: mouse -> caret
  if (tonumber(ctx.phase) or 0) == 1 then
    local cursor = ctx.cursor
    if type(cursor) == "table" and is_down_now(cursor) then
      caret.x = clamp(tonumber(cursor.x) or caret.x, 0, cols - 1)
      caret.y = math.max(0, tonumber(cursor.y) or caret.y)
    end
    return
  end

  -- Phase 0: keyboard + typed input
  local keys = ctx.keys or {}

  -- Arrow navigation (classic wrap rules).
  if keys.left then
    if caret.x > 0 then
      caret.x = caret.x - 1
    elseif caret.y > 0 then
      caret.y = caret.y - 1
      caret.x = cols - 1
    end
  end
  if keys.right then
    if caret.x < cols - 1 then
      caret.x = caret.x + 1
    else
      caret.y = caret.y + 1
      caret.x = 0
    end
  end
  if keys.up then
    if caret.y > 0 then caret.y = caret.y - 1 end
  end
  if keys.down then
    caret.y = caret.y + 1
  end
  if keys.home then caret.x = 0 end
  if keys["end"] then caret.x = cols - 1 end

  -- Editing keys.
  if keys.backspace then
    if caret.x > 0 then
      caret.x = caret.x - 1
    elseif caret.y > 0 then
      caret.y = caret.y - 1
      caret.x = cols - 1
    end
    layer:set(caret.x, caret.y, " ")
    if layer.clearStyle then layer:clearStyle(caret.x, caret.y) end
  end

  if keys["delete"] then
    layer:set(caret.x, caret.y, " ")
    if layer.clearStyle then layer:clearStyle(caret.x, caret.y) end
  end

  if keys.enter then
    -- Insert the currently selected brush glyph (character palette / picker selection).
    if fg == nil and bg == nil and attrs == 0 then
      layer:set(caret.x, caret.y, brush)
    else
      layer:set(caret.x, caret.y, brush, fg, bg, attrs)
    end
    caret.x = caret.x + 1
    if caret.x >= cols then
      caret.x = 0
      caret.y = caret.y + 1
    end
  end

  -- Typed characters.
  local typed = ctx.typed or {}
  if type(typed) == "table" then
    for i = 1, #typed do
      local ch = typed[i]
      if type(ch) == "string" and #ch > 0 then
        if ch == "\t" then ch = " " end
        if ch == "\n" or ch == "\r" then
          caret.y = caret.y + 1
          caret.x = 0
        else
          -- Apply current fg/bg selection when placing glyphs.
          -- Passing nil keeps the channel "unset" (theme default / transparent bg).
          if fg == nil and bg == nil and attrs == 0 then
            layer:set(caret.x, caret.y, ch)
          else
            layer:set(caret.x, caret.y, ch, fg, bg, attrs)
          end
          caret.x = caret.x + 1
          if caret.x >= cols then
            caret.x = 0
            caret.y = caret.y + 1
          end
        end
      end
    end
  end
end
