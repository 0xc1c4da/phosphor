local chars = ".-=:abc123?xyz*;%+,"

function main(coord, ctx, cursor)
  local a = (ctx.metrics and ctx.metrics.aspect) or 1
  -- cursor = ctx.cursor (mouse cursor in cell space)
  local cx, cy = (cursor and cursor.x) or 0, (cursor and cursor.y) or 0
  local x = math.abs(coord.x - cx)
  local y = math.abs(coord.y - cy) / a
  local dist = math.floor(math.max(x, y) + ctx.frame)
  local i = (dist % #chars) + 1
  return chars:sub(i, i)
end
