-- HG_SDF construction kit demo (native LuaJIT bindings)
-- Exercises: 3D primitives, domain repetition helpers, boolean ops, and fOp* operators.
--
-- Controls:
-- - Move cursor to vary repetition + blend radius (if your host provides cursor in ctx.cursor)

settings = { fps = 60 }

local v2 = ansl.vec2
local v3 = ansl.vec3
local sdf = ansl.sdf
local utf8chars = ansl.string.utf8chars

-- Rich gradient for clearer reads of distance falloff (classic ASCII ramp + blocks)
local density = utf8chars(" .'`^\",:;Il!i><~+_-?][}{1)(|\\/*tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$▓█")

local function clampi(i, lo, hi)
  if i < lo then return lo end
  if i > hi then return hi end
  return i
end

-- Called once per frame (host-dependent; safe if absent)
function pre(ctx, cursor)
  -- Touch a wide selection of APIs so missing bindings fail loudly.
  local p3 = { x = 0.1, y = -0.2, z = 0.3 }
  local n3 = { x = 0.0, y = 1.0, z = 0.0 }
  local a3 = { x = -0.5, y = 0.2, z = 0.1 }
  local b3 = { x = 0.5, y = 0.2, z = -0.1 }

  sdf.opUnion(0.1, 0.2)
  sdf.opIntersection(0.1, 0.2)
  sdf.opDifference(0.1, 0.2)

  sdf.fSphere(p3, 0.5)
  sdf.fPlane(p3, n3, 0.1)
  sdf.fBoxCheap(p3, { x = 0.3, y = 0.3, z = 0.3 })
  sdf.fBox(p3, { x = 0.3, y = 0.3, z = 0.3 })
  sdf.fBox2Cheap({ x = 0.2, y = -0.1 }, { x = 0.3, y = 0.4 })
  sdf.fBox2({ x = 0.2, y = -0.1 }, { x = 0.3, y = 0.4 })
  sdf.fCorner({ x = 0.2, y = -0.1 })
  sdf.fBlob(p3)
  sdf.fCylinder(p3, 0.2, 0.4)
  sdf.fCapsule(p3, 0.15, 0.25)
  sdf.fCapsule(p3, a3, b3, 0.1)
  sdf.fLineSegment(p3, a3, b3)
  sdf.fTorus(p3, 0.1, 0.35)
  sdf.fCircle(p3, 0.25)
  sdf.fDisc(p3, 0.25)
  sdf.fHexagonCircumcircle(p3, { x = 0.3, y = 0.2 })
  sdf.fHexagonIncircle(p3, { x = 0.3, y = 0.2 })
  sdf.fCone(p3, 0.3, 0.5)

  sdf.fGDF(p3, 0.5, 3, 6)
  sdf.fGDF(p3, 0.5, 4.0, 3, 6)
  sdf.fOctahedron(p3, 0.5)
  sdf.fOctahedron(p3, 0.5, 2.5)
  sdf.fDodecahedron(p3, 0.5)
  sdf.fDodecahedron(p3, 0.5, 2.5)
  sdf.fIcosahedron(p3, 0.5)
  sdf.fIcosahedron(p3, 0.5, 2.5)
  sdf.fTruncatedOctahedron(p3, 0.5)
  sdf.fTruncatedOctahedron(p3, 0.5, 2.5)
  sdf.fTruncatedIcosahedron(p3, 0.5)
  sdf.fTruncatedIcosahedron(p3, 0.5, 2.5)

  sdf.fOpUnionChamfer(0.1, 0.2, 0.05)
  sdf.fOpIntersectionChamfer(0.1, 0.2, 0.05)
  sdf.fOpDifferenceChamfer(0.1, 0.2, 0.05)
  sdf.fOpUnionRound(0.1, 0.2, 0.05)
  sdf.fOpIntersectionRound(0.1, 0.2, 0.05)
  sdf.fOpDifferenceRound(0.1, 0.2, 0.05)
  sdf.fOpUnionColumns(0.1, 0.2, 0.05, 5)
  sdf.fOpDifferenceColumns(0.1, 0.2, 0.05, 5)
  sdf.fOpIntersectionColumns(0.1, 0.2, 0.05, 5)
  sdf.fOpUnionStairs(0.1, 0.2, 0.05, 6)
  sdf.fOpIntersectionStairs(0.1, 0.2, 0.05, 6)
  sdf.fOpDifferenceStairs(0.1, 0.2, 0.05, 6)
  sdf.fOpUnionSoft(0.1, 0.2, 0.05)
  sdf.fOpPipe(0.1, 0.2, 0.05)
  sdf.fOpEngrave(0.1, 0.2, 0.05)
  sdf.fOpGroove(0.1, 0.2, 0.05, 0.07)
  sdf.fOpTongue(0.1, 0.2, 0.05, 0.07)

  sdf.pR({ x = 1.0, y = 0.0 }, 0.5)
  sdf.pR45({ x = 1.0, y = 0.0 })
  sdf.pMod1(0.3, 1.0)
  sdf.pModMirror1(0.3, 1.0)
  sdf.pModSingle1(0.3, 1.0)
  sdf.pModInterval1(0.3, 1.0, -2, 2)
  sdf.pModPolar({ x = 0.3, y = 0.2 }, 6)
  sdf.pMod2({ x = 0.3, y = 0.2 }, { x = 1.0, y = 1.0 })
  sdf.pModMirror2({ x = 0.3, y = 0.2 }, { x = 1.0, y = 1.0 })
  sdf.pModGrid2({ x = 0.3, y = 0.2 }, { x = 1.0, y = 1.0 })
  sdf.pMod3({ x = 0.3, y = 0.2, z = 0.1 }, { x = 1.0, y = 1.0, z = 1.0 })
  sdf.pMirror(0.3, 0.1)
  sdf.pMirrorOctant({ x = 0.3, y = -0.2 }, { x = 0.1, y = 0.2 })
  sdf.pReflect({ x = 0.3, y = -0.2, z = 0.1 }, { x = 0.0, y = 1.0, z = 0.0 }, 0.0)
