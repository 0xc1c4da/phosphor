settings = {
  id = "02.3-advanced-shading",
  icon = "🌓",
  label = "Advanced Shading (Experimental)",
  shortcut = "Alt+H",

  -- Tool parameters (host renders UI; values are available under ctx.params.*)
  params = {
    size = {
      type = "int",
      label = "Size",
      ui = "slider",
      section = "Brush",
      primary = true,
      order = 0,
      min = 1,
      max = 31,
      step = 1,
      default = 7,
      width = 180,
      tooltip = "Brush diameter in cells.",
    },
    mode = {
      type = "enum",
      label = "Mode",
      ui = "combo",
      section = "Brush",
      primary = true,
      order = 1,
      items = { "toon", "clean", "clean_plus", "halshade", "plus_chroma", "pnakotic", "softblend", "bg_blend" },
      default = "clean",
      tooltip = table.concat({
        "Constrained Pattern Shading",
        "",
        "Modes:",
        "- toon: Big, decisive light/shadow blocks",
        "- clean: Tight 1-cell transition band at region boundaries (minimal ░▒▓).",
        "- clean_plus: Like clean, but slightly smoother (2-cell band). Still avoids overshading.",
        "- halshade: Adds Halaster-style texture (swirls/sparkles/pits) + lit-side border + density reversal, while respecting light.",
        "- plus_chroma: Grit mode. Adds '+' texture and palette-local 'random shade' colour picks (from colours already nearby).",
        "- pnakotic: Subtle micro-variation (small safe hue/value shifts) to avoid flat bands.",
        "- softblend: Wider transition band (softer gradients). Easier to overdo; keep strength modest.",
        "- bg_blend: Background feathering near silhouettes (softens the outside/background side).",
        "",
        "Tip: Right click inverts the intent (darken <-> lighten) while keeping the same global light direction.",
      }, "\n"),
    },
    primary = {
      type = "enum",
      label = "Primary",
      ui = "segmented",
      section = "Brush",
      primary = true,
      order = 2,
      inline = true,
      items = { "darken", "lighten" },
      default = "darken",
      tooltip = "Left click intent for this brush. Right click always inverts it. CPS still respects Light direction, so 'lighten' will prefer the light-facing side and 'darken' the shadow-facing side.",
    },
    strength = {
      type = "float",
      label = "Strength",
      ui = "slider",
      section = "Brush",
      primary = true,
      order = 3,
      min = 0.0,
      max = 1.0,
      step = 0.01,
      default = 0.70,
      inline = true,
      width = 180,
      tooltip = "How hard the brush pushes toward the target tone. Low strength = gentle refinement / fewer changes. High strength = stronger contrast + more aggressive edge snaps ('fast drop') when appropriate.",
    },

    lightAngle = {
      type = "int",
      label = "Light (deg)",
      ui = "slider",
      section = "Lighting",
      order = 10,
      min = 0,
      max = 359,
      step = 1,
      default = 315,
      width = 180,
      tooltip = "Global light direction for the stroke (keep this consistent across the piece). 0=right, 90=down, 180=left, 270=up. CPS uses this to decide what should brighten vs darken and where borders/highlights are allowed.",
    },
    snap8 = {
      type = "bool",
      label = "Snap 8-way",
      ui = "toggle",
      section = "Lighting",
      order = 11,
      default = true,
      inline = true,
      tooltip = "If on: snaps Light to 45° steps (8-way), which usually reads cleaner and keeps shading consistent. If off: continuous angle for finer control.",
    },

    relaxPasses = {
      type = "int",
      label = "Relax",
      ui = "segmented",
      section = "Solve",
      order = 20,
      min = 0,
      max = 3,
      step = 1,
      default = 1,
      tooltip = "Extra consistency passes. 0 is fastest and most 'stroke-like'. 1–3 can reduce speckle and make transitions/borders more coherent, at a CPU cost.",
      inline = true,
    },
    seed = {
      type = "int",
      label = "Seed",
      ui = "slider",
      section = "Solve",
      order = 21,
      min = 0,
      max = 9999,
      step = 1,
      default = 1,
      inline = true,
      width = 180,
      tooltip = "Deterministic randomness control for texture modes (halshade / plus_chroma / pnakotic). Same canvas + same stroke path + same seed => same pattern. Change it if swirls/sparkles/grit feel too regular.",
    },

    allowHardTextures = {
      type = "bool",
      label = "Hard-colour textures",
      ui = "toggle",
      section = "Safety",
      order = 30,
      default = false,
      tooltip = "If off (recommended): suppresses textures on 'hard' colours (classic ANSI reds/greens/blues and vivid saturated colours), because they tend to look noisy/ugly when auto-textured. Turn on only if you intentionally want gritty texture there.",
    },
    allow08Transition = {
      type = "bool",
      label = "Allow 08 as transition",
      ui = "toggle",
      section = "Safety",
      order = 31,
      default = false,
      inline = true,
      tooltip = "Classic ANSI rule: colour 08 is special—often great as a flat fill, often ugly as a blended intermediate. If off: CPS forbids 08 inside fg/bg mixes except safe pairs (03↔08 and 07↔08). Turn on only if you know you want 08 blends.",
    },

    -- Advanced tuning knobs (derived from the CPS spec).
    compatShadedThreshold = {
      type = "float",
      label = "Compat threshold",
      ui = "slider",
      section = "Advanced",
      order = 40,
      min = 0.0,
      max = 1.0,
      step = 0.01,
      default = 0.75,
      width = 180,
      tooltip = "Shaded-together cutoff (Rule 7 scope + transition reasoning). Higher = more conservative about treating two colours as a valid shading pair.",
    },
    brightThreshold = {
      type = "float",
      label = "Bright threshold",
      ui = "slider",
      section = "Advanced",
      order = 41,
      min = 0.0,
      max = 1.0,
      step = 0.01,
      default = 0.65,
      width = 180,
      tooltip = "Luminance cutoff used for Rule 7 'bright next to bright' detection. Lower catches more pairs; higher is less aggressive.",
    },
    rule7WhiteException = {
      type = "bool",
      label = "Rule 7: allow white",
      ui = "toggle",
      section = "Advanced",
      order = 42,
      default = true,
      inline = true,
      tooltip = "Rule 7 allows a 'maybe white' exception. If on: bright-bright adjacency is not penalized when either side is near-white.",
    },
    rule7WhiteThreshold = {
      type = "float",
      label = "White cutoff",
      ui = "slider",
      section = "Advanced",
      order = 43,
      min = 0.0,
      max = 1.0,
      step = 0.01,
      default = 0.92,
      width = 180,
      tooltip = "What counts as 'white' for the Rule 7 exception (only used when Rule 7: allow white is on).",
    },

    thinThicknessMin = {
      type = "int",
      label = "Thin thickness",
      ui = "slider",
      section = "Advanced",
      order = 50,
      min = 1,
      max = 8,
      step = 1,
      default = 2,
      inline = true,
      tooltip = "Thin-feature gating based on region thickness (distance to boundary). Lower shades thinner details; higher protects thin details from clutter.",
    },
    thinDetailMin = {
      type = "float",
      label = "Thin detail",
      ui = "slider",
      section = "Advanced",
      order = 51,
      min = 0.0,
      max = 1.0,
      step = 0.01,
      default = 0.50,
      width = 180,
      tooltip = "Thin-feature gating based on per-cell detail budget D(c). If D(c) < this, textures/extremes are vetoed and transition width is clamped.",
    },

    maxStraightHighlightRun = {
      type = "int",
      label = "Max highlight run",
      ui = "slider",
      section = "Advanced",
      order = 60,
      min = 2,
      max = 12,
      step = 1,
      default = 3,
      inline = true,
      tooltip = "Halaster rule: veto straight highlight runs longer than this (applies to halshade border/sparkle/swirl).",
    },

    maxDensityDetailRatio = {
      type = "float",
      label = "Density detail cap",
      ui = "slider",
      section = "Advanced",
      order = 70,
      min = 0.0,
      max = 0.60,
      step = 0.01,
      default = 0.15,
      width = 180,
      tooltip = "Halshade: cap how much ░▒▓ can appear in a local window (Halaster: density is detail work).",
    },
    densityWindow = {
      type = "int",
      label = "Density window",
      ui = "segmented",
      section = "Advanced",
      order = 71,
      min = 5,
      max = 15,
      step = 1,
      default = 8,
      inline = true,
      tooltip = "Halshade density cap window size (square). Larger windows enforce a smoother global cap; smaller windows allow more local density clusters.",
    },

    chromaMinCount = {
      type = "int",
      label = "Chroma minCount",
      ui = "slider",
      section = "Advanced",
      order = 80,
      min = 1,
      max = 30,
      step = 1,
      default = 6,
      inline = true,
      tooltip = "Plus/Chroma: palette-local random shade only picks colours that appear at least this many times in the local window.",
    },
    chromaRadius = {
      type = "int",
      label = "Chroma radius",
      ui = "slider",
      section = "Advanced",
      order = 81,
      min = 1,
      max = 12,
      step = 1,
      default = 4,
      inline = true,
      tooltip = "Plus/Chroma: radius of the local histogram window (radius=4 means 9×9). Larger = more global reuse; smaller = stricter locality.",
    },

    bgBlendWidth = {
      type = "int",
      label = "BG blend width",
      ui = "slider",
      section = "Advanced",
      order = 90,
      min = 1,
      max = 8,
      step = 1,
      default = 3,
      inline = true,
      tooltip = "bg_blend: how far (in cells) outside the silhouette to feather into the background.",
    },
  },
}

-- IMPORTANT:
-- This file is executed in two contexts:
-- 1) Tool discovery: plain Lua, no `ansl` module available.
-- 2) Tool runtime: host provides global `ansl` and calls render(ctx, layer).
-- So: only access `ansl` lazily at runtime.
local function get_ansl()
  if type(_G) == "table" and type(_G.ansl) == "table" then
    return _G.ansl
  end
  local ok, mod = pcall(function() return require("ansl") end)
  if ok and type(mod) == "table" then
    return mod
  end
  return nil
end

local function clamp(v, a, b)
  if v < a then return a end
  if v > b then return b end
  return v
end

local function to_int(v, def)
  v = tonumber(v)
  if v == nil then return def end
  return math.floor(v)
end

local function to_float(v, def)
  v = tonumber(v)
  if v == nil then return def end
  return v
end

local function norm_glyph(s)
  if type(s) ~= "string" or #s == 0 then return " " end
  return s
end

local function is_blank_glyph(s)
  s = norm_glyph(s)
  return s == " " or s == "\0" or s == " "
end

