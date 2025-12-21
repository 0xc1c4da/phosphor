-- Slime Dish
-- Low-res physarum slime mold simulation
-- Hold mouse button to magnify.

settings = {
  fps = 60,
  bg = ansl.color.ansi16.black,
  fg = ansl.color.ansi16.white,
}

local v2 = ansl.vec2
local map = ansl.num.map

local sin = math.sin
local cos = math.cos
local floor = math.floor
local min = math.min
local max = math.max

-- Environment
local WIDTH = 400
local HEIGHT = 400
local NUM_AGENTS = 1500
local DECAY = 0.9
local MIN_CHEM = 0.0001

-- Agents
local SENS_ANGLE = 45 * math.pi / 180
local SENS_DIST = 9
local AGT_SPEED = 1
local AGT_ANGLE = 45 * math.pi / 180
local DEPOSIT = 1

-- Rendering texture (UTF-8 safe)
local TEXTURE = {
  ansl.string.utf8chars("  ``^@"),
  ansl.string.utf8chars(" ..„v0"),
}
local OOB = " "

local R = min(WIDTH, HEIGHT) / 2

local function bounded_xy(x, y)
  local dx = (x - R)
  local dy = (y - R)
  return (dx * dx + dy * dy) <= (R * R)
end

local function blur(row, col, chem)
  local sum = 0.0
  local base = row * WIDTH + col
  -- 3x3 neighborhood
  for dy = -1, 1 do
    local r = row + dy
    if r >= 0 and r < HEIGHT then
      local roff = r * WIDTH
      for dx = -1, 1 do
        local c = col + dx
        if c >= 0 and c < WIDTH then
          sum = sum + (chem[roff + c + 1] or 0.0)
        end
      end
    end
  end
  return sum / 9.0
end

local function randCircle()
  local rr = math.sqrt(math.random())
  local theta = math.random() * 2 * math.pi
  return { x = rr * cos(theta), y = rr * sin(theta) }
end

-- State
local initialized = false
local chem = {}
local wip = {}
local agents = {}
local viewScale = { x = 100, y = 100 }
local viewFocus = { x = 0.5, y = 0.5 }

