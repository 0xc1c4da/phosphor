settings = {
  id = "04-deform",
  icon = "≋",
  label = "Deform",
  shortcut = "Alt+D",

  -- Action routing hints (used by host Action Router).
  -- When active: Enter is used to toggle keyboard deform (not create a new line).
  handles = {
    { action = "editor.new_line", when = "active" },
  },

  params = {
    size = { type = "int", label = "Size", ui = "slider", section = "Brush", placement = "quick", order = 0, min = 1, max = 61, step = 1, default = 15, width = 160 },
    mode = { type = "enum", label = "Mode", ui = "segmented", section = "Deform", placement = "quick", order = 1, inline = true, items = { "move", "grow", "shrink", "swirl_cw", "swirl_ccw" }, default = "move" },
    strength = { type = "float", label = "Strength", ui = "slider", section = "Brush", placement = "quick", order = 2, min = 0.0, max = 1.0, step = 0.01, default = 0.75, inline = true, width = 160 },
    spacing = { type = "float", label = "Spacing", ui = "slider", section = "Brush", placement = "quick", order = 3, min = 0.05, max = 2.0, step = 0.05, default = 0.25, inline = true, width = 160 },

    hardness = { type = "int", label = "Hardness", ui = "slider", section = "Brush", min = 0, max = 100, step = 1, default = 80 },
    amount = { type = "float", label = "Amount", ui = "slider", section = "Brush", min = 0.0, max = 2.0, step = 0.05, default = 1.0, inline = true },

    algo = { type = "enum", label = "Algorithm", ui = "combo", section = "Sampling", items = { "warp_quantize", "warp_quantize_sticky", "cell_resample" }, default = "warp_quantize" },
    sample = { type = "enum", label = "Sample", ui = "segmented", section = "Sampling", items = { "layer", "composite" }, default = "layer", inline = true },
    scope = { type = "enum", label = "Scope", ui = "combo", section = "Sampling", items = { "selection_if_any", "selection_only", "full_canvas" }, default = "selection_if_any" },
    hysteresis = { type = "float", label = "Hysteresis", ui = "slider", section = "Sampling", min = 0.0, max = 1.0, step = 0.01, default = 0.05, inline = true },
  },
}

-- Stroke state (Lua-side; engine is stateless per dab).
local stroke_active = false
local prev_x = nil
local prev_y = nil
local carry = 0.0
local keyboard_deform_enabled = false
local prev_enter_down = false

local function clamp(v, a, b)
  if v < a then return a end
  if v > b then return b end
  return v
end

local function dist(ax, ay, bx, by)
  local dx = ax - bx
  local dy = ay - by
  return math.sqrt(dx * dx + dy * dy)
end