end

function main(coord, ctx, cursor)
  local t = (ctx.time or 0) * 0.001
  local cols, rows = ctx.cols or 0, ctx.rows or 0
  local m = math.min(cols, rows)
  if m <= 0 then return " " end

  local a = (ctx.metrics and ctx.metrics.aspect) or 1
  local st = {
    x = 2.0 * (coord.x - cols / 2 + 0.5) / m * a,
    y = 2.0 * (coord.y - rows / 2 + 0.5) / m,
  }

  -- Cursor-driven controls (if present)
  local cx = (cursor and cursor.x) or (cols * 0.5)
  local cy = (cursor and cursor.y) or (rows * 0.5)
  local rep = 0.45 + 1.35 * (cx / math.max(cols, 1))
  local r = 0.06 + 0.22 * (cy / math.max(rows, 1))

  -- Repeat domain in 2D (Lua-friendly multi-return)
  local p2, cell2 = sdf.pMod2(st, { x = rep, y = rep })

  -- Mix in polar repetition for some variety
  local pr, sector = sdf.pModPolar(p2, 7)

  -- Lift 2D -> 3D point (visualize a slice of 3D geometry)
  local p3 = { x = pr.x, y = 0.25 * math.sin(t + sector * 0.7), z = pr.y }

  -- Build a scene from HG primitives + HG edge operators
  local d_sphere = sdf.fSphere(p3, r)
  local d_box = sdf.fBox(p3, { x = r * 0.9, y = r * 0.9, z = r * 0.9 })
  local d = sdf.fOpUnionRound(d_sphere, d_box, r * 0.35)

  -- Cut a cylindrical "pipe" through it
  local d_pipe = sdf.fCylinder(p3, r * 0.35, r * 1.5)
  d = sdf.opDifference(d, d_pipe)

  -- Add a chamfered floor plane for depth cues
  local d_floor = sdf.fPlane(p3, { x = 0.0, y = 1.0, z = 0.0 }, 0.18)
  d = sdf.fOpUnionChamfer(d, d_floor, 0.03)

  -- Shade
  local v = 1.0 - math.exp(-10.0 * math.abs(d))
  local i = clampi(math.floor(v * #density) + 1, 1, #density)

  local ch = density[i]
  if d < 0 then ch = density[#density] end

  return { char = ch, fg = ansl.colour.ansi16.white, bg = ansl.colour.ansi16.black }
end