local function any_down(cursor)
  if type(cursor) ~= "table" or cursor.valid ~= true then return false end
  return cursor.left == true or cursor.right == true
end

local function moved_cell(cursor)
  if type(cursor) ~= "table" or cursor.valid ~= true then return false end
  local p = cursor.p or {}
  local px = to_int(p.x, to_int(cursor.x, 0))
  local py = to_int(p.y, to_int(cursor.y, 0))
  return (to_int(cursor.x, 0) ~= px) or (to_int(cursor.y, 0) ~= py)
end

local function press_edge(cursor)
  if type(cursor) ~= "table" or cursor.valid ~= true then return false end
  local p = cursor.p or {}
  local l_edge = (cursor.left == true) and (p.left ~= true)
  local r_edge = (cursor.right == true) and (p.right ~= true)
  return l_edge or r_edge
end

-- LuaJIT (Lua 5.1) bit ops.
-- NOTE: We must not use Lua 5.3+ bitwise operators here (e.g. ~, <<) because tools run on LuaJIT.
local bit = rawget(_G, "bit")
if bit == nil then
  pcall(function() bit = require("bit") end)
end
local band = bit and bit.band or nil
local bxor = bit and bit.bxor or nil
local lshift = bit and bit.lshift or nil
local rshift = bit and bit.rshift or nil
local function u32(x)
  if band then return band(x, 0xFFFFFFFF) end
  -- Fallback: best-effort clamp (less strict determinism, but keeps tool loadable).
  return x % 0x100000000
end

-- -------------------------------------------------------------------------
-- Deterministic RNG (xorshift32)
-- -------------------------------------------------------------------------
local function rng_new(seed)
  seed = to_int(seed, 1) % 0x100000000
  if seed == 0 then seed = 1 end
  return { s = seed }
end

local function rng_u32(r)
  local x = r.s
  if bxor and lshift and rshift and band then
    x = bxor(x, band(lshift(x, 13), 0xFFFFFFFF))
    x = bxor(x, band(rshift(x, 17), 0xFFFFFFFF))
    x = bxor(x, band(lshift(x, 5), 0xFFFFFFFF))
    r.s = band(x, 0xFFFFFFFF)
  else
    -- Fallback: use math.random (non-deterministic across platforms, but avoids crashing).
    r.s = (to_int(r.s, 1) * 1664525 + 1013904223) % 0x100000000
  end
  return r.s
end

local function rng_float01(r)
  return (rng_u32(r) % 1000000) / 1000000.0
end

local function hash32(a, b, c, d)
  a = to_int(a, 0); b = to_int(b, 0); c = to_int(c, 0); d = to_int(d, 0)
  local x = 2166136261
  local function mix(v)
    v = u32(v)
    if bxor and band then
      x = band(bxor(x, v), 0xFFFFFFFF)
    else
      x = u32((x + v) % 0x100000000)
    end
    x = u32(x * 16777619)
  end
  mix(a); mix(b); mix(c); mix(d)
  if x == 0 then x = 1 end
  return x
end

-- -------------------------------------------------------------------------
-- Palette model: rgb/luminance + ANSI-16 compatibility rules
-- -------------------------------------------------------------------------
local lum_cache = {} -- key idx -> Y in [0,1]
local rgb_cache = {} -- idx -> {r,g,b}
local last_palette_id = nil

-- Clear caches when active palette changes (indices are in the *active palette*).
local function maybe_reset_palette_caches()
  -- Best-effort: palette id is stored in Lua registry by the host.
  local dbg = rawget(_G, "debug")
  if type(dbg) ~= "table" or type(dbg.getregistry) ~= "function" then
    return
  end
  local ok, reg = pcall(function() return dbg.getregistry() end)
  if not ok or type(reg) ~= "table" then
    return
  end
  local pid = reg["phosphor.active_palette_instance_id"]
  if type(pid) ~= "number" then
    return
  end
  if last_palette_id == nil then
    last_palette_id = pid
    return
  end
  if pid ~= last_palette_id then
    lum_cache = {}
    rgb_cache = {}
    last_palette_id = pid
  end
end

local function idx_to_rgb(idx)
  if type(idx) ~= "number" then return 0, 0, 0, false end
  idx = math.floor(idx)
  maybe_reset_palette_caches()
  local c = rgb_cache[idx]
  if c ~= nil then
    return c[1], c[2], c[3], true
  end
  local a = get_ansl()
  if not a or not a.colour or not a.colour.rgb_of then
    return 0, 0, 0, false
  end
  local ok, r, g, b = pcall(function() return a.colour.rgb_of(idx) end)
  if not ok then
    return 0, 0, 0, false
  end
  r = to_int(r, 0); g = to_int(g, 0); b = to_int(b, 0)
  rgb_cache[idx] = { r, g, b }
  return r, g, b, true
end

local function luminance(idx)
  if type(idx) ~= "number" then return 0.0 end
  idx = math.floor(idx)
  maybe_reset_palette_caches()
  local y = lum_cache[idx]
  if y ~= nil then return y end
  local r, g, b, ok = idx_to_rgb(idx)
  if not ok then
    y = 0.0
  else
    y = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0
  end
  lum_cache[idx] = y
  return y
end

local function rgb_to_hsv(r, g, b)
  r = r / 255.0; g = g / 255.0; b = b / 255.0
  local mx = math.max(r, g, b)
  local mn = math.min(r, g, b)
  local d = mx - mn
  local h = 0.0
  if d > 1e-6 then
    if mx == r then
      h = ((g - b) / d) % 6.0
    elseif mx == g then
      h = ((b - r) / d) + 2.0
    else
      h = ((r - g) / d) + 4.0
    end
    h = h / 6.0
  end
  local s = (mx <= 1e-6) and 0.0 or (d / mx)
  local v = mx
  return h, s, v
end

-- Runtime-tunable threshold for Rule 7 brightness detection.
local bright_threshold = 0.65
local rule7_white_exception = true
local rule7_white_threshold = 0.92

-- Classic ANSI-16 tutorial-derived rules.
local colourClass16 = {
  [0] = "neutral",
  [1] = "hard", [2] = "hard", [3] = "soft", [4] = "hard",
  [5] = "soft", [6] = "soft", [7] = "soft", [8] = "weird08",
  [9] = "hard", [10] = "hard", [11] = "hard", [12] = "hard",
  [13] = "hard", [14] = "hard", [15] = "hard",
}

local safeAlikePairs = {
  ["1,9"] = true, ["2,10"] = true, ["3,11"] = true, ["4,12"] = true,
  ["5,13"] = true, ["6,14"] = true, ["7,15"] = true,
}

local avoidShadeTogetherPairs = {
  ["2,3"] = true, ["2,7"] = true, ["3,7"] = true,
}

local colour08BlendAllowedPairs = {
  ["3,8"] = true, ["7,8"] = true,
}

local pairCompatRating = {
  ["3,15"] = 0.95, ["3,11"] = 0.95, ["3,9"] = 0.95, ["1,3"] = 0.70,
  ["4,6"] = 0.95, ["5,6"] = 0.35, ["6,7"] = 0.95, ["6,12"] = 0.95,
  ["6,14"] = 0.95, ["6,15"] = 0.70,
  ["2,14"] = 0.95, ["2,15"] = 0.95, ["2,6"] = 0.35,
  ["5,12"] = 0.95, ["5,9"] = 0.35, ["4,5"] = 0.85, ["3,5"] = 0.70,
  ["5,7"] = 0.70, ["1,5"] = 0.70, ["5,8"] = 0.70, -- note: 08 still constrained by policy below
}

local function compat16(a, b, allow08)
  if type(a) ~= "number" or type(b) ~= "number" then return 0.5 end
  a = math.floor(a); b = math.floor(b)
  if a == b then return 1.0 end
  local lo, hi = a, b
  if lo > hi then lo, hi = hi, lo end
  local key = tostring(lo) .. "," .. tostring(hi)

  -- 08 policy dominates (unless user override).
  if (lo == 8 or hi == 8) and not allow08 then
    if colour08BlendAllowedPairs[key] then return 0.70 end
    return 0.00
  end

  -- Rule 5 dominates.
  if avoidShadeTogetherPairs[key] then return 0.00 end

  -- Safe alike pairs.
  if safeAlikePairs[key] then return 0.90 end

  local v = pairCompatRating[key]
  if v ~= nil then return v end

  return 0.50
end

local function is_bright_idx(idx)
  if type(idx) ~= "number" then return false end
  return luminance(idx) >= (bright_threshold or 0.65)
end

local function is_whiteish_idx(idx)
  if type(idx) ~= "number" then return false end
  return luminance(idx) >= (rule7_white_threshold or 0.92)
end

local function colour_class(idx, is_classic16)
  if type(idx) ~= "number" then return "neutral" end
  idx = math.floor(idx)
  if is_classic16 and idx >= 0 and idx <= 15 then
    return colourClass16[idx] or "neutral"
  end
  local r, g, b, ok = idx_to_rgb(idx)
  if not ok then return "neutral" end
  local _, s, v = rgb_to_hsv(r, g, b)
  if s < 0.10 then
    return "soft"
  end
  if v > 0.75 and s > 0.45 then
    return "hard"
  end
  return "soft"
end

local function compat(a, b, is_classic16, allow08)
  if type(a) ~= "number" or type(b) ~= "number" then return 0.5 end
  a = math.floor(a); b = math.floor(b)
  if a == b then return 1.0 end
  if is_classic16 and a >= 0 and a <= 15 and b >= 0 and b <= 15 then
    return compat16(a, b, allow08)
  end
  -- Conservative fallback: prefer close luminance + close hue, but never "perfect".
  local ar, ag, ab, ok1 = idx_to_rgb(a)
  local br, bg, bb, ok2 = idx_to_rgb(b)
  if not ok1 or not ok2 then
    return 0.50
  end
  local ah, as, av = rgb_to_hsv(ar, ag, ab)
  local bh, bs, bv = rgb_to_hsv(br, bg, bb)
  local dh = math.abs(ah - bh)
  dh = math.min(dh, 1.0 - dh) -- circular
  local dy = math.abs(av - bv) -- value proxy
  local ds = math.abs(as - bs)
  local score = 1.0 - (0.55 * dy + 0.30 * dh + 0.15 * ds)
  score = clamp(score, 0.0, 0.95)
  -- Make "hard vs hard" slightly less eager.
  return score
end

-- -------------------------------------------------------------------------
-- Glyph roles / coverage model (font-aware mapping is out of scope for Lua; use defaults)
-- -------------------------------------------------------------------------
local COVER = {
  [" "] = 0.00,
  ["█"] = 1.00,
  ["▓"] = 0.75,
  ["▒"] = 0.50,
  ["░"] = 0.25,
  ["▀"] = 0.50,
  ["▄"] = 0.50,
  ["▌"] = 0.50,
  ["▐"] = 0.50,
  ["+"] = 0.28,
}

