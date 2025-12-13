local ansl = require("ansl")

assert(ansl.version == "1.1")
assert(type(ansl.modules) == "table")
assert(type(ansl.modules.num.map) == "function")

local num = ansl.modules.num
local sdf = ansl.modules.sdf
local vec2 = ansl.modules.vec2

assert(num.map(0.5, 0.0, 1.0, 10.0, 20.0) == 15.0)

local p = {x = 1.0, y = 2.0}
local q = {x = 4.0, y = 6.0}
local r = vec2.sub(q, p)
assert(math.abs(r.x - 3.0) < 1e-9)
assert(math.abs(r.y - 4.0) < 1e-9)

-- NOTE: ANSL's sdBox implementation (as in ansl/src/modules/sdf.js) returns 0 inside the box,
-- not a negative signed distance.
local d0 = sdf.sdBox({x = 0.0, y = 0.0}, {x = 1.0, y = 1.0})
assert(d0 == 0.0)

local d1 = sdf.sdBox({x = 2.0, y = 0.0}, {x = 1.0, y = 1.0})
assert(d1 > 0.0)

print("OK: ansl lua smoke passed")


