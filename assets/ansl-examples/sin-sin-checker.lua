-- Sin Sin (checker variation)

settings = { fps = 60 }

local floor = math.floor
local sin = math.sin

local pattern = {
  ansl.string.utf8chars(" _000111_ "),
  ansl.string.utf8chars(".+abc+.      "),
}

local function clampi(i, lo, hi)
  if i < lo then return lo end
  if i > hi then return hi end
  return i
end

function main(coord, ctx)
  local t = (ctx.time or 0) * 0.001
  local x = (coord.x or 0) - (ctx.cols or 0) / 2
  local y = (coord.y or 0) - (ctx.rows or 0) / 2

  local o = sin(x * y * 0.0017 + y * 0.0033 + t) * 40
  local i = floor(math.abs(x + y + o))

  local c = (floor((coord.x or 0) * 0.09) + floor((coord.y or 0) * 0.09)) % 2
  local p = pattern[c + 1]
  local ch = p[(i % #p) + 1]

  local bg = (c == 1) and ansl.color.ansi16.blue or ansl.color.ansi16.black
  return { char = ch, fg = ansl.color.ansi16.white, bg = bg }
end