local function apply_dab(ctx, layer, x, y, px, py)
  if not ctx or not layer then return nil end
  local canvas = ctx.canvas
  if type(canvas) ~= "userdata" then return nil end

  local p = ctx.params or {}
  local mode = (type(p.mode) == "string") and p.mode or "move"
  local algo = (type(p.algo) == "string") and p.algo or "warp_quantize"
  local size = tonumber(p.size) or 15
  size = clamp(math.floor(size), 1, 61)
  local hardness = tonumber(p.hardness) or 80
  hardness = clamp(hardness, 0, 100)
  local strength = tonumber(p.strength) or 0.75
  strength = clamp(strength, 0.0, 1.0)
  local amount = tonumber(p.amount) or 1.0
  local sample = (type(p.sample) == "string") and p.sample or "layer"
  local scope = (type(p.scope) == "string") and p.scope or "selection_if_any"
  local hysteresis = tonumber(p.hysteresis) or 0.05
  hysteresis = clamp(hysteresis, 0.0, 1.0)

  local args = {
    x = x,
    y = y,
    prev_x = px,
    prev_y = py,
    size = size,
    hardness = hardness, -- allow 0..100; native will normalize
    strength = strength,
    amount = amount,
    mode = mode,
    algo = algo,
    sample = sample,
    scope = scope,
    hysteresis = hysteresis,
    palette = ctx.palette, -- allowed indices list (optional; indices are in the active canvas palette)
    -- Prefer GlyphId token candidates when available (lossless for embedded/bitmap indices).
    glyphIdCandidates = ctx.glyphIdCandidates, -- glyph ids (optional; preferred)
    -- Only pass legacy codepoint candidates when we don't have glyph ids.
    glyphCandidates = (ctx.glyphIdCandidates ~= nil and #ctx.glyphIdCandidates > 0) and nil or ctx.glyphCandidates,
  }

  return ansl.deform.apply_dab(layer, canvas, args)
end

function render(ctx, layer)
  if not ctx or not layer then return end
  if ctx.focused ~= true then return end

  local phase = tonumber(ctx.phase) or 0
  local cols = tonumber(ctx.cols) or 0
  local rows = tonumber(ctx.rows) or 0
  if cols <= 0 or rows <= 0 then return end

  local caret = ctx.caret
  if type(caret) ~= "table" then return end

  caret.x = clamp(math.floor(tonumber(caret.x) or 0), 0, cols - 1)
  caret.y = clamp(math.floor(tonumber(caret.y) or 0), 0, rows - 1)

  local keys = ctx.keys or {}
  local actions = ctx.actions or {}

  -- Phase 0: keyboard-driven deform.
  if phase ~= 1 then
    -- Toggle keyboard deform on Enter. (Shift+Enter applies a single dab without toggling.)
    local enter_down = (keys.enter == true) or (actions["editor.new_line"] == true)
    if enter_down and not prev_enter_down then
      if (ctx.mods and ctx.mods.shift) == true then
        -- One-shot dab at caret.
        apply_dab(ctx, layer, caret.x, caret.y, prev_x, prev_y)
        prev_x = caret.x
        prev_y = caret.y
        carry = 0.0
      else
        keyboard_deform_enabled = not keyboard_deform_enabled
        if keyboard_deform_enabled then
          stroke_active = true
          prev_x = caret.x
          prev_y = caret.y
          carry = 0.0
          -- Initial dab (non-move modes will apply immediately).
          apply_dab(ctx, layer, caret.x, caret.y, nil, nil)
        else
          stroke_active = false
          prev_x = nil
          prev_y = nil
          carry = 0.0
        end
      end
    end
    prev_enter_down = enter_down

    local p = ctx.params or {}
    local size = tonumber(p.size) or 15
    size = clamp(math.floor(size), 1, 61)
    local spacing = tonumber(p.spacing) or 0.25
    spacing = clamp(spacing, 0.01, 10.0)
    local step = math.max(0.01, size * spacing)

    local function apply_segment(x, y)
      if not keyboard_deform_enabled then return end
      if not stroke_active or type(prev_x) ~= "number" or type(prev_y) ~= "number" then
        stroke_active = true
        prev_x = x
        prev_y = y
        carry = 0.0
        apply_dab(ctx, layer, x, y, nil, nil)
        return
      end
      local d = dist(prev_x, prev_y, x, y)
      if d <= 0.0001 then return end
      local t = carry
      while t + step <= d do
        t = t + step
        local a = t / d
        local sx = prev_x + (x - prev_x) * a
        local sy = prev_y + (y - prev_y) * a
        apply_dab(ctx, layer, sx, sy, prev_x, prev_y)
        prev_x = sx
        prev_y = sy
      end
      carry = t - d
      prev_x = x
      prev_y = y
    end

    -- Caret navigation (classic wrap rules; match Edit tool).
    local x0 = caret.x
    local y0 = caret.y
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
        if caret.y > rows - 1 then caret.y = rows - 1 end
        caret.x = 0
      end
      moved = true
    end
    if keys.up then
      if caret.y > 0 then caret.y = caret.y - 1 end
      moved = true
    end
    if keys.down then
      if caret.y < rows - 1 then caret.y = caret.y + 1 end
      moved = true
    end
    if keys.home then caret.x = 0; moved = true end
    if keys["end"] then caret.x = cols - 1; moved = true end

    if moved and (caret.x ~= x0 or caret.y ~= y0) then
      apply_segment(caret.x, caret.y)
    end

    return
  end

  local cursor = ctx.cursor or {}
  if type(cursor) ~= "table" or cursor.valid ~= true then return end

  local left = (cursor.left == true)
  local right = (cursor.right == true)
  local down = left or right

  local x = tonumber(cursor.x)
  local y = tonumber(cursor.y)
  if type(x) ~= "number" or type(y) ~= "number" then return end

  -- Keep tool caret in sync with mouse-driven target so keyboard navigation continues
  -- from the last mouse interaction.
  if down then
    caret.x = clamp(math.floor(x), 0, cols - 1)
    caret.y = clamp(math.floor(y), 0, rows - 1)
  end

  local p = ctx.params or {}
  local size = tonumber(p.size) or 15
  size = clamp(math.floor(size), 1, 61)
  local spacing = tonumber(p.spacing) or 0.25
  spacing = clamp(spacing, 0.01, 10.0)
  local step = math.max(0.01, size * spacing)

  -- Brush size preview (host overlay; transient).
  do
    local r = math.floor(size / 2)
    if ctx.out ~= nil then
      ctx.out[#ctx.out + 1] = { type = "brush.preview", anchor = "cursor", rx = r, ry = r }
    end
  end

  if not down then
    stroke_active = false
    prev_x = nil
    prev_y = nil
    carry = 0.0
    return
  end

  if not stroke_active then
    stroke_active = true
    prev_x = x
    prev_y = y
    carry = 0.0
    -- First dab: move mode returns nil (needs previous), others apply immediately.
    apply_dab(ctx, layer, x, y, nil, nil)
    return
  end

  -- Step along the segment prev -> current using spacing.
  local d = dist(prev_x, prev_y, x, y)
  if d <= 0.0001 then
    return
  end

  local t = carry
  while t + step <= d do
    t = t + step
    local a = t / d
    local sx = prev_x + (x - prev_x) * a
    local sy = prev_y + (y - prev_y) * a
    apply_dab(ctx, layer, sx, sy, prev_x, prev_y)
    prev_x = sx
    prev_y = sy
  end

  carry = t - d
  prev_x = x
  prev_y = y
end