local function init(ctx)
  -- Arrays
  local n = WIDTH * HEIGHT
  for i = 1, n do
    chem[i] = 0.0
    wip[i] = 0.0
  end

  -- Agents
  agents = {}
  for _ = 1, NUM_AGENTS do
    local p = randCircle()
    -- v2.mulN(v2.addN(v2.mulN(randCircle(), 0.5), 1), 0.5 * WIDTH)
    local pos = v2.mulN(v2.addN(v2.mulN(p, 0.5), 1), 0.5 * WIDTH)
    local dir = v2.rot(v2.vec2(1, 0), math.random() * 2 * math.pi)
    agents[#agents + 1] = { pos = pos, dir = dir, scatter = false }
  end

  local aspect = (ctx.metrics and ctx.metrics.aspect) or 1
  viewScale = { x = 100, y = 100 / aspect }
  viewFocus = { x = 0.5, y = 0.5 }
  initialized = true
end

local function agent_sense(agent, m, chem_arr)
  local senseVec = v2.mulN(v2.rot(agent.dir, m * SENS_ANGLE), SENS_DIST)
  local pos = v2.floor(v2.add(agent.pos, senseVec))
  local x = pos.x
  local y = pos.y
  if not bounded_xy(x, y) then
    return -1
  end
  local sensed = chem_arr[y * WIDTH + x + 1] or 0.0
  if agent.scatter then
    return 1.0 - sensed
  end
  return sensed
end

local function agent_react(agent, chem_arr)
  local forward = agent_sense(agent, 0, chem_arr)
  local left = agent_sense(agent, -1, chem_arr)
  local right = agent_sense(agent, 1, chem_arr)

  local rotate = 0.0
  if forward > left and forward > right then
    rotate = 0.0
  elseif forward < left and forward < right then
    rotate = (math.random() < 0.5) and (-AGT_ANGLE) or (AGT_ANGLE)
  elseif left < right then
    rotate = AGT_ANGLE
  elseif right < left then
    rotate = -AGT_ANGLE
  elseif forward < 0 then
    rotate = math.pi / 2
  end

  agent.dir = v2.rot(agent.dir, rotate)
  agent.pos = v2.add(agent.pos, v2.mulN(agent.dir, AGT_SPEED))
end

local function agent_deposit(agent, chem_arr)
  local p = v2.floor(agent.pos)
  local x = p.x
  local y = p.y
  if x < 0 or x >= WIDTH or y < 0 or y >= HEIGHT then return end
  local i1 = y * WIDTH + x + 1
  local v = (chem_arr[i1] or 0.0) + DEPOSIT
  if v > 1.0 then v = 1.0 end
  chem_arr[i1] = v
end

local function updateView(cursor, ctx)
  local aspect = (ctx.metrics and ctx.metrics.aspect) or 1
  local pressed = cursor and (cursor.left == true or cursor.right == true)

  local targetScale
  if pressed then
    targetScale = { x = 1, y = 1 / aspect }
  elseif (ctx.rows or 0) / aspect < (ctx.cols or 0) then
    targetScale = {
      y = 1.1 * WIDTH / (ctx.rows or 1),
      x = 1.1 * WIDTH / (ctx.rows or 1) * aspect,
    }
  else
    targetScale = {
      y = 1.1 * WIDTH / (ctx.cols or 1) / aspect,
      x = 1.1 * WIDTH / (ctx.cols or 1),
    }
  end

  viewScale.y = viewScale.y + 0.1 * (targetScale.y - viewScale.y)
  viewScale.x = viewScale.x + 0.1 * (targetScale.x - viewScale.x)

  local targetFocus
  if not pressed then
    targetFocus = { x = 0.5, y = 0.5 }
  else
    targetFocus = {
      y = (cursor.y or 0) / (ctx.rows or 1),
      x = (cursor.x or 0) / (ctx.cols or 1),
    }
  end

  viewFocus.y = viewFocus.y + 0.1 * (targetFocus.y - viewFocus.y)
  viewFocus.x = viewFocus.x + 0.1 * (targetFocus.x - viewFocus.x)
end

function pre(ctx, cursor)
  if not initialized then
    init(ctx)
  end

  -- Diffuse & decay
  for row = 0, HEIGHT - 1 do
    local roff = row * WIDTH
    for col = 0, WIDTH - 1 do
      local val = DECAY * blur(row, col, chem)
      if val < MIN_CHEM then val = 0.0 end
      wip[roff + col + 1] = val
    end
  end

  -- swap chem/wip
  chem, wip = wip, chem

  -- Sense, rotate, move
  local frame = ctx.frame or 0
  local isScattering = sin(frame / 150) > 0.8
  for i = 1, #agents do
    local ag = agents[i]
    ag.scatter = isScattering
    agent_react(ag, chem)
  end

  -- Deposit
  for i = 1, #agents do
    agent_deposit(agents[i], chem)
  end

  updateView(cursor or {}, ctx)
end

function main(coord, ctx)
  if not initialized then
    init(ctx)
  end

  local offsetY = floor(viewFocus.y * (HEIGHT - viewScale.y * (ctx.rows or 0)))
  local offsetX = floor(viewFocus.x * (WIDTH - viewScale.x * (ctx.cols or 0)))

  local fromY = offsetY + floor((coord.y or 0) * viewScale.y)
  local fromX = offsetX + floor((coord.x or 0) * viewScale.x)
  local toY = offsetY + floor(((coord.y or 0) + 1) * viewScale.y)
  local toX = offsetX + floor(((coord.x or 0) + 1) * viewScale.x)

  if not bounded_xy(fromX, fromY) or not bounded_xy(toX, toY) then
    return OOB
  end

  local sampleH = max(1, toY - fromY)
  local sampleW = max(1, toX - fromX)

  local vmax = 0.0
  local sum = 0.0
  for x = fromX, fromX + sampleW - 1 do
    for y = fromY, fromY + sampleH - 1 do
      local v = chem[y * WIDTH + x + 1] or 0.0
      if v > vmax then vmax = v end
      sum = sum + v
    end
  end

  local val = sum / (sampleW * sampleH)
  val = (val + vmax) / 2.0
  val = val ^ (1 / 3)

  local texRow = ((coord.x or 0) + (coord.y or 0)) % #TEXTURE
  local tex = TEXTURE[texRow + 1]
  local texCol = math.ceil(val * (#tex - 1))
  local ch = tex[texCol + 1]
  return ch or OOB
end


