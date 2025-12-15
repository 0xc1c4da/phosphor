local chars = ".-=:abc123?xyz*;%+,"

function main(coord, ctx, cursor)
  local a = (ctx.metrics and ctx.metrics.aspect) or 1
  -- caret = ctx.caret (text caret in cell space)
  local caret = ctx and ctx.caret
  local cx, cy = (caret and caret.x) or 0, (caret and caret.y) or 0
  local x = math.abs(coord.x - cx)
  local y = math.abs(coord.y - cy) / a
  local dist = math.floor(math.max(x, y) + ctx.frame)
  local i = (dist % #chars) + 1
  return chars:sub(i, i)
end
