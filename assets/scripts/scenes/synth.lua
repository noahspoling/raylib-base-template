-- Synth lab: every gramarye.audio feature behind a slider.
--   Synth tab      oscillator / noise / envelope / filter / output of one patch
--   Resonator tab  the modal bank the patch excites
--   Footsteps tab  lib/sfx footstep generator; any layer can be opened in Synth
-- space = play, L = live (held voice follows the sliders), esc = back

local ui      = gramarye.require("gramarye.ui")
local widgets = gramarye.require("lib/widgets")
local sfx     = gramarye.require("lib/sfx")
local A       = gramarye.audio

local KEY_SPACE  = gramarye.input.key("space")
local KEY_ESCAPE = gramarye.input.key("escape")
local KEY_L      = gramarye.input.key("l")

local PANEL_BG = { 20, 22, 34, 255 }
local GROUP_BG = { 26, 29, 44, 255 }

local desc      = sfx.copy(sfx.presets[1].desc)
local preset_ix = 1
local tab       = 1
local live      = nil          -- handle of the held voice in live mode

local foot_ix, surface_ix = 1, 1
local weight, speed       = 0.5, 0.5
local walking, walk_timer, walk_side = false, 0, 1
local last_layers = {}

-- ------------------------------------------------------------- playback

-- live mode holds the voice open and keeps it audible even for one-shot patches
local function live_desc()
    local d = sfx.copy(desc)
    d.hold = -1
    if d.sustain < 0.5 then d.sustain = 0.5 end
    d.delay = 0
    return d
end

local function play()      A.play(desc) end
local function refresh()   if live then A.update(live, live_desc()) end end
-- audition edits: a held live voice already follows the sliders, otherwise retrigger
local function audition()  if not live then play() end end

local function set_live(on)
    if live then A.release(live); live = nil end
    if on then live = A.play(live_desc()) end
end
local function toggle_live() set_live(not live) end

local function stop_all()
    A.stop_all()
    live, walking = nil, false
end

local function load_desc(d)
    desc = sfx.copy(d)
    if not desc.modes then desc.modes = {} end
    refresh()
end

local function step()
    walk_side = -walk_side
    last_layers = sfx.play_footstep(sfx.FOOT_ORDER[foot_ix], sfx.SURFACE_ORDER[surface_ix],
                                    { weight = weight, speed = speed, pan = 0.15 * walk_side })
end

-- ------------------------------------------------------------- controls

local function field(key) return function(v) desc[key] = v; refresh() end end
local setters = {}
local function setter(key)
    local s = setters[key]
    if not s then s = field(key); setters[key] = s end
    return s
end

local function slider(key, label, lo, hi, fmt, log)
    return widgets.Slider { id = "s_" .. key, label = label, value = desc[key],
                            min = lo, max = hi, log = log, fmt = fmt, on_change = setter(key),
                            on_commit = audition }
end

local function index_of(list, v)
    for i, x in ipairs(list) do if x == v then return i end end
    return 1
end

local function group(title, ...)
    local col = ui.Column { bg = GROUP_BG, radius = 6, pad = 12, gap = 6, w = { grow = true } }
    col[1] = ui.Label { text = title, size = 15, color = { 230, 210, 150, 255 } }
    for i, v in ipairs({ ... }) do col[i + 1] = v end
    return col
end

local function select_wave(_, item)   desc.wave = item;   refresh(); audition() end
local function select_filter(_, item) desc.filter = item; refresh(); audition() end

