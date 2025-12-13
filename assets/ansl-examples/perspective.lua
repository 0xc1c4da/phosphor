settings = { fps = 30 }

function main(coord, ctx)
  local z = math.floor(coord.y - ctx.rows / 2)
  if z == 0 then return " " end
  local val = (coord.x - ctx.cols / 2) / z
  local n = math.floor(val + ctx.cols / 2 + ctx.frame * 0.3)
  local code = ((n % 94) + 94) % 94 + 32
  return string.char(code)
end
