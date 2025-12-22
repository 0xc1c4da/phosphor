settings = {
  id = "04-deform",
  icon = "≋",
  label = "Deform",

  params = {
    size = { type = "int", label = "Size", ui = "slider", section = "Brush", primary = true, order = 0, min = 1, max = 61, step = 1, default = 15, width = 160 },
    mode = { type = "enum", label = "Mode", ui = "segmented", section = "Deform", primary = true, order = 1, inline = true, items = { "move", "grow", "shrink", "swirl_cw", "swirl_ccw" }, default = "move" },
    strength = { type = "float", label = "Strength", ui = "slider", section = "Brush", primary = true, order = 2, min = 0.0, max = 1.0, step = 0.01, default = 0.75, inline = true, width = 160 },
    spacing = { type = "float", label = "Spacing", ui = "slider", section = "Brush", primary = true, order = 3, min = 0.05, max = 2.0, step = 0.05, default = 0.25, inline = true, width = 160 },

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
    palette = ctx.palette, -- xterm indices (optional)
    glyphCandidates = ctx.glyphCandidates, -- codepoints (optional)
  }

  return ansl.deform.apply_dab(layer, canvas, args)
end

function render(ctx, layer)
  if not ctx or not layer then return end
  if ctx.focused ~= true then return end

  local phase = tonumber(ctx.phase) or 0
  if phase ~= 1 then return end

  local cursor = ctx.cursor or {}
  if type(cursor) ~= "table" or cursor.valid ~= true then return end

  local left = (cursor.left == true)
  local right = (cursor.right == true)
  local down = left or right

  local x = tonumber(cursor.x)
  local y = tonumber(cursor.y)
  if type(x) ~= "number" or type(y) ~= "number" then return end

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