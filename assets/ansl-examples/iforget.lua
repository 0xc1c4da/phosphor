-- Port of assets/ansl-examples/iforget.js to Lua (classic ANSL-style `main()`).
-- The host will wrap `main()` into `render(ctx, layer)` automatically.

local num = ANSL.modules.num
local sdf = ANSL.modules.sdf

local map = num.map
local sdBox = sdf.sdBox
local opSmoothUnion = sdf.opSmoothUnion

-- Split a UTF-8 string into glyph strings once at load-time (fast and reusable).
local density = ANSL.modules.string.utf8chars("▚▀abc|/:÷×+-=?*·")

local function transform(p, trans, rot)
  local s = math.sin(-rot)
  local c = math.cos(-rot)
  local dx = p.x - trans.x
  local dy = p.y - trans.y
  return {
    x = dx * c - dy * s,
    y = dx * s + dy * c,
  }
end

function main(coord, context, cursor, buffer)
  local t = context.time
  local m = math.max(context.cols, context.rows)
  local a = (context.metrics and context.metrics.aspect) or 1.0

  local st = {
    x = 2.0 * (coord.x - context.cols / 2) / m,
    y = 2.0 * (coord.y - context.rows / 2) / m / a,
  }

  local d = 1e100
  local s = map(math.sin(t * 0.0005), -1, 1, 0.0, 0.4)
  local g = 1.2

  local by = -g
  while by <= g + 1e-12 do
    local bx = -g
    while bx <= g + 1e-12 do
      local r = t * 0.0004 * (bx + g * 2) + (by + g * 2)
      local f = transform(st, { x = bx, y = by }, r)
      local d1 = sdBox(f, { x = g * 0.33, y = 0.01 })
      d = opSmoothUnion(d, d1, s)
      bx = bx + g * 0.33
    end
    by = by + g * 0.33
  end

  local c = 1.0 - math.exp(-5 * math.abs(d))
  local idx = math.floor(c * #density) + 1
  if idx < 1 then idx = 1 end
  if idx > #density then idx = #density end
  return density[idx]
end


