-- Stacked sin waves (Lua port)

local chars = { "█", "▓", "▒", "░", " " }

local function wave(t, y, s1, s2, s3, a1, a2, a3)
  return (math.sin(t + y * s1) + 1) * a1
    + (math.sin(t + y * s2) + 1) * a2
    + (math.sin(t + y * s3)) * a3
end

function main(coord, ctx)
  local t = (ctx.time or 0) * 0.002
  local x = coord.x or 0
  local y = coord.y or 0

  local v0 = (ctx.cols or 0) / 4 + wave(t, y, 0.15, 0.13, 0.37, 10, 8, 5) * 0.9
  local v1 = v0 + wave(t, y, 0.12, 0.14, 0.27, 3, 6, 5) * 0.8
  local v2 = v1 + wave(t, y, 0.089, 0.023, 0.217, 2, 4, 2) * 0.3
  local v3 = v2 + wave(t, y, 0.167, 0.054, 0.147, 4, 6, 7) * 0.4

  local i = (x > v3) and 5
    or (x > v2) and 4
    or (x > v1) and 3
    or (x > v0) and 2
    or 1
  return chars[i]
end