local function cover_of(g)
  g = norm_glyph(g)
  local c = COVER[g]
  if c ~= nil then return c end
  if is_blank_glyph(g) then return 0.0 end
  return 1.0
end

local function is_density_glyph(g)
  g = norm_glyph(g)
  return g == "░" or g == "▒" or g == "▓"
end

local function is_half_glyph(g)
  g = norm_glyph(g)
  return g == "▀" or g == "▄" or g == "▌" or g == "▐"
end

local function is_fillish(g)
  local c = cover_of(g)
  return c >= 0.70
end

local function is_structural_glyph(g)
  g = norm_glyph(g)
  if is_blank_glyph(g) then return false end
  if is_density_glyph(g) or is_half_glyph(g) or g == "█" then return false end
  return true
end

local function yhat(glyph, fg, bg)
  local cov = cover_of(glyph)
  if type(fg) ~= "number" and type(bg) ~= "number" then return 0.0 end
  if type(fg) ~= "number" then fg = bg end
  if type(bg) ~= "number" then bg = fg end
  local yfg = luminance(fg)
  local ybg = luminance(bg)
  return cov * yfg + (1.0 - cov) * ybg
end

-- Deterministic per-cell randomness, independent of scan order.
local function rand01(seed, x, y, salt)
  local s = hash32(seed or 1, x or 0, y or 0, salt or 0)
  return (s % 1000000) / 1000000.0
end

-- -------------------------------------------------------------------------
-- CPS mode defaults
-- -------------------------------------------------------------------------
local function mode_defaults(mode)
  local p = {
    transitionWidth = 1,
    enableToonFill = false,
    enableHalfEdge = true,
    enableTransition = true,
    enableFastDrop = true,
    enableHalshade = false,
    enableBorder = false,
    enableReverse = false,
    enablePlus = false,
    enableChroma = false,
    enablePnakotic = false,
    enableBgFeather = false,
    relaxPasses = 1,

    thinThicknessMin = 2,
    thinDetailMin = 0.50,
    t0 = 1, t1 = 3,
    compatShadedThreshold = 0.75,
    maxStraightHighlightRun = 3,
    maxCandidatesPerCell = 36,
    -- Halshade density-detail cap (Halaster: ░▒▓ are detail work).
    maxDensityDetailRatio = 0.15,
    densityDetailWindowW = 8,
    densityDetailWindowH = 8,

    -- Chroma histogram settings.
    chromaMinCount = 6,
    chromaHistRadius = 4,

    -- Background blend width (outside of silhouettes).
    bgBlendWidth = 3,

    -- Weights (from spec defaults; tools can override per mode).
    weights = {
      pen_light = 1000,
      pen_overshade = 800,
      pen_half_continuity = 1200, -- veto-like
      pen_rule7_adj = 500,
      pen_bright_edge = 500,
      pen_compat = 300,
      pen_08 = 600,
      pen_curve = 150,
      pen_no_straight_highlights = 1200, -- veto-like
      pen_rand_palette = 1200, -- veto-like (enforced in generator)
      pen_detail_budget = 600,
      pen_stability = 50,
    },
  }

  if mode == "toon" then
    p.transitionWidth = 0
    p.enableToonFill = true
    p.enableHalfEdge = false
    p.enableTransition = false
    p.enableFastDrop = false
    -- Toon stage: overshading isn't relevant; stabilize a bit.
    p.weights.pen_overshade = 0
    p.weights.pen_stability = 25
  elseif mode == "clean" then
    p.transitionWidth = 1
  elseif mode == "clean_plus" then
    p.transitionWidth = 2
    -- Slightly softer overshading penalty inside band.
    p.weights.pen_overshade = 650
  elseif mode == "halshade" then
    p.transitionWidth = 1
    p.enableHalshade = true
    p.enableBorder = true
    p.enableReverse = true
    -- Halshade: keep density as detail-only; strong highlight-line veto.
    p.weights.pen_no_straight_highlights = 2000
  elseif mode == "plus_chroma" then
    p.transitionWidth = 1
    p.enablePlus = true
    p.enableChroma = true
    p.enableReverse = true
    p.weights.pen_rand_palette = 2000
  elseif mode == "pnakotic" then
    p.transitionWidth = 1
    p.enablePnakotic = true
  elseif mode == "softblend" then
    p.transitionWidth = 4
    -- SoftBlend reduces overshade penalty but not to zero.
    p.weights.pen_overshade = 350
  elseif mode == "bg_blend" then
    p.transitionWidth = 3
    p.bgBlendWidth = 3
    p.enableTransition = false
    p.enableBgFeather = true
    p.enableHalfEdge = false
    p.weights.pen_bright_edge = 700
  end

  return p
end

-- -------------------------------------------------------------------------
-- Footprint analysis (region, boundary, distance, thickness, local palette histogram)
-- -------------------------------------------------------------------------
local function key_xy(x, y)
  return y * 65536 + x
end

local function get_allowed_palette(ctx)
  local pal = ctx and ctx.palette or nil
  if type(pal) == "table" and #pal > 0 then
    return pal, true
  end
  -- IMPORTANT:
  -- If the host did not provide an explicit palette list, do NOT invent a [0..15] allowed set.
  -- That would silently clamp custom palettes down to ANSI-16.
  return nil, false
end

local function build_allowed_set(allowed)
  local s = {}
  if type(allowed) == "table" then
    for i = 1, #allowed do
      local v = allowed[i]
      if type(v) == "number" then
        s[math.floor(v)] = true
      end
    end
  end
  return s
end

local function snap_to_allowed(idx, allowed, allowed_set)
  if type(idx) ~= "number" then return nil end
  idx = math.floor(idx)
  -- If there is no allowed palette restriction, pass through unchanged.
  if type(allowed) ~= "table" or #allowed == 0 then
    return idx
  end
  if allowed_set and allowed_set[idx] then
    return idx
  end
  -- Nearest by luminance (cheap, stable).
  local by = luminance(idx)
  local best = nil
  local best_d = nil
  for i = 1, #allowed do
    local a = allowed[i]
    if type(a) == "number" then
      local d = math.abs(luminance(a) - by)
      if best_d == nil or d < best_d then
        best_d = d
        best = a
      end
    end
  end
  return best
end

local function darkest_allowed(allowed)
  if type(allowed) ~= "table" or #allowed == 0 then return nil end
  local best = nil
  local best_y = nil
  for i = 1, #allowed do
    local a = allowed[i]
    if type(a) == "number" then
      local y = luminance(a)
      if best_y == nil or y < best_y then
        best_y = y
        best = a
      end
    end
  end
  return best
end

local function brightest_allowed(allowed)
  if type(allowed) ~= "table" or #allowed == 0 then return nil end
  local best = nil
  local best_y = nil
  for i = 1, #allowed do
    local a = allowed[i]
    if type(a) == "number" then
      local y = luminance(a)
      if best_y == nil or y > best_y then
        best_y = y
        best = a
      end
    end
  end
  return best
end

local function sample_cell(ctx, layer, x, y, sample_mode)
  -- Returns: ch, fg, bg, attrs (attrs may be nil if unavailable)
  local ch, fg, bg, attrs = nil, nil, nil, nil
  local canvas = ctx and ctx.canvas or nil
  if canvas and canvas.getCell then
    local cch, cfg, cbg, _, cattrs = canvas:getCell(x, y, sample_mode or "layer")
    ch = norm_glyph(cch)
    if type(cfg) == "number" then fg = math.floor(cfg) end
    if type(cbg) == "number" then bg = math.floor(cbg) end
    if type(cattrs) == "number" then attrs = math.floor(cattrs) end
    return ch, fg, bg, attrs
  end
  local cch, cfg, cbg = layer:get(x, y)
  ch = norm_glyph(cch)
  if type(cfg) == "number" then fg = math.floor(cfg) end
  if type(cbg) == "number" then bg = math.floor(cbg) end
  return ch, fg, bg, nil
end

local function dominant_colour(ch, fg, bg)
  -- Pick a stable "region colour key".
  if type(fg) ~= "number" and type(bg) ~= "number" then return nil end
  if type(fg) ~= "number" then return bg end
  if type(bg) ~= "number" then return fg end
  local cov = cover_of(ch)
  if cov >= 0.5 then return fg end
  return bg
end

local function is_filled_cell(ch, fg, bg)
  if type(fg) ~= "number" and type(bg) ~= "number" then
    return not is_blank_glyph(ch)
  end
  if is_blank_glyph(ch) then
    -- A coloured-space is treated as filled.
    return (type(bg) == "number")
  end
  if is_density_glyph(ch) or is_half_glyph(ch) or ch == "█" then
    return true
  end
  return cover_of(ch) >= 0.60
end

