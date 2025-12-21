-- Mod Xor
-- Patterns obtained through modulo and XOR

local pattern = ansl.string.utf8chars("└┧─┨┕┪┖┫┘┩┙┪━")

local floor = math.floor

-- Bit ops:
-- - LuaJIT provides `bit.*`
-- - Lua 5.2+ may provide `bit32.*`
-- - otherwise, use small pure-Lua implementations (works for our small ints here).
local bxor, band
if bit and bit.bxor and bit.band then
  bxor = bit.bxor
  band = bit.band
elseif bit32 and bit32.bxor and bit32.band then
  bxor = bit32.bxor
  band = bit32.band
else
  local function _band(a, b)
    a = floor(a); b = floor(b)
    if a < 0 then a = -a end
    if b < 0 then b = -b end
    local res = 0
    local bitv = 1
    while a > 0 or b > 0 do
      local aa = a % 2
      local bb = b % 2
      if aa == 1 and bb == 1 then res = res + bitv end
      a = floor(a / 2)
      b = floor(b / 2)
      bitv = bitv * 2
    end
    return res
  end

  local function _bxor(a, b)
    a = floor(a); b = floor(b)
    if a < 0 then a = -a end
    if b < 0 then b = -b end
    local res = 0
    local bitv = 1
    while a > 0 or b > 0 do
      local aa = a % 2
      local bb = b % 2
      if aa ~= bb then res = res + bitv end
      a = floor(a / 2)
      b = floor(b / 2)
      bitv = bitv * 2
    end
    return res
  end

  bxor = _bxor
  band = _band
end

function main(coord, ctx)
  local frame = ctx.frame or 0
  local t1 = floor(frame / 2)
  local t2 = floor(frame / 128)

  local x = coord.x or 0
  local y = (coord.y or 0) + t1

  local m = (t2 * 2) % 30 + 31
  local expr = (x + bxor(y, x) - y)
  local i = band(expr % m, 1)
  local c0 = (t2 + i) % #pattern
  return pattern[c0 + 1]
end


