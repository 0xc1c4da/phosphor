-- Sin Sin (wave variation)

local pattern = ansl.string.utf8chars("┌┘└┐╰╮╭╯")
local sin = math.sin
local abs = math.abs

local function round(x)
  return math.floor(x + 0.5)
end

function main(coord, ctx)
  local t = (ctx.time or 0) * 0.0005
  local x = coord.x or 0
  local y = coord.y or 0
  local o = sin(y * x * sin(t) * 0.003 + y * 0.01 + t) * 20
  local i0 = round(abs(x + y + o)) % #pattern
  return pattern[i0 + 1]
end