local function analyze_footprint(ctx, layer, cx, cy, r, stroke_dx, stroke_dy, light_dx, light_dy, params, sample_mode)
  -- Analyze a (2r+1)^2 footprint centered at (cx,cy), plus a 1-cell margin for boundary detection.
  local cols = to_int(ctx.cols, 0)
  local rows = to_int(ctx.rows, 0)
  local default_bg = nil
  if type(ctx.bg) == "number" then default_bg = math.floor(ctx.bg) end
  local x0 = cx - r - 1
  local x1 = cx + r + 1
  local y0 = cy - r - 1
  local y1 = cy + r + 1

  local cell = {} -- key -> {ch, fg, bg, attrs, filled, dom}
  for y = y0, y1 do
    if y >= 0 and (rows <= 0 or y < rows) then
      for x = x0, x1 do
        if x >= 0 and x < cols then
          local ch, fg, bg, attrs = sample_cell(ctx, layer, x, y, sample_mode)
          local filled = is_filled_cell(ch, fg, bg)
          local dom = dominant_colour(ch, fg, bg)
          cell[key_xy(x, y)] = { ch = ch, fg = fg, bg = bg, attrs = attrs, filled = filled, dom = dom }
        end
      end
    end
  end

  -- Flood-fill regions by dominant colour (4-connected) inside the extended footprint.
  local region = {} -- key -> regionId (int)
  local region_dom = {} -- regionId -> dom colour
  local next_id = 1
  local qx, qy = {}, {}

  local function can_join(a, b)
    if not a or not b then return false end
    if a.filled ~= true or b.filled ~= true then return false end
    if a.dom == nil or b.dom == nil then return false end
    return a.dom == b.dom
  end

  for y = y0, y1 do
    for x = x0, x1 do
      local k = key_xy(x, y)
      if region[k] == nil then
        local c = cell[k]
        if c and c.filled and c.dom ~= nil then
          local rid = next_id
          next_id = next_id + 1
          region_dom[rid] = c.dom
          region[k] = rid
          local head, tail = 1, 1
          qx[1], qy[1] = x, y
          while head <= tail do
            local px, py = qx[head], qy[head]; head = head + 1
            local pk = key_xy(px, py)
            local pc = cell[pk]
            if pc then
              local nbs = {
                { px - 1, py }, { px + 1, py }, { px, py - 1 }, { px, py + 1 },
              }
              for i = 1, #nbs do
                local nx, ny = nbs[i][1], nbs[i][2]
                local nk = key_xy(nx, ny)
                if region[nk] == nil then
                  local nc = cell[nk]
                  if can_join(pc, nc) then
                    region[nk] = rid
                    tail = tail + 1
                    qx[tail], qy[tail] = nx, ny
                  end
                end
              end
            end
          end
        end
      end
    end
  end

  -- Boundary mask on the core footprint (cx±r,cy±r).
  -- IMPORTANT: include silhouette boundaries (filled region adjacent to empty/background).
  local boundary = {}
  for y = cy - r, cy + r do
    if y >= 0 and (rows <= 0 or y < rows) then
      for x = cx - r, cx + r do
        if x >= 0 and x < cols then
          local k = key_xy(x, y)
          local rid = region[k]
          local b = false
          if rid ~= nil then
            local nbs = { key_xy(x - 1, y), key_xy(x + 1, y), key_xy(x, y - 1), key_xy(x, y + 1) }
            for i = 1, #nbs do
              local nr = region[nbs[i]]
              if nr == nil or nr ~= rid then
                b = true
                break
              end
            end
          end
          boundary[k] = b
        end
      end
    end
  end

  -- Distance-to-boundary within each region (multi-source BFS).
  local dist = {}
  local q = {}
  local qi, qj = 1, 0
  for y = cy - r, cy + r do
    if y >= 0 and (rows <= 0 or y < rows) then
      for x = cx - r, cx + r do
        if x >= 0 and x < cols then
          local k = key_xy(x, y)
          if region[k] ~= nil then
            if boundary[k] then
              dist[k] = 0
              qj = qj + 1
              q[qj] = { x = x, y = y }
            else
              dist[k] = 1e9
            end
          end
        end
      end
    end
  end

  while qi <= qj do
    local cur = q[qi]; qi = qi + 1
    local x, y = cur.x, cur.y
    local k = key_xy(x, y)
    local rid = region[k]
    local d0 = dist[k] or 1e9
    local nbs = { { x - 1, y }, { x + 1, y }, { x, y - 1 }, { x, y + 1 } }
    for i = 1, #nbs do
      local nx, ny = nbs[i][1], nbs[i][2]
      local nk = key_xy(nx, ny)
      if region[nk] == rid and dist[nk] ~= nil then
        local nd = dist[nk]
        if d0 + 1 < nd then
          dist[nk] = d0 + 1
          qj = qj + 1
          q[qj] = { x = nx, y = ny }
        end
      end
    end
  end

  -- Edge normal: outward-pointing vector from region interior toward "outside".
  -- This is used for lit-side decisions and half-block continuity.
  local normal = {}
  for y = cy - r, cy + r do
    if y >= 0 and (rows <= 0 or y < rows) then
      for x = cx - r, cx + r do
        if x >= 0 and x < cols then
          local k = key_xy(x, y)
          local rid = region[k]
          if boundary[k] and rid ~= nil then
            local gx, gy = 0, 0
            local nbs = {
              { x - 1, y, -1, 0 }, { x + 1, y, 1, 0 }, { x, y - 1, 0, -1 }, { x, y + 1, 0, 1 },
            }
            for i = 1, #nbs do
              local nx, ny, dx, dy = nbs[i][1], nbs[i][2], nbs[i][3], nbs[i][4]
              local nr = region[key_xy(nx, ny)]
              if nr == nil or nr ~= rid then
                gx = gx + dx
                gy = gy + dy
              end
            end
            normal[k] = { gx, gy }
          else
            normal[k] = { 0, 0 }
          end
        end
      end
    end
  end

  -- Per-region max interior distance (proxy for thickness of the feature).
  local region_max_dist = {}
  for y = cy - r, cy + r do
    if y >= 0 and (rows <= 0 or y < rows) then
      for x = cx - r, cx + r do
        if x >= 0 and x < cols then
          local k = key_xy(x, y)
          local rid = region[k]
          local d = dist[k]
          if rid ~= nil and d ~= nil and d < 1e8 then
            local m = region_max_dist[rid]
            if m == nil or d > m then region_max_dist[rid] = d end
          end
        end
      end
    end
  end

  -- Local palette histogram for "random shade" constraints (window 9x9).
  -- Only compute when the active mode uses chroma.
  local hist = nil
  if params and params.enableChroma then
    hist = {}
    local hw = clamp(to_int(params.chromaHistRadius, 4), 1, 20)
    for y = cy - r, cy + r do
      if y >= 0 and (rows <= 0 or y < rows) then
        for x = cx - r, cx + r do
          if x >= 0 and x < cols then
            local k = key_xy(x, y)
            local h = {}
            for yy = y - hw, y + hw do
              for xx = x - hw, x + hw do
                local c = cell[key_xy(xx, yy)]
                if c and c.dom ~= nil then
                  h[c.dom] = (h[c.dom] or 0) + 1
                end
              end
            end
            hist[k] = h
          end
        end
      end
    end
  end

  -- Stroke tangent / light direction.
  local tdx, tdy = stroke_dx or 0, stroke_dy or 0
  local tlen = math.sqrt(tdx * tdx + tdy * tdy)
  if tlen > 1e-6 then
    tdx, tdy = tdx / tlen, tdy / tlen
  else
    tdx, tdy = 1.0, 0.0
  end

  local ldx, ldy = light_dx or 1.0, light_dy or 0.0
  local llen = math.sqrt(ldx * ldx + ldy * ldy)
  if llen > 1e-6 then
    ldx, ldy = ldx / llen, ldy / llen
  else
    ldx, ldy = 1.0, 0.0
  end

  -- Per-cell lighting scalar and detail budget.
  local L = {}
  local T = {}
  local D = {}
  for y = cy - r, cy + r do
    if y >= 0 and (rows <= 0 or y < rows) then
      for x = cx - r, cx + r do
        if x >= 0 and x < cols then
          local k = key_xy(x, y)
          local n = normal[k] or { 0, 0 }
          local nx, ny = n[1], n[2]
          local nd = math.sqrt(nx * nx + ny * ny)
          local ldir = 0.0
          if nd > 1e-6 then
            nx, ny = nx / nd, ny / nd
            ldir = nx * ldx + ny * ldy
          else
            -- fallback: stroke normal (perp tangent)
            local px, py = -tdy, tdx
            ldir = px * ldx + py * ldy
          end
          local lc = clamp(0.5 + 0.5 * ldir, 0.0, 1.0)
          L[k] = lc

          -- Thickness proxy: use dist-to-boundary.
          local thick = dist[k]
          if thick == nil or thick > 1e8 then thick = 0 end
          local d = clamp((thick - params.t0) / (params.t1 - params.t0), 0.0, 1.0)
          D[k] = d

          -- Tone target delta (filled in by caller based on intent/strength).
          T[k] = 0.0
        end
      end
    end
  end

  -- Outside-of-silhouette boundary + distance (for bg_blend).
  -- outside_boundary: cells NOT in a filled region but adjacent to one.
  local outside_boundary = {}
  local outside_in_dom = {}
  local outside_dist = {}
  do
    for y = cy - r, cy + r do
      if y >= 0 and (rows <= 0 or y < rows) then
        for x = cx - r, cx + r do
          if x >= 0 and x < cols then
            local k = key_xy(x, y)
            if region[k] == nil then
              local nbs = {
                { x - 1, y }, { x + 1, y }, { x, y - 1 }, { x, y + 1 },
              }
              local hit = false
              for i = 1, #nbs do
                local nk = key_xy(nbs[i][1], nbs[i][2])
                local nr = region[nk]
                if nr ~= nil then
                  outside_boundary[k] = true
                  outside_in_dom[k] = region_dom[nr]
                  hit = true
                  break
                end
              end
              if not hit then
                outside_boundary[k] = false
              end
            end
          end
        end
      end
    end

    -- BFS outward in non-region space (multi-source from outside_boundary).
    local oq = {}
    local oi, oj = 1, 0
    for y = cy - r, cy + r do
      if y >= 0 and (rows <= 0 or y < rows) then
        for x = cx - r, cx + r do
          if x >= 0 and x < cols then
            local k = key_xy(x, y)
            if region[k] == nil then
              if outside_boundary[k] then
                outside_dist[k] = 0
                oj = oj + 1
                oq[oj] = { x = x, y = y }
              else
                outside_dist[k] = 1e9
              end
            end
          end
        end
      end
    end

    while oi <= oj do
      local cur = oq[oi]; oi = oi + 1
      local x, y = cur.x, cur.y
      local k = key_xy(x, y)
      local d0 = outside_dist[k] or 1e9
      local nbs = { { x - 1, y }, { x + 1, y }, { x, y - 1 }, { x, y + 1 } }
      for i = 1, #nbs do
        local nx, ny = nbs[i][1], nbs[i][2]
        if nx >= cx - r and nx <= cx + r and ny >= cy - r and ny <= cy + r then
          if ny >= 0 and (rows <= 0 or ny < rows) and nx >= 0 and nx < cols then
            local nk = key_xy(nx, ny)
            if region[nk] == nil and outside_dist[nk] ~= nil then
              local nd = outside_dist[nk]
              if d0 + 1 < nd then
                outside_dist[nk] = d0 + 1
                -- propagate "inside colour" outward (first-hit is fine for a local feather).
                if outside_in_dom[nk] == nil then
                  outside_in_dom[nk] = outside_in_dom[k]
                end
                oj = oj + 1
                oq[oj] = { x = nx, y = ny }
              end
            end
          end
        end
      end
    end
  end

  return {
    cell = cell,
    region = region,
    region_dom = region_dom,
    region_max_dist = region_max_dist,
    boundary = boundary,
    dist = dist,
    normal = normal,
    hist = hist,
    L = L,
    T = T,
    D = D,
    outside_boundary = outside_boundary,
    outside_dist = outside_dist,
    outside_in_dom = outside_in_dom,
    default_bg = default_bg,
    x0 = cx - r, x1 = cx + r, y0 = cy - r, y1 = cy + r,
  }
