-- SDF Wireframe Cube
-- Uses pre() to project vertices once per frame, then main() renders segments via sdSegment.

settings = { fps = 60 }

local map = ansl.num.map
local sdSegment = ansl.sdf.sdSegment

local density = ansl.string.utf8chars(" -=+abcdX") -- 0-based index behavior replicated in code below

-- Background pattern (UTF-8 safe)
local bgRows = {
  ansl.string.utf8chars("┼──────"),
  ansl.string.utf8chars("│      "),
  ansl.string.utf8chars("│      "),
  ansl.string.utf8chars("│      "),
  ansl.string.utf8chars("│      "),
  ansl.string.utf8chars("│      "),
}
local bgW = #bgRows[1]
local bgH = #bgRows

-- Cube primitive
local l = 0.6
local boxVertices = {
  { x =  l, y =  l, z =  l },
  { x = -l, y =  l, z =  l },
  { x = -l, y = -l, z =  l },
  { x =  l, y = -l, z =  l },
  { x =  l, y =  l, z = -l },
  { x = -l, y =  l, z = -l },
  { x = -l, y = -l, z = -l },
  { x =  l, y = -l, z = -l },
}
local boxEdges = {
  { 1, 2 }, { 2, 3 }, { 3, 4 }, { 4, 1 },
  { 5, 6 }, { 6, 7 }, { 7, 8 }, { 8, 5 },
  { 1, 5 }, { 2, 6 }, { 3, 7 }, { 4, 8 },
}

-- Projected vertices (filled in pre())
local boxProj = {}

local function rotX(v, ang)
  local s, c = math.sin(ang), math.cos(ang)
  return { x = v.x, y = v.y * c - v.z * s, z = v.y * s + v.z * c }
end

local function rotY(v, ang)
  local s, c = math.sin(ang), math.cos(ang)
  return { x = v.x * c + v.z * s, y = v.y, z = -v.x * s + v.z * c }
end

local function rotZ(v, ang)
  local s, c = math.sin(ang), math.cos(ang)
  return { x = v.x * c - v.y * s, y = v.x * s + v.y * c, z = v.z }
end

-- Called once per frame by the host shim (classic ANSL compatibility)
function pre(ctx, cursor, buffer)
  local t = (ctx.time or 0) * 0.01 -- ctx.time is ms
  local rot = { x = t * 0.11, y = t * 0.13, z = -t * 0.15 }
  local d = 2.0
  local zOffs = map(math.sin(t * 0.12), -1, 1, -2.5, -6.0)

  for i = 1, #boxVertices do
    local v = boxVertices[i]
    local vt = rotX(v, rot.x)
    vt = rotY(vt, rot.y)
    vt = rotZ(vt, rot.z)

    local denom = (vt.z - zOffs)
    local k = (denom ~= 0) and (d / denom) or 0
    boxProj[i] = { x = vt.x * k, y = vt.y * k }
  end
end

function main(coord, ctx, cursor)
  local t = (ctx.time or 0) * 0.01 -- ctx.time is ms
  local cols, rows = ctx.cols or 0, ctx.rows or 0
  local m = math.min(cols, rows)
  if m <= 0 then return " " end

  local a = (ctx.metrics and ctx.metrics.aspect) or 1
  local st = {
    x = 2.0 * (coord.x - cols / 2 + 0.5) / m * a,
    y = 2.0 * (coord.y - rows / 2 + 0.5) / m,
  }

  -- Use cursor.x/y directly (don’t depend on cursor.valid) to match the JS example
  -- and keep interaction responsive in hosts that don’t toggle valid.
  local mx = (cursor and cursor.x) or (cols / 2)
  local my = (cursor and cursor.y) or (rows / 2)
  local thickness = map(mx, 0, math.max(1, cols), 0.001, 0.1)
  local expMul = map(my, 0, math.max(1, rows), -100, -5)

  local d = 1e10
  for i = 1, #boxEdges do
    local ea, eb = boxEdges[i][1], boxEdges[i][2]
    local pa = boxProj[ea]
    local pb = boxProj[eb]
    if pa and pb then
      d = math.min(d, sdSegment(st, pa, pb, thickness))
    end
  end

  -- Replicate JS behavior where idx is effectively 0-based and idx==0 shows background.
  local n = #density
  local idx0 = math.floor(math.exp(expMul * math.abs(d)) * n)
  if idx0 < 0 then idx0 = 0 end
  if idx0 > n - 1 then idx0 = n - 1 end

  if idx0 == 0 then
    local x = (coord.x % bgW) + 1
    local y = (coord.y % bgH) + 1
    local ch = (d < 0) and " " or bgRows[y][x]
    return { char = ch, fg = ansl.color.ansi16.black }
  end

  return { char = density[idx0 + 1], fg = ansl.color.hex("#4169e1") } -- royalblue
end


