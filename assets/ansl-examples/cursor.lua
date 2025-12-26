function main(coord, ctx, cursor)
  -- cursor = ctx.cursor (mouse cursor in cell space)
  local x = math.floor((cursor and cursor.x) or 0)
  local y = math.floor((cursor and cursor.y) or 0)
  if coord.x == x and coord.y == y then return "┼" end
  if coord.x == x then return "│" end
  if coord.y == y then return "─" end
  -- caret = ctx.caret (text caret in cell space)
  local caret = ctx and ctx.caret
  local cx = math.floor((caret and caret.x) or -999999)
  local cy = math.floor((caret and caret.y) or -999999)
  local caret_fg = ansl.colour.ansi16.bright_black
  if coord.x == cx and coord.y == cy then return { char = "┼", fg = caret_fg } end
  if coord.x == cx then return { char = "│", fg = caret_fg } end
  if coord.y == cy then return { char = "─", fg = caret_fg } end
  return ((coord.x + coord.y) % 2 == 1) and "·" or " "
end