end

-- -------------------------------------------------------------------------
-- CPS candidate generation + scoring
-- -------------------------------------------------------------------------
local function shaded_together(a, b, is_classic16, allow08, compatShadedThreshold)
  if type(a) ~= "number" or type(b) ~= "number" then return false end
  if a == b then return true end
  local lo, hi = a, b
  if lo > hi then lo, hi = hi, lo end
  local key = tostring(lo) .. "," .. tostring(hi)
  if is_classic16 and safeAlikePairs[key] then return true end
  return compat(a, b, is_classic16, allow08) >= (compatShadedThreshold or 0.75)
end

local function pick_neighbor_other(fields, x, y)
  local k = key_xy(x, y)
  local rid = fields.region[k]
  if rid == nil then return nil, nil end
  local nbs = {
    { x - 1, y, -1, 0 }, { x + 1, y, 1, 0 }, { x, y - 1, 0, -1 }, { x, y + 1, 0, 1 },
  }
  for i = 1, #nbs do
    local nx, ny, dx, dy = nbs[i][1], nbs[i][2], nbs[i][3], nbs[i][4]
    local nk = key_xy(nx, ny)
    local nr = fields.region[nk]
    if nr ~= nil and nr ~= rid then
      local other_dom = fields.region_dom[nr]
      return other_dom, { dx = dx, dy = dy }
    end
    -- Silhouette: neighbor is not in a filled region (treat as background side).
    if nr == nil then
      local nc = fields.cell[nk]
      local other_dom = (nc and nc.dom) or fields.default_bg
      if type(other_dom) == "number" then
        return other_dom, { dx = dx, dy = dy }
      end
    end
  end
  return nil, nil
end

local function choose_half_glyph(dx, dy)
  -- dx,dy points from inside cell towards outside neighbor.
  if math.abs(dx) >= math.abs(dy) then
    if dx > 0 then return "▌" end -- outside on right => inside is left => left-half ink
    return "▐" -- outside on left => inside is right => right-half ink
  end
  if dy > 0 then return "▀" end -- outside below => inside above => top-half ink
  return "▄" -- outside above => inside below => bottom-half ink
end

local function half_ink_side(g)
  -- Returns a vector pointing from center toward the INK half.
  if g == "▌" then return -1, 0 end
  if g == "▐" then return 1, 0 end
  if g == "▀" then return 0, -1 end
  if g == "▄" then return 0, 1 end
  return 0, 0
end

local function clamp_candidates(cands, maxn)
  if #cands <= maxn then return cands end
  local out = {}
  for i = 1, maxn do out[i] = cands[i] end
  return out
end

local function merge_write(cur, w)
  local out = {
    ch = cur.ch,
    fg = cur.fg,
    bg = cur.bg,
    attrs = cur.attrs,
  }
  if w.ch ~= nil then out.ch = w.ch end
  if w.fg ~= nil then out.fg = w.fg end
  if w.bg ~= nil then out.bg = w.bg end
  if w.attrs ~= nil then out.attrs = w.attrs end
  return out
end

local function pen_no_straight_highlights(fields, proposed, x, y, new_dom, is_bright, max_run)
  if not is_bright then return 0.0 end
  max_run = max_run or 3
  local dirs = {
    {1,0}, {0,1}, {1,1}, {1,-1},
    {-1,0}, {0,-1}, {-1,-1}, {-1,1},
  }

  local function dom_at(xx, yy)
    local k = key_xy(xx, yy)
    local p = proposed[k]
    if p ~= nil then
      return p.dom
    end
    local c = fields.cell[k]
    return c and c.dom or nil
  end

  local best = 1
  for i = 1, 4 do
    local dx, dy = dirs[i][1], dirs[i][2]
    local run = 1
    for step = 1, max_run + 2 do
      local dd = dom_at(x + dx * step, y + dy * step)
      if dd == nil then break end
      if is_bright_idx(dd) then run = run + 1 else break end
    end
    for step = 1, max_run + 2 do
      local dd = dom_at(x - dx * step, y - dy * step)
      if dd == nil then break end
      if is_bright_idx(dd) then run = run + 1 else break end
    end
    if run > best then best = run end
  end
  if best > max_run then
    return math.huge
  end
  return 0.0
end

-- Forward declaration: Lua local functions are not visible to earlier code unless declared first.
-- (Without this, `score_cell()` would resolve `curve_penalty` as a global and crash at runtime.)
local curve_penalty = nil
local density_ratio = nil

local function score_cell(fields, params, proposed, x, y, cur_state, cand, intent_sign, strength, is_classic16, allow08, hard_textures, allowed, allowed_set)
  local k = key_xy(x, y)
  local bmask = fields.boundary[k] == true
  local dist = fields.dist[k] or 1e9
  local rid = fields.region[k]
  local rmax = (rid and fields.region_max_dist and fields.region_max_dist[rid]) or 0
  local thin_feature = (rmax < (params.thinThicknessMin or 2)) or ((fields.D[k] or 0.0) < (params.thinDetailMin or 0.50))
  local W = params.weights or {}
  local pen = 0.0

  -- Merge candidate into current.
  local w = merge_write(cur_state, cand)

  -- Dominant colour for adjacency reasoning.
  local dom = dominant_colour(w.ch, w.fg, w.bg)
  if dom == nil then dom = cur_state.dom end

  -- Snap computed colours to allowed palette if host provided a restriction.
  if type(w.fg) == "number" then w.fg = snap_to_allowed(w.fg, allowed, allowed_set) end
  if type(w.bg) == "number" then w.bg = snap_to_allowed(w.bg, allowed, allowed_set) end
  dom = dominant_colour(w.ch, w.fg, w.bg) or dom

  -- Overshading guardrail.
  if is_density_glyph(w.ch) then
    if dist > params.transitionWidth then
      return math.huge
    end
  end

  -- Halshade: density glyphs are detail-only (cap density usage per window).
  if params.enableHalshade and is_density_glyph(w.ch) then
    local ratio = density_ratio(fields, proposed, x, y, params.densityDetailWindowW, params.densityDetailWindowH)
    if ratio > (params.maxDensityDetailRatio or 0.15) then
      return math.huge
    end
  end

  -- Thin-feature hard gating.
  if thin_feature then
    if cand.tag == "swirl" or cand.tag == "pit" or cand.tag == "sparkle" or cand.tag == "plus" or cand.tag == "chroma" or cand.tag == "reverse" or cand.tag == "pnakotic" then
      return math.huge
    end
  end

  -- Detail budget penalty (beyond the hard thin-feature gate).
  do
    local d = fields.D[k] or 0.0
    if d < 0.85 then
      if cand.tag == "swirl" or cand.tag == "pit" or cand.tag == "sparkle" or cand.tag == "plus" or cand.tag == "chroma" or cand.tag == "pnakotic" then
        local wgt = W.pen_detail_budget or 600
        -- Ramp up as detail budget shrinks.
        local t = (0.85 - d) / 0.85
        if t < 0 then t = 0 end
        if t > 1 then t = 1 end
        pen = pen + (t * wgt)
      end
    end
  end

  -- Hard-colour clamp: suppress texture on hard colours unless allowed.
  local base_dom = cur_state.dom
  local base_class = colour_class(base_dom, is_classic16)
  if (base_class == "hard") and (not hard_textures) then
    if cand.tag == "swirl" or cand.tag == "pit" or cand.tag == "sparkle" or cand.tag == "plus" or cand.tag == "chroma" or cand.tag == "pnakotic" then
      return math.huge
    end
  end

  -- 08 policy for transitions (unless override enabled).
  if (cand.tag == "transition" or cand.tag == "half_edge" or cand.tag == "reverse") and (not allow08) then
    local fg, bg = w.fg, w.bg
    if type(fg) == "number" and type(bg) == "number" then
      local lo, hi = fg, bg
      if lo > hi then lo, hi = hi, lo end
      local kk = tostring(lo) .. "," .. tostring(hi)
      if (lo == 8 or hi == 8) and (not colour08BlendAllowedPairs[kk]) then
        return math.huge
      end
    end
  end

  -- Light direction consistency (strong).
  local y0 = yhat(cur_state.ch, cur_state.fg, cur_state.bg)
  local y1 = yhat(w.ch, w.fg, w.bg)
  local dc = y1 - y0
  if intent_sign ~= 0 then
    -- Rule 7 safety candidates are allowed to override the tone intent locally.
    if cand.tag ~= "rule7_sep" and cand.tag ~= "rule7_fade" then
      local t = fields.T[k] or 0.0
      local want = (t >= 0.0) and 1 or -1
      local got = (dc >= 0.02) and 1 or ((dc <= -0.02) and -1 or 0)
      if got ~= 0 and got ~= want then
        return math.huge
      end
    end
  end

  -- Compatibility penalty (mostly for mixed fg/bg).
  if type(w.fg) == "number" and type(w.bg) == "number" and w.fg ~= w.bg then
    local c = compat(w.fg, w.bg, is_classic16, allow08)
    pen = pen + (1.0 - c) * (W.pen_compat or 300)
  end

  -- Rule 7: unshaded bright adjacency (4-neighborhood).
  do
    local nbs = { { x - 1, y }, { x + 1, y }, { x, y - 1 }, { x, y + 1 } }
    for i = 1, #nbs do
      local nx, ny = nbs[i][1], nbs[i][2]
      local nk = key_xy(nx, ny)
      local nprop = proposed[nk]
      local ndom = nil
      if nprop ~= nil then
        ndom = nprop.dom
      else
        local nc = fields.cell[nk]
        ndom = nc and nc.dom or nil
      end
      if type(dom) == "number" and type(ndom) == "number" then
        local shaded = shaded_together(dom, ndom, is_classic16, allow08, params.compatShadedThreshold)
        -- Transition band exception: if we're right at a boundary, allow deliberate transitions
        -- without Rule 7 fighting them (spec: Rule 7 is for unshaded adjacency).
        if not shaded and (dist <= (params.transitionWidth or 1)) and rid ~= nil then
          local nr = fields.region[nk]
          if nr == nil or nr ~= rid then
            shaded = true
          end
        end
        if not shaded and is_bright_idx(dom) and is_bright_idx(ndom) then
          if rule7_white_exception and (is_whiteish_idx(dom) or is_whiteish_idx(ndom)) then
            -- "except maybe white"
          else
          pen = pen + (W.pen_rule7_adj or 500)
          end
        end
      end
    end
  end

  -- Bright edge suppression (avoid brightest-at-silhouette without separation).
  if bmask and type(dom) == "number" and is_bright_idx(dom) then
    local other_dom = select(1, pick_neighbor_other(fields, x, y))
    if type(other_dom) == "number" and not shaded_together(dom, other_dom, is_classic16, allow08, params.compatShadedThreshold) then
      pen = pen + (W.pen_bright_edge or 500)
    end
  end

  -- Half-block continuity: veto obvious "rim" inversions.
  if is_half_glyph(w.ch) and type(w.fg) == "number" and type(w.bg) == "number" then
    -- Use light direction (via fields.L) + outside-facing half to prevent rims:
    -- - darken: forbid bright outside-facing half when outside is in shadow
    -- - lighten: forbid dark outside-facing half when outside is lit
    local n = fields.normal[k] or { 0, 0 }
    local nx, ny = n[1] or 0, n[2] or 0
    local nd = math.sqrt(nx * nx + ny * ny)
    if nd > 1e-6 then
      nx, ny = nx / nd, ny / nd
    end
    -- Which half faces outside? (ink faces in direction of half_ink_side)
    local ix, iy = half_ink_side(w.ch)
    local ink_faces_outside = (ix * nx + iy * ny) > 0
    local yfg = luminance(w.fg)
    local ybg = luminance(w.bg)
    local y_out = ink_faces_outside and yfg or ybg
    local y_in = ink_faces_outside and ybg or yfg
    local ldir = (fields.L[k] or 0.5) * 2.0 - 1.0 -- normal·lightDir estimate in [-1,1]
    if intent_sign < 0 then
      -- Darken: avoid a bright rim on the shadow-facing outside.
      if ldir < -0.10 and (y_out > y_in + 0.05) then
        return math.huge
      end
    elseif intent_sign > 0 then
      -- Lighten: avoid a dark rim on the light-facing outside.
      if ldir > 0.10 and (y_out + 0.05 < y_in) then
        return math.huge
      end
    end
  end

  -- Curve progression penalty (lightweight).
  do
    local cp = curve_penalty(fields, proposed, x, y, w.ch)
    if cp > 0 then
      pen = pen + cp * (W.pen_curve or 150)
    end
  end

  -- Halaster: no straight highlight runs.
  if cand.tag == "border" or cand.tag == "sparkle" or cand.tag == "swirl" then
    local v = pen_no_straight_highlights(fields, proposed, x, y, dom, type(dom) == "number" and is_bright_idx(dom), params.maxStraightHighlightRun)
    if v == math.huge then return math.huge end
  end

  -- Stability: don't churn too much at low strength.
  do
    local churn = 0.0
    if w.ch ~= cur_state.ch then churn = churn + 1.0 end
    if w.fg ~= cur_state.fg then churn = churn + 1.0 end
    if w.bg ~= cur_state.bg then churn = churn + 1.0 end
    pen = pen + churn * ((W.pen_stability or 50) * (1.0 - strength))
  end

  -- Reward (small) for moving in desired tone direction.
  if intent_sign ~= 0 then
    local t = fields.T[k] or 0.0
    local want = (t >= 0.0) and 1 or -1
    local got = (dc >= 0.01) and 1 or ((dc <= -0.01) and -1 or 0)
    if got ~= 0 and got == want then
      pen = pen - 20.0 * math.abs(t)
    end
  end

  return pen
