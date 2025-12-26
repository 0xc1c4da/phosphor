-- HG_SDF bool-ops showcase (2D slice through 3D scene)
-- Port of the hg_sdf.glsl demo logic you pasted, adapted to this editor (no raymarcher).
--
-- Cursor X selects opIndex. Cursor Y controls r (feature radius).

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
    u = 0.5 + 0.5 * math.sin(t * 0.35)
  end
  return clampi(math.floor(u * (maxIndex + 1)), 0, maxIndex)
end

local function rFromCursor(cursor, ctx)
  local rows = ctx.rows or 1
  local cy = (cursor and cursor.y) or (rows * 0.5)
  local u = (rows > 1) and (cy / (rows - 1)) or 0.5
  return 0.05 + 0.45 * u
end

local function fBoolOps(p, opIndex, r, n)
  local box = sdf.fBox(p, { x = 1, y = 1, z = 1 })
  local sphere = v3.length(v3.sub(p, { x = 1, y = 1, z = 1 })) - 1
  local d

  if opIndex == 0 then d = math.min(box, sphere)
  elseif opIndex == 1 then d = math.max(box, sphere)
  elseif opIndex == 2 then d = math.max(box, -sphere)

  elseif opIndex == 3 then d = sdf.fOpUnionRound(box, sphere, r)
  elseif opIndex == 4 then d = sdf.fOpIntersectionRound(box, sphere, r)
  elseif opIndex == 5 then d = sdf.fOpDifferenceRound(box, sphere, r)

  elseif opIndex == 6 then d = sdf.fOpUnionChamfer(box, sphere, r)
  elseif opIndex == 7 then d = sdf.fOpIntersectionChamfer(box, sphere, r)
  elseif opIndex == 8 then d = sdf.fOpDifferenceChamfer(box, sphere, r)

  elseif opIndex == 9 then d = sdf.fOpUnionColumns(box, sphere, r, n)
  elseif opIndex == 10 then d = sdf.fOpIntersectionColumns(box, sphere, r, n)
  elseif opIndex == 11 then d = sdf.fOpDifferenceColumns(box, sphere, r, n)

  elseif opIndex == 12 then d = sdf.fOpUnionStairs(box, sphere, r, n)
  elseif opIndex == 13 then d = sdf.fOpIntersectionStairs(box, sphere, r, n)
  elseif opIndex == 14 then d = sdf.fOpDifferenceStairs(box, sphere, r, n)

  elseif opIndex == 15 then d = sdf.fOpPipe(box, sphere, r * 0.3)
  elseif opIndex == 16 then d = sdf.fOpEngrave(box, sphere, r * 0.3)
  elseif opIndex == 17 then d = sdf.fOpGroove(box, sphere, r * 0.3, r * 0.3)
  elseif opIndex == 18 then d = sdf.fOpTongue(box, sphere, r * 0.3, r * 0.3)
  else d = math.min(box, sphere)
  end

  return d
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

  local opIndex = opIndexFromCursor(cursor, ctx, 18)
  local r = rFromCursor(cursor, ctx)
  local n = 4

  -- Visualize a 2D slice: map screen -> (x,y,z=0)
  local scale = 2.2
  local p = { x = st.x * scale, y = st.y * scale, z = 0.0 }
  local d = fBoolOps(p, opIndex, r, n)

  -- Shade by distance-to-surface (bright near surface), solid fill inside
  local k = 10.0
  local v = math.exp(-k * math.abs(d))
  local i = clampi(math.floor(v * (#density - 1)) + 1, 1, #density)
  local ch = density[i]
  if d < 0 then ch = density[#density] end

  return { char = ch, fg = ansl.colour.ansi16.white, bg = ansl.colour.ansi16.black }
end


