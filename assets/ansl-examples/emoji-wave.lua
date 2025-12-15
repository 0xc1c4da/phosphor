-- Emoji Wave (Lua port)
-- From wingdings icons to unicode emojis

settings = {
  fg = ansl.color.ansi16.white,
  bg = ansl.color.rgb(100, 0, 300),
}

local floor = math.floor
local sin = math.sin
local cos = math.cos

-- Tokenized (space-separated) to avoid JS-style UTF-16 indexing pitfalls and keep emojis intact.
local density = { "☆", "☺︎", "👀", "🌈", "🌮🌮", "🌈", "👀", "☺︎", "☆" }

function main(coord, ctx)
  local t = (ctx.time or 0) * 0.0008
  local x = coord.x or 0
  local y = coord.y or 0
  local c = ctx.cols or 0

  local posCenter = floor((c - #density) * 0.5)
  local wave = sin(y * cos(t)) * 5
  local i = floor(x + wave) - posCenter -- 0-based

  return density[i + 1] or " "
end