local function synth_tab()
    return ui.Column {
        w = { grow = true }, gap = 10,
        ui.Row { w = { grow = true }, gap = 10,
            group("Oscillator",
                widgets.Segmented { id = "seg_wave", items = A.WAVES,
                                    selected = index_of(A.WAVES, desc.wave), on_select = select_wave },
                slider("freq", "Pitch", 20, 4000, "%.0f Hz", true),
                slider("duty", "Pulse width", 0.05, 0.95, "%.2f"),
                slider("osc_level", "Level", 0, 1)),
            group("Pitch motion",
                slider("pitch_start", "Start x", 0.25, 8, "x%.2f", true),
                slider("pitch_time", "Glide", 0.001, 1, "%.3f s", true),
                slider("lfo_rate", "LFO rate", 0, 20, "%.1f Hz"),
                slider("lfo_depth", "LFO depth", 0, 12, "%.2f st")),
            group("Noise & grains",
                slider("noise_level", "Level", 0, 1),
                slider("noise_color", "Color", 0, 1, "%.2f"),
                slider("grain_rate", "Grains/s", 0, 800, "%.0f"),
                slider("grain_len", "Grain len", 0.001, 0.05, "%.3f s", true)),
        },
        ui.Row { w = { grow = true }, gap = 10,
            group("Amp envelope",
                slider("attack", "Attack", 0.0005, 2, "%.4f s", true),
                slider("decay", "Decay", 0.001, 4, "%.3f s", true),
                slider("sustain", "Sustain", 0, 1),
                slider("release", "Release", 0.001, 4, "%.3f s", true),
                slider("hold", "Hold", 0, 4, "%.2f s"),
                slider("delay", "Delay", 0, 0.5, "%.3f s")),
            group("Filter",
                widgets.Segmented { id = "seg_filter", items = A.FILTERS,
                                    selected = index_of(A.FILTERS, desc.filter), on_select = select_filter },
                slider("cutoff", "Cutoff", 20, 20000, "%.0f Hz", true),
                slider("q", "Resonance", 0.5, 20, "%.2f", true),
                slider("filter_env", "Env amount", -4, 6, "%+.1f oct"),
                slider("filter_decay", "Env decay", 0.001, 2, "%.3f s", true)),
            group("Output",
                slider("drive", "Drive", 0, 1),
                slider("gain", "Gain", 0, 1),
                slider("pan", "Pan", -1, 1, "%+.2f"),
                slider("jitter", "Jitter", 0, 0.5, "%.2f")),
        },
    }
end

-- ------------------------------------------------------------- resonator tab

local mode_setters = {}
local function mode_setter(i, key)
    local id = i .. key
    local s = mode_setters[id]
    if not s then
        s = function(v) desc.modes[i][key] = v; refresh() end
        mode_setters[id] = s
    end
    return s
end

