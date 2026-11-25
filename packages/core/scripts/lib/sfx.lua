local A = gramarye.audio
local M = {}

local function clamp(x, lo, hi) return x < lo and lo or (x > hi and hi or x) end

function M.copy(t)
    local c = {}
    for k, v in pairs(t) do c[k] = type(v) == "table" and M.copy(v) or v end
    return c
end

function M.patch(overrides)
    local d = A.defaults()
    for k, v in pairs(overrides or {}) do d[k] = type(v) == "table" and M.copy(v) or v end
    return d
end

local function fmt_value(v)
    if type(v) == "string" then return string.format("%q", v) end
    if math.type(v) == "integer" then return tostring(v) end
    return (string.format("%.4f", v):gsub("0+$", ""):gsub("%.$", ""))
end

function M.serialize(d)
    local keys = {}
    for k in pairs(d) do if k ~= "modes" and k ~= "name" then keys[#keys + 1] = k end end
    table.sort(keys)
    local parts = {}
    for _, k in ipairs(keys) do parts[#parts + 1] = k .. " = " .. fmt_value(d[k]) end
    if d.modes and #d.modes > 0 then
        local ms = {}
        for _, m in ipairs(d.modes) do
            ms[#ms + 1] = string.format("{ freq = %s, decay = %s, gain = %s }",
                fmt_value(m.freq), fmt_value(m.decay), fmt_value(m.gain))
        end
        parts[#parts + 1] = "modes = { " .. table.concat(ms, ", ") .. " }"
    end
    return "{ " .. table.concat(parts, ", ") .. " }"
end

M.MATERIAL_ORDER = { "wood", "stone", "metal", "glass", "bell", "drum" }
M.materials = {
    wood  = { base = 420,  ratios = { 1, 2.42, 3.90, 5.30 },
              decays = { 0.12, 0.08, 0.05, 0.03 }, gains = { 1, 0.6, 0.4, 0.25 } },
    stone = { base = 900,  ratios = { 1, 1.68, 2.83, 4.10 },
              decays = { 0.05, 0.04, 0.03, 0.02 }, gains = { 1, 0.8, 0.6, 0.4 } },
    metal = { base = 520,  ratios = { 1, 2.76, 5.40, 8.93 },
              decays = { 1.6, 1.2, 0.9, 0.6 },     gains = { 1, 0.7, 0.5, 0.35 } },
    glass = { base = 1250, ratios = { 1, 2.32, 4.25, 6.63 },
              decays = { 0.9, 0.6, 0.4, 0.25 },    gains = { 1, 0.5, 0.35, 0.2 } },
    bell  = { base = 440,  ratios = { 0.5, 1, 1.19, 1.5, 2, 2.51 },
              decays = { 3.0, 2.2, 1.6, 1.2, 1.0, 0.7 }, gains = { 0.6, 1, 0.5, 0.4, 0.5, 0.3 } },
    drum  = { base = 160,  ratios = { 1, 1.59, 2.14, 2.30, 2.65, 2.92 },
              decays = { 0.35, 0.25, 0.2, 0.18, 0.15, 0.12 }, gains = { 1, 0.6, 0.45, 0.4, 0.3, 0.25 } },
}

function M.modes(name, pitch, decay)
    local m = M.materials[name]
    if not m then return {} end
    pitch, decay = pitch or 1, decay or 1
    local out = {}
    for i, r in ipairs(m.ratios) do
        out[i] = { freq = m.base * r * pitch, decay = m.decays[i] * decay, gain = m.gains[i] }
    end
    return out
end

local function scale_modes(modes, pitch, decay)
    local out = {}
    for i, m in ipairs(modes or {}) do
        out[i] = { freq = m.freq * pitch, decay = m.decay * decay, gain = m.gain }
    end
    return out
end

M.presets = {
    { name = "Sine",   desc = M.patch { wave = "sine", freq = 440, decay = 0.6 } },
    { name = "Square", desc = M.patch { wave = "square", freq = 220, sustain = 0.6, hold = 0.4,
                                        decay = 0.3, gain = 0.45 } },
    { name = "Kick",   desc = M.patch { freq = 50, pitch_start = 4, pitch_time = 0.04, decay = 0.35,
                                        gain = 1.0, drive = 0.1 } },
    { name = "Snare",  desc = M.patch { wave = "triangle", freq = 190, pitch_start = 1.6, pitch_time = 0.03,
                                        osc_level = 0.6, noise_level = 0.9, decay = 0.14,
                                        filter = "hp", cutoff = 400, gain = 0.8 } },
    { name = "Hi-hat", desc = M.patch { osc_level = 0, noise_level = 1, decay = 0.05,
                                        filter = "hp", cutoff = 7000, gain = 0.8 } },
    { name = "Pluck",  desc = M.patch { wave = "saw", freq = 220, decay = 0.6, filter = "lp",
                                        cutoff = 350, q = 1.2, filter_env = 4, filter_decay = 0.15,
                                        gain = 0.5 } },
    { name = "Laser",  desc = M.patch { wave = "square", freq = 300, pitch_start = 6, pitch_time = 0.12,
                                        decay = 0.25, gain = 0.5 } },
    { name = "Coin",   desc = M.patch { wave = "pulse", duty = 0.25, freq = 1319, pitch_start = 0.75,
                                        pitch_time = 0.02, decay = 0.35, gain = 0.45 } },
    { name = "Bell",   desc = M.patch { osc_level = 0, noise_level = 1, decay = 0.003, dry = 0,
                                        modes = M.modes("bell", 1.2), modes_level = 0.9, gain = 0.3 } },
    { name = "Wood",   desc = M.patch { osc_level = 0, noise_level = 1, decay = 0.004, dry = 0.2,
                                        modes = M.modes("wood"), gain = 0.3 } },
    { name = "Wind",   desc = M.patch { osc_level = 0, noise_level = 1, noise_color = 0.3,
                                        attack = 0.5, sustain = 1, hold = 2.5, release = 1,
                                        filter = "bp", cutoff = 500, q = 3, gain = 0.6 } },
    { name = "Rain",   desc = M.patch { osc_level = 0, noise_level = 1, grain_rate = 140, grain_len = 0.002,
                                        attack = 0.3, sustain = 1, hold = 3, release = 0.8,
                                        filter = "hp", cutoff = 1500, gain = 0.7 } },
    { name = "Engine", desc = M.patch { wave = "saw", freq = 45, lfo_rate = 7, lfo_depth = 0.6,
                                        sustain = 1, hold = 2, attack = 0.2, release = 0.4,
                                        filter = "lp", cutoff = 300, q = 4, drive = 0.6, gain = 1.0 } },
}

M.FOOT_ORDER = { "flesh", "boot", "bone", "hoof", "metal", "claw" }
M.feet = {
    flesh = { attack = 0.004,  burst = 0.012, color = 0.75, hardness = 0.15, thud = 1.0, damp = 0.5 },
    boot  = { attack = 0.002,  burst = 0.010, color = 0.45, hardness = 0.5,  thud = 0.8, damp = 0.75, scuff = 0.4 },
    bone  = { attack = 0.0005, burst = 0.004, color = 0.05, hardness = 0.9,  thud = 0.15, damp = 1.0, rattle = 2 },
    hoof  = { attack = 0.0008, burst = 0.006, color = 0.3,  hardness = 0.85, thud = 0.7, damp = 0.9 },
    metal = { attack = 0.0005, burst = 0.005, color = 0.1,  hardness = 1.0,  thud = 0.6, damp = 1.0,
              ring = { material = "metal", pitch = 3.6, decay = 0.2, level = 0.35 } },
    claw  = { attack = 0.0005, burst = 0.003, color = 0.0,  hardness = 0.8,  thud = 0.2, damp = 0.9,
              scuff = 0.9, clicks = 2 },
}

M.SURFACE_ORDER = { "stone", "wood", "metal", "dirt", "grass", "gravel", "snow", "water" }
M.surfaces = {
    stone  = { modes = M.modes("stone"), modes_level = 0.8, dry = 0.6, cutoff = 9000, thud = 0.6 },
    wood   = { modes = M.modes("wood", 0.6, 1.5), modes_level = 1.0, dry = 0.4, cutoff = 6000, thud = 0.9 },
    metal  = { modes = M.modes("metal", 0.7, 0.5), modes_level = 0.6, dry = 0.3, cutoff = 12000, thud = 0.5 },
    dirt   = { dry = 1.0, cutoff = 900, thud = 1.0,
               texture = { rate = 60, len = 0.006, filter = "lp", cutoff = 1200, tail = 0.08, level = 0.4 } },
    grass  = { dry = 0.5, cutoff = 2500, thud = 0.6,
               texture = { rate = 260, len = 0.003, filter = "hp", cutoff = 2500, tail = 0.12, level = 0.6 } },
    gravel = { dry = 0.6, cutoff = 4000, thud = 0.7,
               texture = { rate = 180, len = 0.008, filter = "bp", cutoff = 2200, q = 1.2, tail = 0.15, level = 0.9 } },
    snow   = { dry = 0.3, cutoff = 1800, thud = 0.8,
               texture = { rate = 400, len = 0.004, filter = "lp", cutoff = 1800, tail = 0.18, level = 0.8 } },
    water  = { dry = 0.4, cutoff = 2500, thud = 0.4, splash = 3,
               texture = { rate = 120, len = 0.01, filter = "bp", cutoff = 900, q = 2, tail = 0.12, level = 0.5 } },
}

function M.footstep(foot_name, surface_name, opts)
    local f = M.feet[foot_name] or M.feet.flesh
    local s = M.surfaces[surface_name] or M.surfaces.stone
    opts = opts or {}
    local w     = clamp(opts.weight or 0.5, 0, 1)
    local sp    = clamp(opts.speed or 0.5, 0, 1)
    local pan   = opts.pan or 0
    local pitch = 1.25 - 0.55 * w
    local force = 0.35 + 0.45 * sp + 0.2 * w
    local hard  = f.hardness
    local layers = {}

    local impact = M.patch {
        name = "impact", osc_level = 0, noise_level = 1,
        noise_color = clamp(f.color + w * 0.15, 0, 1),
        attack = f.attack * (1.6 - sp), decay = f.burst * (0.8 + 0.6 * w),
        modes = scale_modes(s.modes, pitch, f.damp * (0.7 + 0.6 * w)),
        excite = 0.4 + 0.6 * hard, dry = s.dry, modes_level = s.modes_level or 1,
        filter = "lp", cutoff = s.cutoff * (0.35 + 0.65 * hard) * (1.2 - 0.4 * w), q = 0.7,
        gain = force * 0.5, pan = pan, jitter = 0.06,
    }
    layers[#layers + 1] = impact

    if sp < 0.5 then
        local toe = M.copy(impact)
        toe.name   = "toe"
        toe.delay  = 0.05 + (0.5 - sp) * 0.2
        toe.gain   = impact.gain * 0.45
        toe.cutoff = impact.cutoff * 1.3
        layers[#layers + 1] = toe
    end

    local thud = f.thud * (s.thud or 0.5) * (0.2 + 0.8 * w)
    if thud > 0.05 then
        layers[#layers + 1] = M.patch {
            name = "thud", wave = "sine", freq = 95 - 45 * w,
            pitch_start = 2.2, pitch_time = 0.025, attack = 0.001, decay = 0.05 + 0.1 * w,
            gain = thud * force * 0.9, pan = pan, jitter = 0.05,
        }
    end

    local t = s.texture
    if t then
        layers[#layers + 1] = M.patch {
            name = "texture", osc_level = 0, noise_level = 1,
            grain_rate = t.rate * (0.6 + 0.8 * w) * (0.7 + 0.6 * sp), grain_len = t.len,
            attack = 0.003, decay = t.tail * (0.6 + 0.8 * w), delay = 0.004,
            filter = t.filter or "bp", cutoff = t.cutoff * pitch, q = t.q or 0.8,
            gain = t.level * force * 2.0, pan = pan, jitter = 0.1,
        }
    end

    if f.ring then
        layers[#layers + 1] = M.patch {
            name = "foot ring", osc_level = 0, noise_level = 1, decay = 0.003, dry = 0,
            modes = M.modes(f.ring.material, f.ring.pitch * pitch, f.ring.decay),
            gain = f.ring.level * force, pan = pan, jitter = 0.08,
        }
    end

    if f.scuff and sp > 0.3 then
        layers[#layers + 1] = M.patch {
            name = "scuff", osc_level = 0, noise_level = 1, noise_color = 0.2,
            attack = 0.02, decay = 0.06, delay = 0.02,
            filter = "hp", cutoff = 3000, gain = f.scuff * sp * 0.35, pan = pan, jitter = 0.1,
        }
    end

    local clicks = (f.rattle or 0) + (f.clicks or 0)
    for i = 1, clicks do
        layers[#layers + 1] = M.patch {
            name = "click " .. i, osc_level = 0, noise_level = 1, decay = 0.002,
            delay = 0.012 * i + math.random() * 0.012, dry = 0.3,
            modes = M.modes("wood", 4.5 + math.random(), 0.4),
            gain = 0.25 * force, pan = pan, jitter = 0.15,
        }
    end

    for i = 1, (s.splash or 0) do
        layers[#layers + 1] = M.patch {
            name = "splash " .. i, wave = "sine", freq = 500 + math.random() * 900,
            pitch_start = 0.55, pitch_time = 0.03, decay = 0.04,
            delay = 0.01 + math.random() * 0.06, gain = 0.3 * force, pan = pan,
        }
    end

    return layers
end

function M.play_layers(layers)
    for _, d in ipairs(layers) do A.play(d) end
    return layers
end

function M.play_footstep(foot, surface, opts)
    return M.play_layers(M.footstep(foot, surface, opts))
end

return M