end

local function pick_toon_colour(allowed, base, want_dir, is_classic16)
  -- want_dir: -1 darken, +1 lighten
  local allowed_set = build_allowed_set(allowed)
  if type(base) ~= "number" then
    if type(allowed) == "table" and #allowed > 0 then
      base = allowed[1]
    else
      base = 0
    end
  end
  base = math.floor(base)
  if is_classic16 and base >= 0 and base <= 15 then
    if want_dir < 0 then
      -- darken: bias to black/08; keep hue if already dark.
      local choices = { 0, 8, 1, 4 }
      local best = base
      local by = luminance(base)
      for i = 1, #choices do
        local c = choices[i]
        local y = luminance(c)
        if y < by then
          best = c
          by = y
        end
      end
      return snap_to_allowed(best, allowed, allowed_set) or best
    else
      -- lighten: prefer alike bright pair, else white.
      local alike = { [1] = 9, [2] = 10, [3] = 11, [4] = 12, [5] = 13, [6] = 14, [7] = 15 }
      local b = alike[base]
      if b ~= nil then return snap_to_allowed(b, allowed, allowed_set) or b end
      return snap_to_allowed(15, allowed, allowed_set) or 15
    end
  end

  -- General palette: pick nearest brighter/darker by luminance within allowed.
  if type(allowed) ~= "table" or #allowed == 0 then
    return base
  end
  local by = luminance(base)
  local best = base
  local best_d = 1e9
  for i = 1, #allowed do
    local idx = allowed[i]
    if type(idx) == "number" then
      local y = luminance(idx)
      local dy = y - by
      if want_dir > 0 and dy > 0.01 then
        if dy < best_d then best_d = dy; best = idx end
      elseif want_dir < 0 and dy < -0.01 then
        if -dy < best_d then best_d = -dy; best = idx end
      end
    end
  end
  return best
end

local function is_classic16_palette(ctx, allowed)
  local is_builtin = (ctx and ctx.palette_is_builtin) == true
  local b = to_int(ctx and ctx.palette_builtin, 0)
  -- phos::colour::BuiltinPalette:
  --   Vga16 = 1, Xterm16 = 3
  if is_builtin and (b == 1 or b == 3) then
    return true
  end
  if type(allowed) == "table" and #allowed == 16 then
    local set = build_allowed_set(allowed)
    for i = 0, 15 do
      if not set[i] then return false end
    end
    return true
  end
  return false
end

density_ratio = function(fields, proposed, x, y, ww, hh)
  ww = ww or 8
  hh = hh or 8
  local rx = math.floor(ww / 2)
  local ry = math.floor(hh / 2)
  local total = 0
  local dens = 0

  for yy = y - ry, y + ry do
    for xx = x - rx, x + rx do
      local k = key_xy(xx, yy)
      local ch = nil
      local p = proposed[k]
      if p ~= nil and p.write then
        ch = p.write.ch
      else
        local c = fields.cell[k]
        ch = c and c.ch or nil
      end
      if ch ~= nil then
        total = total + 1
        if is_density_glyph(ch) then dens = dens + 1 end
      end
    end
  end
  if total <= 0 then return 0.0 end
  return dens / total
end

curve_penalty = function(fields, proposed, x, y, cand_ch)
  -- Lightweight approximation of pen_curve:
  -- On boundaries, prefer consistent half-edge orientation along the boundary tangent.
  local k = key_xy(x, y)
  if fields.boundary[k] ~= true then return 0.0 end
  if not is_half_glyph(cand_ch) then return 0.0 end

  local n = fields.normal[k] or { 0, 0 }
  local nx, ny = n[1], n[2]
  if (nx == 0 and ny == 0) then return 0.0 end

  -- Tangent is perpendicular to normal.
  local tx, ty = -ny, nx
  -- Choose a dominant tangent axis.
  if math.abs(tx) >= math.abs(ty) then
    tx = (tx >= 0) and 1 or -1
    ty = 0
  else
    ty = (ty >= 0) and 1 or -1
    tx = 0
  end

  local function half_kind_at(xx, yy)
    local kk = key_xy(xx, yy)
    local p = proposed[kk]
    local ch = nil
    if p and p.write then ch = p.write.ch else
      local c = fields.cell[kk]; ch = c and c.ch or nil
    end
    if not is_half_glyph(ch) then return nil end
    -- Kind: horizontal (▀/▄) vs vertical (▌/▐)
    if ch == "▀" or ch == "▄" then return "h" end
    return "v"
  end

  local my_kind = (cand_ch == "▀" or cand_ch == "▄") and "h" or "v"
  local prev_kind = half_kind_at(x - tx, y - ty)
  local next_kind = half_kind_at(x + tx, y + ty)

  -- Penalize alternating patterns like h-v-h along tangent.
  if prev_kind and next_kind and prev_kind == next_kind and prev_kind ~= my_kind then
    return 1.0
  end
  return 0.0
end