local function add_mode()
    if #desc.modes >= A.MAX_MODES then return end
    local prev = desc.modes[#desc.modes]
    desc.modes[#desc.modes + 1] = prev and { freq = prev.freq * 1.5, decay = prev.decay, gain = prev.gain * 0.7 }
                                       or { freq = 440, decay = 0.4, gain = 1 }
    refresh()
end
local function remove_mode()
    desc.modes[#desc.modes] = nil
    refresh()
end

local material_clicks = {}
for i, name in ipairs(sfx.MATERIAL_ORDER) do
    material_clicks[i] = function() desc.modes = sfx.modes(name); refresh(); audition() end
end

local function mode_row(i)
    local m = desc.modes[i]
    return group("Mode " .. i,
        widgets.Slider { id = "m_f" .. i, label = "Freq", value = m.freq, min = 30, max = 12000, log = true,
                         fmt = "%.0f Hz", on_change = mode_setter(i, "freq"), on_commit = audition },
        widgets.Slider { id = "m_d" .. i, label = "Decay (T60)", value = m.decay, min = 0.01, max = 5, log = true,
                         fmt = "%.2f s", on_change = mode_setter(i, "decay"), on_commit = audition },
        widgets.Slider { id = "m_g" .. i, label = "Gain", value = m.gain, min = 0, max = 1,
                         on_change = mode_setter(i, "gain"), on_commit = audition })
end

local function resonator_tab()
    local chips = ui.Row { gap = 6, align = { y = "center" },
                           ui.Label { text = "Fill from material:", size = 13, skin = "label_dim" } }
    for i, name in ipairs(sfx.MATERIAL_ORDER) do
        chips[#chips + 1] = widgets.Chip { id = "mat_" .. name, label = name, on_click = material_clicks[i] }
    end

    local col = ui.Column { w = { grow = true }, gap = 10,
        group("Routing   (source = enveloped osc + noise from the Synth tab)",
            slider("excite", "Excite", 0, 2, "%.2f"),
            slider("dry", "Dry source", 0, 1),
            slider("modes_level", "Modes out", 0, 2, "%.2f"),
            ui.Label { text = "Short noise bursts (decay ~3 ms) ring the modes like a strike; "
                           .. "a held oscillator near a mode frequency drives it like a bow.",
                       size = 12, skin = "label_dim" }),
        chips,
        ui.Row { gap = 8, align = { y = "center" },
            widgets.Chip { id = "mode_add", label = "+ mode", on_click = add_mode },
            widgets.Chip { id = "mode_del", label = "- mode", on_click = remove_mode },
            ui.Label { text = string.format("%d / %d modes", #desc.modes, A.MAX_MODES), size = 13,
                       skin = "label_dim" } },
    }
    local row
    for i = 1, #desc.modes do
        if i % 3 == 1 then row = ui.Row { w = { grow = true }, gap = 10 }; col[#col + 1] = row end
        row[#row + 1] = mode_row(i)
    end
    return col
end

-- ------------------------------------------------------------- footsteps tab

local function select_foot(i)    foot_ix = i end
local function select_surface(i) surface_ix = i end
local function set_weight(v)     weight = v end
local function set_speed(v)      speed = v end
local function toggle_walk()     walking = not walking; walk_timer = 0 end

local function describe(d)
    local bits = {}
    if d.osc_level > 0 then bits[#bits + 1] = string.format("%s %.0f Hz", d.wave, d.freq) end
    if d.pitch_start ~= 1 then bits[#bits + 1] = string.format("pitch x%.1f", d.pitch_start) end
    if d.noise_level > 0 then
        bits[#bits + 1] = d.grain_rate > 0 and string.format("grains %.0f/s", d.grain_rate) or "noise"
    end
    if #d.modes > 0 then bits[#bits + 1] = #d.modes .. " modes" end
    if d.filter ~= "off" then bits[#bits + 1] = string.format("%s %.0f Hz", d.filter, d.cutoff) end
    if d.delay > 0 then bits[#bits + 1] = string.format("+%.0f ms", d.delay * 1000) end
    return table.concat(bits, "  •  ")  -- • is in the baked font codepoints, · is not
end

local function footsteps_tab()
    local layer_list = ui.Column { gap = 4, w = { grow = true } }
    if #last_layers == 0 then
        layer_list[1] = ui.Label { text = "Press Step to generate layers.", size = 13, skin = "label_dim" }
    end
    for i, d in ipairs(last_layers) do
        layer_list[i] = ui.Row { w = { grow = true }, gap = 10, align = { y = "center" },
            { _type = "frame", layout = { w = 90 }, ui.Label { text = d.name or "?", size = 14 } },
            ui.Label { text = describe(d), size = 13, skin = "label_dim" },
            ui.Spacer(),
            widgets.Chip { id = "edit_layer_" .. i, label = "Open in Synth",
                           on_click = function() load_desc(d); tab = 1 end },
        }
    end

    return ui.Column { w = { grow = true }, gap = 10,
        group("Foot (exciter: what hits)",
            widgets.Segmented { id = "seg_foot", items = sfx.FOOT_ORDER, selected = foot_ix,
                                on_select = select_foot }),
        group("Surface (resonator: what rings)",
            widgets.Segmented { id = "seg_surface", items = sfx.SURFACE_ORDER, selected = surface_ix,
                                on_select = select_surface }),
        group("Transform",
            widgets.Slider { id = "fs_weight", label = "Weight", value = weight, min = 0, max = 1,
                             on_change = set_weight },
            widgets.Slider { id = "fs_speed", label = "Speed", value = speed, min = 0, max = 1,
                             on_change = set_speed },
            ui.Label { text = "weight: lower resonances, longer ring, bigger thud.  "
                           .. "speed: harder attack, louder, heel-toe merges into one strike below 0.5.",
                       size = 12, skin = "label_dim" }),
        ui.Row { gap = 8,
            widgets.Chip { id = "fs_step", label = "Step", on_click = step, h = 32 },
            widgets.Chip { id = "fs_walk", label = walking and "Walking..." or "Walk", selected = walking,
                           on_click = toggle_walk, h = 32 },
        },
        group("Layers of the last step", layer_list),
    }
end

-- ------------------------------------------------------------- side panel

local preset_clicks = {}
for i, p in ipairs(sfx.presets) do
    preset_clicks[i] = function() preset_ix = i; load_desc(p.desc); play() end
end

local function log_patch() gramarye.log("patch: " .. sfx.serialize(desc)) end
local function set_master(v) A.set_master(v) end

local function side_panel()
    local grid = ui.Column { gap = 6, w = { grow = true } }
    local row
    for i, p in ipairs(sfx.presets) do
        if i % 4 == 1 then row = ui.Row { gap = 6 }; grid[#grid + 1] = row end
        row[#row + 1] = widgets.Chip { id = "preset_" .. i, label = p.name, w = 100,
                                       selected = preset_ix == i, on_click = preset_clicks[i] }
    end

    return ui.Column {
        bg = PANEL_BG, w = 470, h = { grow = true }, pad = 14, gap = 10,
        { _type = "custom", kind = A.SCOPE_KIND,    layout = { w = { grow = true }, h = 190 } },
        { _type = "custom", kind = A.SPECTRUM_KIND, layout = { w = { grow = true }, h = 230 } },
        ui.Label { text = string.format("voices %d / %d   %s", A.voices(), A.max_voices(),
                       A.ready() and "" or "  (no audio device)"), size = 13, skin = "label_dim" },
        widgets.Slider { id = "master", label = "Master", value = A.master(), min = 0, max = 1.5,
                         on_change = set_master },
        ui.Row { gap = 8,
            widgets.Chip { id = "btn_play", label = "Play (space)", on_click = play, h = 32 },
            widgets.Chip { id = "btn_live", label = live and "Live: on (L)" or "Live: off (L)",
                           selected = live ~= nil, on_click = toggle_live, h = 32 },
            widgets.Chip { id = "btn_stop", label = "Stop all", on_click = stop_all, h = 32 },
            widgets.Chip { id = "btn_log", label = "Log patch", on_click = log_patch, h = 32 },
        },
        ui.Label { text = "Presets", size = 15, color = { 230, 210, 150, 255 } },
        grid,
    }
end

-- ------------------------------------------------------------- scene

local TABS = { "Synth", "Resonator", "Footsteps" }
local BODIES = { synth_tab, resonator_tab, footsteps_tab }
local tab_clicks = {}
for i = 1, #TABS do tab_clicks[i] = function() tab = i end end

local function back() gramarye.scene.change("main") end

return {
    on_enter = function() gramarye.systems.add("global") end,
    on_exit  = function() stop_all() end,

    on_update = function(dt)
        if gramarye.input.key_pressed(KEY_ESCAPE) then back() end
        if gramarye.input.key_pressed(KEY_SPACE) then
            if tab == 3 then step() else play() end
        end
        if gramarye.input.key_pressed(KEY_L) then toggle_live() end

        if walking then
            walk_timer = walk_timer - dt
            if walk_timer <= 0 then
                step()
                walk_timer = 0.65 - 0.4 * speed
            end
        end
    end,

    on_draw = function()
        widgets.begin_frame()

        local tabs = ui.Row { gap = 6, pad = { h = 14, v = 8 }, align = { y = "center" } }
        for i, name in ipairs(TABS) do
            tabs[i] = widgets.Chip { id = "tab_" .. i, label = name, selected = tab == i,
                                     on_click = tab_clicks[i], h = 32 }
        end
        tabs[#tabs + 1] = ui.Label { size = 13, skin = "label_dim",
            text = live and "   Live: the held note follows every slider"
                        or "   Sliders edit the patch and play it on release.  L = hold a note while you drag" }

        gramarye.ui.render(ui.Column {
            id = "synth_root", w = { grow = true }, h = { grow = true }, gap = 0,
            widgets.HeaderBar {
                title   = "Synth Lab",
                actions = { ui.Button { id = "synth_back", label = "×", w = 36, h = 36, on_click = back } },
            },
            ui.Row { w = { grow = true }, h = { grow = true }, gap = 0,
                ui.Column { w = { grow = true }, h = { grow = true }, gap = 0,
                    tabs,
                    ui.ScrollFrame { id = "synth_body", v_scroll = true,
                        layout = { dir = "column", w = { grow = true }, h = { grow = true },
                                   pad = { h = 14, v = 4 }, gap = 10 },
                        BODIES[tab](),
                    },
                },
                side_panel(),
            },
        })
    end,
}
