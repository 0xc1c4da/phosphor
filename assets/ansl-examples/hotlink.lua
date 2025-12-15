-- Hotlink (Lua port)
-- Replaces the JS version that fetches external noise code.
-- Uses native libnoise bindings: ansl.noise.*

settings = { fps = 60 }

local density = ansl.string.utf8chars("Ñ@#W$9876543210?!abcxyz;:+=-,._ ")

local clampi = function(i, lo, hi)
  if i < lo then return lo end
  if i > hi then return hi end
  return i
end

-- Configure once; use :get() per pixel.
local n = ansl.noise.perlin({
  seed = 1337,
  frequency = 2.0,
  lacunarity = 2.0,
  octaves = 4,
  persistence = 0.5,
  quality = "std",
})

function main(coord, ctx)
  local t = (ctx.time or 0) * 0.0007
  local s = 0.03
  local aspect = (ctx.metrics and ctx.metrics.aspect) or 1

  local x = (coord.x or 0) * s
  local y = (coord.y or 0) * s / aspect + t

  local v = n:get(x, y, t) -- usually ~[-1, 1]
  local i = math.floor((v * 0.5 + 0.5) * #density)
  i = clampi(i, 0, #density - 1)
  return density[i + 1]
end


