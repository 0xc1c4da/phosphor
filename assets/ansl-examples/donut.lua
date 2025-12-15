--[[
@author ertdfgcvb
@title  Donut
@desc   Ported from a1k0n’s donut demo.
https://www.a1k0n.net/2011/07/20/donut-math.html

This program uses a z-buffer and renders into a cached frame buffer once per frame,
then `main()` returns the cached glyph for each coord.
]]

settings = {
  bg = "#f5f5f5",
  fg = ansl.color.ansi16.black,
}

local shades = ".,-~:;=!*#$@"
local maxN = #shades - 1 -- N is clamped 0..maxN
local TAU = math.pi * 2

-- Cached framebuffer (1D, 1-based) rebuilt once per frame.
local last_frame = -1
local last_cols = -1
local last_rows = -1
local fb = {}

local function rebuild(ctx)
  local width = tonumber(ctx.cols) or 0
  local height = tonumber(ctx.rows) or 0
  if width <= 0 or height <= 0 then
    fb = {}
    return
  end

  local aspect = (ctx.metrics and ctx.metrics.aspect) or 1

  -- Match JS: A,B derived from time (ms)
  local tms = tonumber(ctx.time) or 0
  local A = tms * 0.0015
  local B = tms * 0.0017

  local centerX = width / 2
  local centerY = height / 2
  local scaleX = 50
  local scaleY = scaleX * aspect

  -- Precompute sines/cosines of A,B
  local cA, sA = math.cos(A), math.sin(A)
  local cB, sB = math.cos(B), math.sin(B)

  -- Init framebuffer + z buffer
  local num = width * height
  local z = {}
  for k = 1, num do
    fb[k] = " "
    z[k] = 0
  end

  -- Theta goes around the cross-sectional circle of a torus
  for j = 0, TAU, 0.05 do
    local ct, st = math.cos(j), math.sin(j)

    -- Phi goes around the center of revolution of a torus
    for i = 0, TAU, 0.01 do
      local sp, cp = math.sin(i), math.cos(i)

      local h = ct + 2
      local D = 1 / (sp * h * sA + st * cA + 5) -- 1/z
      local t = sp * h * cA - st * sA

      local x = math.floor(centerX + scaleX * D * (cp * h * cB - t * sB))
      local y = math.floor(centerY + scaleY * D * (cp * h * sB + t * cB))

      if y >= 0 and y < height and x >= 0 and x < width then
        local o = x + width * y -- 0-based linear index
        local idx1 = o + 1      -- Lua arrays are 1-based

        if D > z[idx1] then
          z[idx1] = D

          local N = math.floor(
            8
              * (
                (st * sA - sp * ct * cA) * cB
                - sp * ct * sA
                - st * cA
                - cp * ct * sB
              )
          )
          if N < 0 then N = 0 end
          if N > maxN then N = maxN end

          fb[idx1] = shades:sub(N + 1, N + 1)
        end
      end
    end
  end
end

function main(coord, ctx)
  local frame = tonumber(ctx.frame) or 0
  local cols = tonumber(ctx.cols) or 0
  local rows = tonumber(ctx.rows) or 0

  if frame ~= last_frame or cols ~= last_cols or rows ~= last_rows then
    last_frame = frame
    last_cols = cols
    last_rows = rows
    rebuild(ctx)
  end

  -- coord.index is 0-based; Lua arrays are 1-based.
  local idx1 = (tonumber(coord.index) or 0) + 1
  return fb[idx1] or " "
end


