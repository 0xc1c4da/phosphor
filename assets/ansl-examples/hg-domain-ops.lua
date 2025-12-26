-- HG_SDF domain-ops showcase (2D slice through 3D scene)
-- Port of the hg_sdf.glsl domain demo logic you pasted, adapted to this editor (no raymarcher).
--
-- Cursor X selects opIndex.

settings = { fps = 60 }

local v3 = ansl.vec3
local sdf = ansl.sdf
local utf8chars = ansl.string.utf8chars

local density = utf8chars(" .'`^\",:;Il!i><~+_-?][}{1)(|\\/*tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$▓█")

local function clampi(i, lo, hi)
  if i < lo then return lo end
  if i > hi then return hi end
  return i
end

local function opIndexFromCursor(cursor, ctx, maxIndex)
  local cols = ctx.cols or 1
  local cx = (cursor and cursor.x) or (cols * 0.5)
  local t = (ctx.time or 0) * 0.001
  local u = (cols > 1) and (cx / (cols - 1)) or 0.0
  if not cursor then
    u = 0.5 + 0.5 * math.sin(t * 0.25)
  end
  return clampi(math.floor(u * (maxIndex + 1)), 0, maxIndex)
end

local function fDomainOps(p, opIndex)
  local size = 2.2
  local c = 0

  if opIndex == 0 then
    -- no domain manipulation
  elseif opIndex == 1 then
    p.x, c = sdf.pMod1(p.x, size)
  elseif opIndex == 2 then
    p.x, c = sdf.pModSingle1(p.x, size)
  elseif opIndex == 3 then
    p.x, c = sdf.pModInterval1(p.x, size, 1, 3)
  elseif opIndex == 4 then
    local xz = { x = p.x, y = p.z }
    xz, c = sdf.pModPolar(xz, 7)
    p.x, p.z = xz.x, xz.y
    p.x = p.x - 10
  elseif opIndex == 5 then
    local xz = { x = p.x, y = p.z }
    xz, c = sdf.pMod2(xz, { x = size, y = size })
    p.x, p.z = xz.x, xz.y
  elseif opIndex == 6 then
    local xz = { x = p.x, y = p.z }
    xz, c = sdf.pModMirror2(xz, { x = size, y = size })
    p.x, p.z = xz.x, xz.y
  elseif opIndex == 7 then
    p, c = sdf.pMod3(p, { x = size, y = size, z = size })
  end

  -- repeated geometry:
  local box = sdf.fBox(p, { x = 1, y = 1, z = 1 })
  local sphere = v3.length(v3.sub(p, { x = 1, y = 1, z = 1 })) - 1
  local d = math.min(box, sphere)

  -- guard object to prevent discontinuities between cells (as in HG demo)
  local guard = -sdf.fBoxCheap(p, { x = size * 0.5, y = size * 0.5, z = size * 0.5 })
  guard = math.abs(guard) + size * 0.1

  return math.min(d, guard), c, size
end

function main(coord, ctx, cursor)
  local cols, rows = ctx.cols or 0, ctx.rows or 0
  local m = math.min(cols, rows)
  if m <= 0 then return " " end

  local a = (ctx.metrics and ctx.metrics.aspect) or 1
  local st = {
    x = 2.0 * (coord.x - cols / 2 + 0.5) / m * a,
    y = 2.0 * (coord.y - rows / 2 + 0.5) / m,
  }

  local opIndex = opIndexFromCursor(cursor, ctx, 7)

  -- Visualize xz-plane: screen -> (x,z), keep y=0
  local scale = 6.0
  local p = { x = st.x * scale, y = 0.0, z = st.y * scale }
  local d = fDomainOps(p, opIndex)

  local k = 8.0
  local v = math.exp(-k * math.abs(d))
  local i = clampi(math.floor(v * (#density - 1)) + 1, 1, #density)
  local ch = density[i]
  if d < 0 then ch = density[#density] end

  return { char = ch, fg = ansl.colour.ansi16.white, bg = ansl.colour.ansi16.black }
end