local function gen_candidates(ctx, fields, params, allowed, allowed_set, dab_seed, x, y, intent_sign, strength, is_classic16, allow08)
  local k = key_xy(x, y)
  local c = fields.cell[k]
  if not c then return {} end

  local cur = {
    ch = c.ch,
    fg = c.fg,
    bg = c.bg,
    attrs = c.attrs,
    dom = c.dom,
  }

  local dist = fields.dist[k] or 1e9
  local bmask = fields.boundary[k] == true
  local rid = fields.region[k]
  local rmax = (rid and fields.region_max_dist and fields.region_max_dist[rid]) or 0
  local thin_feature = (rmax < (params.thinThicknessMin or 2)) or ((fields.D[k] or 0.0) < (params.thinDetailMin or 0.50))
  allowed_set = allowed_set or build_allowed_set(allowed)

  -- Local transition width clamp for hard colours (tutorial: hard colours don't shade well).
  local tw = params.transitionWidth
  if type(cur.dom) == "number" then
    local cls = colour_class(cur.dom, is_classic16)
    if cls == "hard" and tw > 1 then tw = 1 end
  end
  if thin_feature and tw > 1 then tw = 1 end

  local cands = {}
  cands[#cands + 1] = { tag = "keep" } -- merge_write uses cur for missing fields

  -- Stage A: toon fill (contrast mask).
  if params.enableToonFill then
    local want = 0
    local t = fields.T[k] or 0.0
    if intent_sign < 0 then want = (t <= 0) and -1 or 0
    elseif intent_sign > 0 then want = (t >= 0) and 1 or 0 end
    if want ~= 0 then
      local glyph = is_structural_glyph(cur.ch) and cur.ch or "█"
      local new_fg = pick_toon_colour(allowed, cur.dom, want, is_classic16)
      cands[#cands + 1] = { ch = glyph, fg = new_fg, tag = "toon_fill" }
    else
      -- still allow collapsing non-structural to fill in toon mode at low strength
      if not is_structural_glyph(cur.ch) and cur.ch ~= "█" then
        cands[#cands + 1] = { ch = "█", tag = "toon_fill" }
      end
    end
  end

  -- Half-edge continuity (boundary only).
  if params.enableHalfEdge and bmask then
    local other_dom, odir = pick_neighbor_other(fields, x, y)
    if type(other_dom) == "number" and odir ~= nil and type(cur.dom) == "number" then
      local hg = choose_half_glyph(odir.dx, odir.dy)
      -- Inside region colour goes to the ink half; outside to the other half.
      cands[#cands + 1] = { ch = hg, fg = cur.dom, bg = other_dom, tag = "half_edge" }
    end
  end

  -- Stage B: transitions in band (distance 1..transitionWidth).
  if params.enableTransition and dist >= 1 and dist <= tw then
    local other_dom = select(1, pick_neighbor_other(fields, x, y))
    if type(other_dom) == "number" and type(cur.dom) == "number" then
      local glyphs = { "░", "▒", "▓" }
      for i = 1, #glyphs do
        cands[#cands + 1] = { ch = glyphs[i], fg = cur.dom, bg = other_dom, tag = "transition" }
        cands[#cands + 1] = { ch = glyphs[i], fg = other_dom, bg = cur.dom, tag = "transition" }
      end

      -- Solo density (Soth caution): default allow {1,8} and optionally {4} in classic.
      if is_classic16 then
        local d = cur.dom
        if d == 1 or d == 8 or d == 4 then
          local z = snap_to_allowed(0, allowed, allowed_set)
          cands[#cands + 1] = { ch = "░", fg = d, bg = z, tag = "transition" }
          cands[#cands + 1] = { ch = "▒", fg = d, bg = z, tag = "transition" }
        end
      end

      -- Fast drop (skip-step) at strong intent: jump tone without midtone ladder.
      if params.enableFastDrop and math.abs((fields.T[k] or 0.0)) >= 0.60 then
        local want = (intent_sign < 0) and -1 or 1
        local jump = pick_toon_colour(allowed, cur.dom, want, is_classic16)
        cands[#cands + 1] = { ch = is_structural_glyph(cur.ch) and cur.ch or "█", fg = jump, tag = "fast_drop" }
      end
    end
  end

  -- Rule 7 fix candidates: separator line + edge fade (only on boundaries).
  if bmask and type(cur.dom) == "number" then
    local other_dom = select(1, pick_neighbor_other(fields, x, y))
    if type(other_dom) == "number" then
      local shaded = shaded_together(cur.dom, other_dom, is_classic16, allow08, params.compatShadedThreshold)
      if (not shaded) and is_bright_idx(cur.dom) and is_bright_idx(other_dom) then
        if rule7_white_exception and (is_whiteish_idx(cur.dom) or is_whiteish_idx(other_dom)) then
          -- allow the "white exception"
        else
        -- Prefer a dark separator (default colour 0 if allowed).
        local sep = snap_to_allowed(0, allowed, allowed_set) or darkest_allowed(allowed) or cur.dom
        cands[#cands + 1] = { ch = "█", fg = sep, bg = sep, tag = "rule7_sep" }
        -- Or fade the edge inward by darkening this cell.
        local fade = pick_toon_colour(allowed, cur.dom, -1, is_classic16)
        if type(fade) == "number" then
          cands[#cands + 1] = { ch = is_structural_glyph(cur.ch) and cur.ch or "█", fg = fade, tag = "rule7_fade" }
        end
        end
      end
    end
  end

  -- Halaster reversal inside band (F1<->F2 to break stripes).
  if params.enableReverse and dist >= 1 and dist <= params.transitionWidth then
    if cur.ch == "░" then cands[#cands + 1] = { ch = "▒", tag = "reverse" } end
    if cur.ch == "▒" then cands[#cands + 1] = { ch = "░", tag = "reverse" } end
  end

  -- Halaster border (lit-side) and grit.
  if params.enableBorder and bmask and (not thin_feature) then
    -- Bias border to the light-facing side (simple heuristic: high L(c)).
    local lc = fields.L[k] or 0.5
    if lc >= 0.65 then
      local bright = pick_toon_colour(allowed, cur.dom, 1, is_classic16)
      cands[#cands + 1] = { ch = is_structural_glyph(cur.ch) and cur.ch or "█", fg = bright, tag = "border" }
    end
  end

  -- Halshade texture/extrimities (approximation of swirl + pits/sparkles).
  if params.enableHalshade and (not thin_feature) and type(cur.dom) == "number" then
    local lc = fields.L[k] or 0.5
    local d = dist or 0
    if d >= 2 then
      -- pits in shadow centers
      if lc <= 0.30 then
        local pit = snap_to_allowed(0, allowed, allowed_set) or darkest_allowed(allowed) or cur.dom
        cands[#cands + 1] = { ch = is_structural_glyph(cur.ch) and cur.ch or "█", fg = pit, tag = "pit" }
      end
      -- sparkles in highlight centers
      if lc >= 0.70 then
        local sp = pick_toon_colour(allowed, cur.dom, 1, is_classic16) or brightest_allowed(allowed) or cur.dom
        cands[#cands + 1] = { ch = is_structural_glyph(cur.ch) and cur.ch or "█", fg = sp, tag = "sparkle" }
      end
    end
    -- swirl: sparse highlights guided by deterministic noise
    if lc >= 0.55 then
      local n = rand01(dab_seed, x, y, 101)
      if n >= 0.93 then
        local sw = pick_toon_colour(allowed, cur.dom, 1, is_classic16) or cur.dom
        cands[#cands + 1] = { ch = is_structural_glyph(cur.ch) and cur.ch or "█", fg = sw, tag = "swirl" }
      end
    end
  end

  if params.enablePlus and (not thin_feature) then
    -- Use '+' as grit in safe contexts (keep colours unless we can respect intent).
    local lc = fields.L[k] or 0.5
    if lc >= 0.55 then
      cands[#cands + 1] = { ch = "+", fg = cur.dom, bg = cur.bg, tag = "plus" }
    end
  end

  if params.enableChroma and (not thin_feature) then
    local h = (fields.hist and fields.hist[k]) or {}
    -- Allowed = colours that appear locally at least minCount.
    local minCount = clamp(to_int(params.chromaMinCount, 6), 1, 9999)
    local pool = {}
    for col, cnt in pairs(h) do
      if cnt >= minCount then
        pool[#pool + 1] = col
      end
    end
    if #pool > 0 then
      local pick = pool[1 + math.floor(rand01(dab_seed, x, y, 201) * #pool)]
      if type(pick) == "number" then
        cands[#cands + 1] = { ch = is_structural_glyph(cur.ch) and cur.ch or "█", fg = pick, tag = "chroma" }
      end
    end
  end

  if params.enablePnakotic and (not thin_feature) then
    -- Micro-variation: small fg shifts among close compat colours.
    if type(allowed) ~= "table" or #allowed == 0 then
      -- Host didn't provide a palette list; skip pnakotic (cannot search neighbors safely).
    else
    local base = cur.dom
    if type(base) == "number" then
      local bests = {}
      local by = luminance(base)
      for i = 1, #allowed do
        local idx = allowed[i]
        if type(idx) == "number" and idx ~= base then
          local cscore = compat(base, idx, is_classic16, allow08)
          if cscore >= 0.80 then
            local dy = math.abs(luminance(idx) - by)
            if dy <= 0.18 then
              bests[#bests + 1] = idx
            end
          end
        end
      end
      if #bests > 0 then
        local pick = bests[1 + math.floor(rand01(dab_seed, x, y, 301) * #bests)]
        cands[#cands + 1] = { fg = pick, tag = "pnakotic" }
      end
    end
    end
  end

  if params.enableBgFeather then
    -- Background blend (spec): apply on the OUTSIDE (non-region) cells near silhouettes.
    local od = fields.outside_dist and fields.outside_dist[k] or nil
    local in_dom = fields.outside_in_dom and fields.outside_in_dom[k] or nil
    local wbg = clamp(to_int(params.bgBlendWidth, params.transitionWidth or 3), 1, 32)
    if rid == nil and type(od) == "number" and od <= wbg and type(in_dom) == "number" then
      local base_bg = cur.dom
      if type(base_bg) ~= "number" then base_bg = fields.default_bg end
      if type(base_bg) == "number" then
        local g = "░"
        if od <= 0 then g = "▒"
        elseif od == 1 then g = "░"
        else g = " "
        end
        if g ~= " " then
          cands[#cands + 1] = { ch = g, fg = in_dom, bg = base_bg, tag = "bg_feather" }
        end
      end
    end
  end

  return clamp_candidates(cands, params.maxCandidatesPerCell)
end

local function solve_and_apply(ctx, layer, cx, cy, r, fields, params, allowed, allowed_set, intent_sign, strength, dab_seed, is_classic16, allow08, hard_textures, stroke_dx, stroke_dy)
  local proposed = {} -- key -> {write, dom}

  local function get_cur_state(x, y)
    local k = key_xy(x, y)
    local p = proposed[k]
    if p ~= nil then
      return p.write
    end
    local c = fields.cell[k]
    if not c then
      return { ch = " ", fg = nil, bg = nil, attrs = nil, dom = nil }
    end
    return { ch = c.ch, fg = c.fg, bg = c.bg, attrs = c.attrs, dom = c.dom }
  end

  local function apply_proposed(x, y, w)
    local k = key_xy(x, y)
    local dom = dominant_colour(w.ch, w.fg, w.bg)
    proposed[k] = { write = w, dom = dom }
  end

  -- Generate candidates per cell.
  local cand_by = {}
  local order = {}
  for y = fields.y0, fields.y1 do
    if y >= 0 and (to_int(ctx.rows, 0) <= 0 or y < to_int(ctx.rows, 0)) then
      for x = fields.x0, fields.x1 do
        if x >= 0 and x < to_int(ctx.cols, 0) then
          local k = key_xy(x, y)
          if fields.cell[k] ~= nil then
            cand_by[k] = gen_candidates(ctx, fields, params, allowed, allowed_set, dab_seed, x, y, intent_sign, strength, is_classic16, allow08)
            order[#order + 1] = { x = x, y = y, k = k }
          end
        end
      end
    end
  end

  -- Deterministic stroke-direction ordering (approximation of "orderedByStroke").
  do
    local dx, dy = to_float(stroke_dx, 0.0), to_float(stroke_dy, 0.0)
    local len = math.sqrt(dx * dx + dy * dy)
    if len > 1e-6 then
      dx, dy = dx / len, dy / len
      for i = 1, #order do
        local o = order[i]
        o.t = (o.x * dx + o.y * dy)
      end
      table.sort(order, function(a, b)
        if a.t ~= b.t then return a.t < b.t end
        if a.y ~= b.y then return a.y < b.y end
        return a.x < b.x
      end)
    else
      -- Default stable order: y then x.
      table.sort(order, function(a, b)
        if a.y ~= b.y then return a.y < b.y end
        return a.x < b.x
      end)
    end
  end

  -- Greedy solve (stroke-direction order).
  for i = 1, #order do
    local o = order[i]
    local x, y, k = o.x, o.y, o.k
    local cands = cand_by[k]
    if cands ~= nil then
      local cur = get_cur_state(x, y)
      local best = nil
      local bestE = math.huge
      for j = 1, #cands do
        local cand = cands[j]
        local e = score_cell(fields, params, proposed, x, y, cur, cand, intent_sign, strength, is_classic16, allow08, hard_textures, allowed, allowed_set)
        if e < bestE then
          bestE = e
          best = cand
        end
      end
      if best ~= nil and bestE < math.huge then
        local w = merge_write(cur, best)
        -- Preserve attrs from ctx when writing if host doesn't provide per-cell attrs.
        local a = to_int(ctx.attrs, 0)
        if cur.attrs == nil and type(a) == "number" then
          w.attrs = a
        end
        apply_proposed(x, y, w)
      end
    end
  end

  -- Relaxation passes.
  local passes = clamp(to_int(ctx.params and ctx.params.relaxPasses, params.relaxPasses), 0, 3)
  for pass = 1, passes do
    for y = fields.y0, fields.y1 do
      if y >= 0 and (to_int(ctx.rows, 0) <= 0 or y < to_int(ctx.rows, 0)) then
        for x = fields.x0, fields.x1 do
          if x >= 0 and x < to_int(ctx.cols, 0) then
            local k = key_xy(x, y)
            local cands = cand_by[k]
            if cands ~= nil then
              local cur = get_cur_state(x, y)
              local best = nil
              local bestE = math.huge
              for i = 1, #cands do
                local cand = cands[i]
                local e = score_cell(fields, params, proposed, x, y, cur, cand, intent_sign, strength, is_classic16, allow08, hard_textures, allowed, allowed_set)
                if e < bestE then
                  bestE = e
                  best = cand
                end
              end
              if best ~= nil and bestE < math.huge then
                local w = merge_write(cur, best)
                local a = to_int(ctx.attrs, 0)
                if cur.attrs == nil and type(a) == "number" then
                  w.attrs = a
                end
                apply_proposed(x, y, w)
              end
            end
          end
        end
      end
    end
  end

  -- Commit proposed writes to the layer (only if changed).
  for y = fields.y0, fields.y1 do
    if y >= 0 and (to_int(ctx.rows, 0) <= 0 or y < to_int(ctx.rows, 0)) then
      for x = fields.x0, fields.x1 do
        if x >= 0 and x < to_int(ctx.cols, 0) then
          local k = key_xy(x, y)
          local p = proposed[k]
          if p ~= nil then
            local w = p.write
            local c = fields.cell[k]
            local ch0 = c and c.ch or " "
            local fg0 = c and c.fg or nil
            local bg0 = c and c.bg or nil
            if w.ch ~= ch0 or w.fg ~= fg0 or w.bg ~= bg0 then
              local attrs = w.attrs
              if type(attrs) ~= "number" then attrs = 0 end
              if w.fg == nil and w.bg == nil and attrs == 0 then
                layer:set(x, y, w.ch)
              else
                layer:set(x, y, w.ch, w.fg, w.bg, attrs)
              end
            end
          end
        end
      end
    end
  end
end

-- -------------------------------------------------------------------------
-- Stroke handling (mouse drag) + tool render
-- -------------------------------------------------------------------------
local stroke = {
  active = false,
  last_x = 0,
  last_y = 0,
  seed = 1,
}

local function light_dir_from_params(p)
  local a = to_int(p.lightAngle, 315)
  if p.snap8 ~= false then
    local step = 45
    a = step * math.floor((a + step / 2) / step)
  end
  local rad = (a * math.pi) / 180.0
  local dx = math.cos(rad)
  local dy = math.sin(rad) -- y+ is down in canvas coords
  return dx, dy
end

local function end_stroke()
  stroke.active = false
end

local function shade_at(ctx, layer, x, y, dx, dy)
  local p = ctx.params or {}
  local size = clamp(to_int(p.size, 7), 1, 200)
  local r = math.floor(size / 2)

  local mode = (type(p.mode) == "string") and p.mode or "clean"
  local md = mode_defaults(mode)
  md.relaxPasses = clamp(to_int(p.relaxPasses, md.relaxPasses), 0, 3)

  local strength = clamp(to_float(p.strength, 0.7), 0.0, 1.0)
  local primary = (type(p.primary) == "string") and p.primary or "darken"
  local cursor = ctx.cursor or {}
  local secondary = (cursor.right == true)
  local intent = primary
  if secondary then
    intent = (primary == "darken") and "lighten" or "darken"
  end
  local intent_sign = (intent == "lighten") and 1 or -1

  local light_dx, light_dy = light_dir_from_params(p)
  local allowed = select(1, get_allowed_palette(ctx))
  local allowed_set = build_allowed_set(allowed)

  local is_classic16 = is_classic16_palette(ctx, allowed)
  local allow08 = (p.allow08Transition == true)
  local hard_textures = (p.allowHardTextures == true)

  -- Apply runtime-tunable thresholds.
  bright_threshold = clamp(to_float(p.brightThreshold, bright_threshold or 0.65), 0.0, 1.0)
  rule7_white_exception = (p.rule7WhiteException ~= false)
  rule7_white_threshold = clamp(to_float(p.rule7WhiteThreshold, rule7_white_threshold or 0.92), 0.0, 1.0)

  -- Apply advanced per-stroke overrides into the mode defaults.
  md.compatShadedThreshold = clamp(to_float(p.compatShadedThreshold, md.compatShadedThreshold), 0.0, 1.0)
  md.thinThicknessMin = clamp(to_int(p.thinThicknessMin, md.thinThicknessMin), 1, 32)
  md.thinDetailMin = clamp(to_float(p.thinDetailMin, md.thinDetailMin), 0.0, 1.0)
  md.maxStraightHighlightRun = clamp(to_int(p.maxStraightHighlightRun, md.maxStraightHighlightRun), 2, 999)
  md.maxDensityDetailRatio = clamp(to_float(p.maxDensityDetailRatio, md.maxDensityDetailRatio), 0.0, 1.0)
  local dw = clamp(to_int(p.densityWindow, md.densityDetailWindowW), 3, 31)
  md.densityDetailWindowW = dw
  md.densityDetailWindowH = dw
  md.chromaMinCount = clamp(to_int(p.chromaMinCount, md.chromaMinCount), 1, 9999)
  md.chromaHistRadius = clamp(to_int(p.chromaRadius, md.chromaHistRadius), 1, 32)
  md.bgBlendWidth = clamp(to_int(p.bgBlendWidth, md.bgBlendWidth), 1, 32)

  local fields = analyze_footprint(ctx, layer, x, y, r, dx, dy, light_dx, light_dy, md, "layer")

  -- Fill per-cell tone target T(c) from L(c) and stroke strength.
  for yy = fields.y0, fields.y1 do
    if yy >= 0 and (to_int(ctx.rows, 0) <= 0 or yy < to_int(ctx.rows, 0)) then
      for xx = fields.x0, fields.x1 do
        if xx >= 0 and xx < to_int(ctx.cols, 0) then
          local k = key_xy(xx, yy)
          local lc = fields.L[k] or 0.5
          local t = (2.0 * lc - 1.0) * strength * intent_sign
          fields.T[k] = t
        end
      end
    end
  end

  -- Deterministic dab RNG: seeded per stroke + dab location.
  local dab_seed = hash32(stroke.seed, x, y, to_int(p.seed, 1))

  solve_and_apply(ctx, layer, x, y, r, fields, md, allowed, allowed_set, intent_sign, strength, dab_seed, is_classic16, allow08, hard_textures, dx, dy)
end

function render(ctx, layer)
  if type(ctx) ~= "table" or not layer then return end
  if not ctx.focused then
    end_stroke()
    return
  end

  local cols = to_int(ctx.cols, 0)
  if cols <= 0 then
    end_stroke()
    return
  end

  local caret = ctx.caret
  if type(caret) ~= "table" then
    end_stroke()
    return
  end
  caret.x = clamp(to_int(caret.x, 0), 0, cols - 1)
  caret.y = math.max(0, to_int(caret.y, 0))

  -- Brush preview (host overlay; transient).
  do
    local p = ctx.params or {}
    local size = clamp(to_int(p.size, 7), 1, 200)
    local r = math.floor(size / 2)
    if ctx.out ~= nil then
      ctx.out[#ctx.out + 1] = { type = "brush.preview", anchor = "cursor", rx = r, ry = r }
    end
  end

  local phase = to_int(ctx.phase, 0)
  if phase ~= 1 then
    -- Mouse-only for now.
    if not any_down(ctx.cursor or {}) then
      end_stroke()
    end
    return
  end

  local cursor = ctx.cursor or {}
  if cursor.valid ~= true then
    end_stroke()
    return
  end

  if not any_down(cursor) then
    end_stroke()
    return
  end

  local x1 = clamp(to_int(cursor.x, caret.x), 0, cols - 1)
  local y1 = math.max(0, to_int(cursor.y, caret.y))
  caret.x, caret.y = x1, y1

  local edge = press_edge(cursor)
  if edge or (not stroke.active) then
    stroke.active = true
    stroke.last_x = caret.x
    stroke.last_y = caret.y
    -- Deterministic stroke seed: based on start cell + mode + seed param.
    local p = ctx.params or {}
    local mode = (type(p.mode) == "string") and p.mode or "clean"
    stroke.seed = hash32(to_int(p.seed, 1), caret.x, caret.y, (#mode))
    shade_at(ctx, layer, caret.x, caret.y, 0, 0)
    return
  end

  if not moved_cell(cursor) then
    return
  end

  -- Interpolate between last and current cell to avoid gaps.
  local function bresenham(xa, ya, xb, yb, fn, skip_first)
    xa, ya, xb, yb = math.floor(xa), math.floor(ya), math.floor(xb), math.floor(yb)
    local dx = math.abs(xb - xa)
    local sx = (xa < xb) and 1 or -1
    local dy = -math.abs(yb - ya)
    local sy = (ya < yb) and 1 or -1
    local err = dx + dy
    local first = true
    while true do
      if not (skip_first and first) then
        fn(xa, ya)
      end
      first = false
      if xa == xb and ya == yb then break end
      local e2 = 2 * err
      if e2 >= dy then err = err + dy; xa = xa + sx end
      if e2 <= dx then err = err + dx; ya = ya + sy end
    end
  end

  local x0, y0 = stroke.last_x, stroke.last_y
  local dx = caret.x - x0
  local dy = caret.y - y0
  bresenham(x0, y0, caret.x, caret.y, function(ix, iy)
    ix = clamp(ix, 0, cols - 1)
    if iy < 0 then iy = 0 end
    shade_at(ctx, layer, ix, iy, dx, dy)
  end, true)

  stroke.last_x = caret.x
  stroke.last_y = caret.y
end


