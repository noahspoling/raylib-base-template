local ui       = gramarye.require("lib/ui")
local edui     = gramarye.require("lib/editor_ui")
local camera   = gramarye.require("lib/camera")
local session  = gramarye.require("lib/tilemap_session")
local TILESETS = gramarye.require("lib/tilesets")
local TERRAINS = gramarye.require("lib/terrains")
local stress   = gramarye.require("lib/stress_map")
local tm       = gramarye.tilemap
local input    = gramarye.input

local MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE = 0, 1, 2

local NEW_TILE_LAYERS = { "detail", "objects" }
local LEVEL_CHOICES   = { 4, 8, 12, 16, 24, 32 }
local SLOPE_STEP  = 4
local BRUSHES     = { 1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 24, 32 }
local TILT_STEP   = 5
local ROT_SPEED   = 90
local ROT_STEP    = 15
local FLOW_MIN_MOVE = 4
local ORBIT_TURN  = 0.4
local ORBIT_TILT  = 0.3
local SIZES       = { 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024 }
local PAN_SPEED   = 600
local NAME_MAX    = 48

local BAR_BG  = edui.BAR_BG
local VIEW_BG = { 24, 26, 36, 255 }
local EDGE_H  = 2
local GRAB_H  = 12
local MIN_PX  = ui.MIN_TAP_H

local KEY = {}
for _, k in ipairs { "escape", "space", "lctrl", "rctrl", "lshift", "rshift", "lalt",
                     "b", "e", "f", "i", "c", "g", "z", "y", "s", "w", "a", "d",
                     "up", "down", "left", "right", "minus", "equal", "lbracket", "rbracket",
                     "comma", "period", "slash", "pageup", "pagedown",
                     "1", "2", "3", "4", "5", "6", "7", "8", "9" } do
    KEY[k] = input.key(k)
end

local btn      = edui.button
local short_id = edui.short_id
local leave    = edui.leave

local function clamp(v, lo, hi) return math.max(lo, math.min(v, hi)) end

local function memo(cache, key, make)
    local f = cache[key]
    if not f then f = make(key); cache[key] = f end
    return f
end

local cam = camera.new()

local ed = {
    tilesets = {},
    terrains = {},
    layers   = {},
    kinds    = {},
    levels   = {},
    layer    = 1,
    level    = 0,
    column   = true,
    cut      = false,
    terrain  = 1,
    brush    = 1,
    brush_i  = 1,
    water_depth = 1,
    water_bed = "riverbed",
    job      = nil,
    pois     = {},
    poi_names = {},
    flow_dir = 180,
    flag     = "solid",
    show_flags = false,
    tool     = "brush",
    tileset  = 1,
    sel      = { x = 0, y = 0, w = 1, h = 1 },
    palette_scale = 2,
    grid     = true,
    dim      = false,
    status   = "",

    hover    = nil,
    stroke   = nil,
    pan      = nil,
    pal_drag = nil,
    fit      = true,

    dialog   = nil,
    confirm  = nil,
    form     = { name = "", wi = 3, hi = 3, li = 2, gen = false },
    maps     = {},
    targets  = {},
    target   = 1,
}

local function map() return session.map end

local function display_name(id, defs)
    for _, def in ipairs(defs or TILESETS) do
        if def.id == id then return def.name or id end
    end
    return id
end

local function on_terrain() return ed.kinds[ed.layer] == "terrain" end

local function flowing()
    local t = ed.terrains[ed.terrain]
    return on_terrain() and t ~= nil and (t.flow or 0) > 0
end

local function refresh_targets()
    ed.targets = {}
    for _, p in ipairs(gramarye.mods.active()) do
        local game, launcher = false, false
        for _, a in ipairs(p.apps) do
            if a == "game" then game = true elseif a == "launcher" then launcher = true end
        end
        if game and not launcher then ed.targets[#ed.targets + 1] = p.id end
    end
    ed.target = #ed.targets
    for i, id in ipairs(ed.targets) do
        if id == session.package then ed.target = i end
    end
end

local function target_id() return ed.targets[ed.target] end

local function refresh_layers()
    local m = map()
    ed.layers = m:layers()
    ed.kinds, ed.levels = {}, {}
    for i = 1, #ed.layers do
        ed.kinds[i]  = m:layer_kind(i)
        ed.levels[i] = m:layer_elevation(i)
    end
end

local function level_layer(k, create)
    for i, kind in ipairs(ed.kinds) do
        if kind == "terrain" and ed.levels[i] == k then return i end
    end
    if not create then return nil end
    local l = map():add_layer("level " .. k, "terrain", k)
    if l then refresh_layers() end
    return l
end

local function extra_terrain(i)
    return ed.kinds[i] == "terrain" and level_layer(ed.levels[i]) ~= i
end

local function active_level()
    return ed.levels[ed.layer] or 0
end

local function select_level(k)
    k = clamp(k, 0, map():levels() - 1)
    local l = level_layer(k, true)
    if not l then
        ed.status = "no room for another layer"
        return
    end
    ed.level, ed.layer = k, l
end

local function use_map(m, name, package)
    session.map, session.name, session.package = m, name, package
    for _, def in ipairs(TILESETS) do
        local ok, err = m:add_tileset(def)
        if not ok then gramarye.log("tilemap editor: " .. tostring(err)) end
    end
    for _, def in ipairs(TERRAINS) do
        local ok, err = m:add_terrain(def)
        if not ok then gramarye.log("tilemap editor: " .. tostring(err)) end
    end
    ed.tilesets = m:tilesets()
    ed.terrains = m:terrains()
    refresh_layers()
    ed.layer    = 1
    ed.level    = ed.levels[1] or 0
    ed.tileset  = clamp(ed.tileset, 1, math.max(1, #ed.tilesets))
    ed.terrain  = clamp(ed.terrain, 1, math.max(1, #ed.terrains))
    ed.sel      = { x = 0, y = 0, w = 1, h = 1 }
    ed.stroke, ed.pan, ed.pal_drag = nil, nil, nil
    ed.fit      = true
    cam.tilt    = select(2, m:elevation())
    refresh_targets()
end

local function new_map(w, h, levels)
    local layers = {}
    for k = 0, levels - 1 do layers[#layers + 1] = { name = "level " .. k, kind = "terrain", elevation = k } end
    for _, name in ipairs(NEW_TILE_LAYERS) do layers[#layers + 1] = name end
    use_map(tm.new(w, h, { tile = 16, levels = levels, layers = layers }), nil, nil)
    ed.pois, ed.poi_names = {}, {}
    ed.status = ("new %dx%dx%d map"):format(w, h, levels)
end

local function start_generated(size, levels)
    ed.job = stress.job(math.max(size, 64), { levels = math.max(levels, stress.LEVELS) })
    ed.job_t = 0
    ed.dialog = "busy"
end

local function finish_generated()
    local job = ed.job
    ed.job = nil
    ed.dialog = nil
    use_map(job.map, nil, nil)
    ed.pois, ed.poi_names, ed.poi = job.pois, {}, 0
    for i, p in ipairs(job.pois) do ed.poi_names[i] = p.name end
    local w, h = job.map:size()
    ed.status = ("generated %dx%dx%d in %.1f s, %d places to go to"):format(w, h, job.map:levels(), ed.job_t, #job.pois)
end

local function open_map(name)
    local m, pkg = tm.load(name)
    if not m then
        ed.status = "open failed: " .. tostring(pkg)
        return
    end
    use_map(m, name, pkg)
    ed.status = ("opened %s from %s"):format(name, short_id(pkg))
end

local function open_dialog(kind)
    ed.dialog = kind
    ed.stroke, ed.pan, ed.pal_drag = nil, nil, nil
    if kind == "open"    then ed.maps = tm.list() end
    if kind == "save_as" then ed.form.name = session.name or "" end
end

local function close_dialog()
    ed.dialog, ed.confirm = nil, nil
end

local function guard(text, action)
    if map() and map():dirty() then
        ed.confirm = { text = text, yes = action }
        ed.dialog  = "confirm"
    else
        action()
    end
end

local function confirm_yes()
    local action = ed.confirm and ed.confirm.yes
    close_dialog()
    if action then action() end
end

local function save_to(name)
    local pkg = target_id()
    if not pkg then
        ed.status = "no mounted package can take maps"
        return false
    end
    local ok, err = map():save(pkg, name)
    if not ok then
        ed.status = "save failed: " .. tostring(err)
        return false
    end
    session.name, session.package = name, pkg
    ed.status = ("saved %s to %s"):format(name, short_id(pkg))
    return true
end

local function save()
    if session.name then save_to(session.name) else open_dialog("save_as") end
end

local function submit_save_as()
    local name = ed.form.name
    if #name == 0 or #name > NAME_MAX or not name:match("^[%w_%-]+$") then
        ed.status = "name: 1-48 letters, digits, _ or -"
        return
    end
    local function write()
        if save_to(name) then close_dialog() end
    end
    if name ~= session.name then
        for _, m in ipairs(tm.list()) do
            if m.name == name then
                ed.confirm = { text = ("Overwrite '%s' (%s)?"):format(name, short_id(m.package)), yes = write }
                ed.dialog  = "confirm"
                return
            end
        end
    end
    write()
end

local function undo()
    local l = map():undo()
    ed.status = l and ("undo (" .. ed.layers[l] .. ")") or "nothing to undo"
end

local function redo()
    local l = map():redo()
    ed.status = l and ("redo (" .. ed.layers[l] .. ")") or "nothing to redo"
end

local function open_viewer() gramarye.scene.push("tilemap_view") end

local function select_tileset(i)
    if #ed.tilesets == 0 then return end
    i = ((i - 1) % #ed.tilesets) + 1
    if i ~= ed.tileset then
        ed.tileset = i
        ed.sel = { x = 0, y = 0, w = 1, h = 1 }
    end
end

local function select_terrain(i)
    if #ed.terrains == 0 then return end
    ed.terrain = ((i - 1) % #ed.terrains) + 1
    if ed.tool ~= "fill" then ed.tool = "brush" end
end

local function cycle_target()
    if #ed.targets > 0 then ed.target = ed.target % #ed.targets + 1 end
end

local function change_level(d)
    map():set_layer_elevation(ed.layer, map():layer_elevation(ed.layer) + d)
    ed.levels[ed.layer] = map():layer_elevation(ed.layer)
end

local function change_levels(d)
    local n = map():levels()
    if d > 0 then
        map():set_levels(n + 1)
        ed.status = ("%d levels"):format(map():levels())
        return
    end
    if n <= 1 then return end
    local top = {}
    for i, kind in ipairs(ed.kinds) do
        if kind == "terrain" and ed.levels[i] == n - 1 then
            if not map():layer_empty(i) then
                ed.status = ("level %d has blocks: erase them first"):format(n - 1)
                return
            end
            top[#top + 1] = i
        end
    end
    local was_terrain, was_name = on_terrain(), ed.layers[ed.layer]
    for j = #top, 1, -1 do map():remove_layer(top[j]) end
    map():set_levels(n - 1)
    refresh_layers()
    if was_terrain then
        select_level(math.min(ed.level, n - 2))
    else
        ed.layer = 1
        for i, name in ipairs(ed.layers) do
            if name == was_name and ed.kinds[i] ~= "terrain" then ed.layer = i end
        end
    end
    ed.status = ("%d levels%s"):format(n - 1, #top > 0 and " (undo history cleared)" or "")
end

local function change_style(d_height, d_tilt, d_run)
    local height, tilt, run = map():elevation()
    map():set_elevation(height + d_height, tilt + d_tilt, run + (d_run or 0))
    cam.tilt = select(2, map():elevation())
end

local function goto_poi(i)
    local p = ed.pois[i]
    if not p then return end
    local t = map():tile()
    cam.x, cam.y = (p.x + 0.5) * t, (p.y + 0.5) * t
    cam:set_zoom_index(8)
    ed.fit = false
    select_level(p.level)
    ed.status = ("%d/%d %s"):format(i, #ed.pois, p.name)
end

local on = {
    new     = function() guard("Discard unsaved changes and start a new map?", function() open_dialog("new") end) end,
    open    = function() open_dialog("open") end,
    save_as = function() open_dialog("save_as") end,
    grid    = function() ed.grid = not ed.grid end,
    dim     = function() ed.dim = not ed.dim end,
    layer_visible = function() map():set_layer_visible(ed.layer, not map():layer_visible(ed.layer)) end,
    pal_out = function() ed.palette_scale = clamp(ed.palette_scale - 1, 1, 4) end,
    pal_in  = function() ed.palette_scale = clamp(ed.palette_scale + 1, 1, 4) end,
    brush_dn = function() ed.brush_i = clamp(ed.brush_i - 1, 1, #BRUSHES); ed.brush = BRUSHES[ed.brush_i] end,
    brush_up = function() ed.brush_i = clamp(ed.brush_i + 1, 1, #BRUSHES); ed.brush = BRUSHES[ed.brush_i] end,
    depth_0  = function() ed.water_depth = 0 end,
    depth_1  = function() ed.water_depth = 1 end,
    depth_2  = function() ed.water_depth = 2 end,
    depth_3  = function() ed.water_depth = 3 end,
    bed_riverbed = function() ed.water_bed = "riverbed" end,
    bed_kelp = function() ed.water_bed = "kelp" end,
    bed_keep = function() ed.water_bed = "keep" end,
    form_empty = function() ed.form.gen = false end,
    form_gen = function() ed.form.gen = true end,
    level_dn  = function() change_level(-1) end,
    level_up  = function() change_level(1) end,
    tlevel_dn = function() select_level(ed.level - 1) end,
    tlevel_up = function() select_level(ed.level + 1) end,
    levels_dn = function() change_levels(-1) end,
    levels_up = function() change_levels(1) end,
    terrain_mode = function() select_level(ed.level) end,
    column    = function() ed.column = true end,
    block     = function() ed.column = false end,
    cut       = function() ed.cut = not ed.cut end,
    height_dn = function() change_style(-1, 0) end,
    height_up = function() change_style(1, 0) end,
    tilt_dn   = function() change_style(0, -TILT_STEP) end,
    tilt_up   = function() change_style(0, TILT_STEP) end,
    run_dn    = function() change_style(0, 0, -SLOPE_STEP) end,
    run_up    = function() change_style(0, 0, SLOPE_STEP) end,
    flag_solid = function() ed.flag = "solid"; ed.tool = "flags" end,
    flag_water = function() ed.flag = "water"; ed.tool = "flags" end,
    show_flags = function() ed.show_flags = not ed.show_flags end,
    rot_dn    = function() cam:rotate_by(-ROT_STEP) end,
    rot_up    = function() cam:rotate_by(ROT_STEP) end,
    rot_zero  = function() cam:orbit_to(0) end,
    create  = function()
        if ed.form.gen then
            start_generated(SIZES[ed.form.wi], LEVEL_CHOICES[ed.form.li])
            return
        end
        new_map(SIZES[ed.form.wi], SIZES[ed.form.hi], LEVEL_CHOICES[ed.form.li])
        close_dialog()
    end,
    goto_next = function()
        if #ed.pois == 0 then return end
        ed.poi = (ed.poi or 0) % #ed.pois + 1
        goto_poi(ed.poi)
    end,
    form_name = function(v) ed.form.name = v end,
}
local tool_fns, layer_fns, tileset_fns, terrain_fns, open_fns, step_fns = {}, {}, {}, {}, {}, {}

local function set_tool(t)    return memo(tool_fns, t, function(k) return function() ed.tool = k end end) end
local function set_layer(i)
    return memo(layer_fns, i, function(k)
        return function()
            ed.layer = k
            if ed.kinds[k] == "terrain" then ed.level = ed.levels[k] end
        end
    end)
end
local function set_tileset(i) return memo(tileset_fns, i, function(k) return function() select_tileset(k) end end) end
local function set_terrain(i) return memo(terrain_fns, i, function(k) return function() select_terrain(k) end end) end

local function open_entry(i)
    return memo(open_fns, i, function(k)
        return function()
            local entry = ed.maps[k]
            if not entry then return end
            guard("Discard unsaved changes and open '" .. entry.name .. "'?", function()
                open_map(entry.name)
                close_dialog()
            end)
        end
    end)
end

local function step(field, delta)
    return memo(step_fns, field .. delta, function()
        return function()
            ed.form[field] = clamp(ed.form[field] + delta, 1, field == "li" and #LEVEL_CHOICES or #SIZES)
        end
    end)
end

local function layer_entries()
    local out = { { id = "layer_terrain", label = "Terrain", terrain = true, select = on.terrain_mode,
                    active = on_terrain() and not extra_terrain(ed.layer) } }
    for i, name in ipairs(ed.layers) do
        if ed.kinds[i] ~= "terrain" or extra_terrain(i) then
            local level = (ed.levels[i] or 0) > 0 and (" ^" .. ed.levels[i]) or ""
            out[#out + 1] = { id = "layer_" .. i, label = name .. level .. (map():layer_visible(i) and "" or " (hidden)"),
                              select = set_layer(i), active = ed.layer == i }
        end
    end
    return out
end

local function ctrl()  return input.key_down(KEY.lctrl) or input.key_down(KEY.rctrl) end
local function shift() return input.key_down(KEY.lshift) or input.key_down(KEY.rshift) end

local function cell_at(rect, mx, my)
    local wx, wy = cam:screen_to_world(rect, mx, my, map():layer_height(ed.layer))
    local t = map():tile()
    return math.floor(wx / t), math.floor(wy / t), wx, wy
end

local function tool_size()
    if ed.tool == "brush" or ed.tool == "erase" or ed.tool == "flags" then
        if on_terrain() then return ed.brush, ed.brush end
        return ed.sel.w, ed.sel.h
    end
    return 1, 1
end

local function snap(cx, cy)
    if on_terrain() then
        local w, h = tool_size()
        return cx - (w - 1) // 2, cy - (h - 1) // 2
    end
    local s = ed.stroke
    if not s then return cx, cy end
    local w, h = tool_size()
    return s.x0 + math.floor((cx - s.x0) / w) * w, s.y0 + math.floor((cy - s.y0) / h) * h
end

local function painting_water()
    local t = ed.terrains[ed.terrain]
    return on_terrain() and t ~= nil and t.water == true
end

local function terrain_slot(id)
    for i, t in ipairs(ed.terrains) do if t.id == id then return i end end
    return nil
end

local function carve_water(cx, cy, w, h)
    local k = ed.level
    local d = math.min(ed.water_depth, k)
    local changed = 0
    local L = map():levels()
    for j = k + 1, L - 1 do
        local l = level_layer(j)
        if l then changed = changed + map():erase(l, cx, cy, w, h) end
    end
    local dir = flowing() and ed.flow_dir or nil
    for j = k - d + 1, k do
        local l = level_layer(j, true)
        if l then changed = changed + map():fill(l, cx, cy, w, h, ed.terrain, j == k and dir or nil) end
    end
    local bed = ed.water_bed ~= "keep" and terrain_slot(ed.water_bed) or nil
    local under = bed or terrain_slot("riverbed") or 1
    if k - d >= 0 then
        local l = level_layer(k - d, true)
        if l then
            changed = changed + (bed and map():fill(l, cx, cy, w, h, bed) or map():fill_empty(l, cx, cy, w, h, under))
        end
    end
    for j = 0, k - d - 1 do
        local l = level_layer(j, true)
        if l then changed = changed + map():fill_empty(l, cx, cy, w, h, under) end
    end
    return changed
end

local function apply_at(cx, cy)
    local s = ed.stroke
    cx, cy = snap(cx, cy)
    if s.lx == cx and s.ly == cy then return end
    s.lx, s.ly = cx, cy
    local w, h = tool_size()
    if not s.first then s.first = { x = cx, y = cy } end
    if ed.tool == "flags" then
        s.changed = s.changed + map():paint_flags(ed.layer, cx, cy, w, h, ed.flag, not s.erase)
    elseif s.erase then
        for _, l in ipairs(s.layers) do s.changed = s.changed + map():erase(l, cx, cy, w, h) end
    elseif on_terrain() and s.carve then
        s.changed = s.changed + carve_water(cx, cy, w, h)
    elseif on_terrain() then
        s.changed = s.changed + map():fill(ed.layer, cx, cy, w, h, ed.terrain, flowing() and ed.flow_dir or nil)
        for _, l in ipairs(s.layers) do
            if l ~= ed.layer then s.changed = s.changed + map():fill_empty(l, cx, cy, w, h, ed.terrain) end
        end
    else
        s.changed = s.changed + map():stamp(ed.layer, cx, cy, ed.tileset, ed.sel.x, ed.sel.y, w, h)
    end
end

local function stroke_to(cx, cy)
    local s = ed.stroke
    if not s.px then
        apply_at(cx, cy)
    else
        local dx, dy = cx - s.px, cy - s.py
        local n = math.max(math.abs(dx), math.abs(dy))
        for i = 1, n do
            apply_at(s.px + math.floor(dx * i / n + 0.5), s.py + math.floor(dy * i / n + 0.5))
        end
    end
    s.px, s.py = cx, cy
end

local function track_flow(wx, wy)
    local s = ed.stroke
    if not (s and flowing() and not s.erase) then return end
    if not s.fx then s.fx, s.fy = wx, wy return end
    local dx, dy = wx - s.fx, wy - s.fy
    if dx * dx + dy * dy < FLOW_MIN_MOVE * FLOW_MIN_MOVE then return end
    ed.flow_dir = math.floor(math.deg(math.atan(dx, -dy)) + 0.5) % 360
    s.fx, s.fy = wx, wy
    if not s.aimed and s.first then
        s.aimed = true
        local w, h = tool_size()
        s.changed = s.changed + map():fill(ed.layer, s.first.x, s.first.y, w, h, ed.terrain, ed.flow_dir)
    end
end

local function pick(cx, cy)
    if on_terrain() then
        local level, t = ed.level, (map():get(ed.layer, cx, cy))
        if t == 0 then level, t = map():level_at(cx, cy, ed.level) end
        if t == 0 then
            ed.status = "nothing to pick at or below this level"
            return
        end
        ed.terrain, ed.tool = t, "brush"
        ed.status = ("picked %s (level %d)"):format(display_name(ed.terrains[t].id, TERRAINS), level)
        return
    end
    local ts, tile = map():get(ed.layer, cx, cy)
    local info = ed.tilesets[ts]
    if ts == 0 or not info or info.cols == 0 then
        ed.status = "nothing to pick on this layer"
        return
    end
    ed.tileset = ts
    ed.sel     = { x = tile % info.cols, y = tile // info.cols, w = 1, h = 1 }
    ed.tool    = "brush"
end

local function begin_stroke(button, cx, cy)
    if ed.tool == "pick" or input.key_down(KEY.lalt) then
        pick(cx, cy)
        return
    end
    local erase = button == MOUSE_RIGHT or ed.tool == "erase"
    local info  = ed.tilesets[ed.tileset]
    if ed.tool == "flags" and not on_terrain() then
        ed.status = "collision flags go on terrain layers"
        return
    elseif ed.tool == "flags" then
    elseif on_terrain() then
        if not erase and #ed.terrains == 0 then
            ed.status = "no terrains (add some to lib/terrains.lua)"
            return
        end
    elseif not erase and not (info and info.loaded) then
        ed.status = "no tileset image loaded"
        return
    end
    local layers = { ed.layer }
    local carve = not erase and ed.tool == "brush" and ed.column and painting_water()
                  and ed.water_depth > 0 and not extra_terrain(ed.layer)
    if on_terrain() and ed.column and not extra_terrain(ed.layer) and (ed.tool == "brush" or ed.tool == "erase") then
        layers = {}
        local from, to = 0, ed.level
        if erase then from, to = ed.level, map():levels() - 1 end
        if carve then to = map():levels() - 1 end
        for k = from, to do
            local l = level_layer(k, not erase and k <= ed.level)
            if l then layers[#layers + 1] = l end
        end
        ed.layer = level_layer(ed.level)
    end
    map():snapshot(table.unpack(layers))
    if ed.tool == "fill" and on_terrain() then
        local dir = (not erase and flowing()) and ed.flow_dir or nil
        if map():flood(ed.layer, cx, cy, erase and 0 or ed.terrain, dir) == 0 then map():drop_snapshot() end
        return
    elseif ed.tool == "fill" then
        local tile = ed.sel.x + ed.sel.y * (info and info.cols or 0)
        local n = map():flood(ed.layer, cx, cy, erase and 0 or ed.tileset, erase and 0 or tile)
        if n == 0 then map():drop_snapshot() end
        return
    end
    ed.stroke = { button = button, erase = erase, x0 = cx, y0 = cy, changed = 0, layers = layers, carve = carve }
    stroke_to(cx, cy)
end

local function end_stroke()
    if ed.stroke.changed == 0 then map():drop_snapshot() end
    ed.stroke = nil
end

local function palette_tile_at(mx, my)
    local info = ed.tilesets[ed.tileset]
    if not (info and info.loaded) then return nil end
    local px, py = gramarye.ui.element_rect("tm_palette")
    local tw, th = info.tile_w * ed.palette_scale, info.tile_h * ed.palette_scale
    local tx, ty = math.floor((mx - px) / tw), math.floor((my - py) / th)
    local inside = tx >= 0 and ty >= 0 and tx < info.cols and ty < info.rows
    return clamp(tx, 0, info.cols - 1), clamp(ty, 0, info.rows - 1), inside
end

local function update_palette(mx, my)
    if ed.pal_drag then
        if not input.mouse_down(MOUSE_LEFT) then
            ed.pal_drag = nil
            return
        end
        local tx, ty = palette_tile_at(mx, my)
        if tx then
            local a = ed.pal_drag
            ed.sel = { x = math.min(a.x, tx), y = math.min(a.y, ty),
                       w = math.abs(tx - a.x) + 1, h = math.abs(ty - a.y) + 1 }
        end
    elseif input.mouse_pressed(MOUSE_LEFT) and gramarye.ui.hovered("tm_palette") then
        local tx, ty, inside = palette_tile_at(mx, my)
        if inside then
            ed.pal_drag = { x = tx, y = ty }
            ed.sel = { x = tx, y = ty, w = 1, h = 1 }
            if ed.tool ~= "fill" then ed.tool = "brush" end
        end
    end
end

local function update_keys(dt, rect)
    if input.key_pressed(KEY.escape) then leave() return true end
    if ctrl() then
        if input.key_pressed(KEY.z) then if shift() then redo() else undo() end end
        if input.key_pressed(KEY.y) then redo() end
        if input.key_pressed(KEY.s) then save() end
        return false
    end
    if input.key_pressed(KEY.b) then ed.tool = "brush" end
    if input.key_pressed(KEY.e) then ed.tool = "erase" end
    if input.key_pressed(KEY.f) then ed.tool = "fill" end
    if input.key_pressed(KEY.i) then ed.tool = "pick" end
    if input.key_pressed(KEY.c) then ed.tool = "flags" end
    if input.key_pressed(KEY.g) then ed.grid = not ed.grid end
    local entries = layer_entries()
    for i = 1, math.min(9, #entries) do
        if input.key_pressed(KEY[tostring(i)]) then entries[i].select() end
    end
    if input.key_pressed(KEY.pageup) then select_level(ed.level + 1) end
    if input.key_pressed(KEY.pagedown) then select_level(ed.level - 1) end
    if on_terrain() then
        if input.key_pressed(KEY.lbracket) then select_terrain(ed.terrain - 1) end
        if input.key_pressed(KEY.rbracket) then select_terrain(ed.terrain + 1) end
    else
        if input.key_pressed(KEY.lbracket) then select_tileset(ed.tileset - 1) end
        if input.key_pressed(KEY.rbracket) then select_tileset(ed.tileset + 1) end
    end
    local rot = (input.key_down(KEY.period) and 1 or 0) - (input.key_down(KEY.comma) and 1 or 0)
    if rot ~= 0 then cam:rotate_by(rot * ROT_SPEED * dt) end
    if input.key_pressed(KEY.slash) then cam:orbit_to(0) end
    local cx, cy = rect.x + rect.w / 2, rect.y + rect.h / 2
    if input.key_pressed(KEY.minus) then cam:zoom_by(-1, rect, cx, cy) end
    if input.key_pressed(KEY.equal) then cam:zoom_by(1, rect, cx, cy) end
    if input.key_down(KEY.space) then return false end
    local dx = ((input.key_down(KEY.right) or input.key_down(KEY.d)) and 1 or 0)
             - ((input.key_down(KEY.left)  or input.key_down(KEY.a)) and 1 or 0)
    local dy = ((input.key_down(KEY.down)  or input.key_down(KEY.s)) and 1 or 0)
             - ((input.key_down(KEY.up)    or input.key_down(KEY.w)) and 1 or 0)
    if dx ~= 0 or dy ~= 0 then cam:pan_screen(-dx * PAN_SPEED * dt, -dy * PAN_SPEED * dt) end
    return false
end

local bottom = {
    frac          = 0.30,
    min_frac      = 0.10,
    max_frac      = 0.60,
    minimized     = false,
    is_resizeable = true,

    dragging      = false,
    was_down      = false,
}

local function bottom_height(sh)
    if bottom.minimized then return EDGE_H end
    local px = bottom.frac * sh
    return math.floor(clamp(px, math.max(MIN_PX, bottom.min_frac * sh), bottom.max_frac * sh))
end

local function update_bottom_drag(sh)
    local _, my = gramarye.ui.mouse_pos()
    local down  = gramarye.ui.mouse_down()

    if not down then
        bottom.dragging = false
    elseif bottom.dragging then
        bottom.frac = clamp((sh - my) / sh, bottom.min_frac, bottom.max_frac)
    elseif not bottom.was_down and bottom.is_resizeable
           and gramarye.ui.hovered("bottom_handle") then
        bottom.dragging  = true
        bottom.minimized = false
    end
    bottom.was_down = down
end

local function toggle_minimized() bottom.minimized = not bottom.minimized end

local function top_bar()
    local w, h = map():size()
    return ui.Row {
        id = "top_bar", bg = BAR_BG,
        w = { grow = true }, h = 44, pad = { h = 8 }, gap = 6,
        align = { y = "center" },
        ui.Label { text = "Tilemap", size = 16 },
        ui.Label { text = ("%s%s   %dx%dx%d   -> %s"):format(session.name or "untitled",
                       map():dirty() and " *" or "", w, h, map():levels(), short_id(target_id())),
                   skin = "label_dim" },
        ui.Spacer(),
        btn("tb_new", "New", on.new),
        btn("tb_open", "Open", on.open),
        btn("tb_save", "Save", save),
        btn("tb_save_as", "Save as", on.save_as),
        ui.Spacer(8),
        btn("tb_undo", "Undo", undo),
        btn("tb_redo", "Redo", redo),
        ui.Spacer(8),
        #ed.pois > 0 and btn("tb_goto", "Go to >", on.goto_next) or ui.Spacer(0),
        btn("tb_view", "View >", open_viewer),
        ui.Button { id = "top_close", label = "×", w = 32, h = 32, on_click = leave },
    }
end

local TOOLS = { { "brush", "Brush" }, { "erase", "Erase" }, { "fill", "Fill" }, { "pick", "Pick" }, { "flags", "Flags" } }

local function tool_bar()
    local row = ui.Row {
        id = "tool_bar", bg = { 28, 28, 44, 255 },
        w = { grow = true }, h = 40, pad = { h = 8 }, gap = 4,
        align = { y = "center" },
    }
    for _, t in ipairs(TOOLS) do
        row[#row + 1] = btn("tool_" .. t[1], t[2], set_tool(t[1]), ed.tool == t[1])
    end
    row[#row + 1] = ui.Spacer(12)
    for i, e in ipairs(layer_entries()) do
        row[#row + 1] = btn(e.id, i .. " " .. e.label, e.select, e.active)
        if e.terrain and on_terrain() and not extra_terrain(ed.layer) then
            row[#row + 1] = ui.Label { text = ("level %d / %d"):format(ed.level, map():levels() - 1), skin = "label_dim" }
            row[#row + 1] = btn("tlevel_dn", "-", on.tlevel_dn, false, 26)
            row[#row + 1] = btn("tlevel_up", "+", on.tlevel_up, false, 26)
            row[#row + 1] = btn("br_column", "Column", on.column, ed.column)
            row[#row + 1] = btn("br_block", "Block", on.block, not ed.column)
            row[#row + 1] = ui.Spacer(6)
        end
    end
    row[#row + 1] = btn("layer_vis", map():layer_visible(ed.layer) and "Hide" or "Show", on.layer_visible)
    row[#row + 1] = ui.Spacer(12)
    row[#row + 1] = btn("tb_cut", "Cut above", on.cut, ed.cut)
    row[#row + 1] = btn("tb_grid", "Grid", on.grid, ed.grid)
    row[#row + 1] = btn("tb_dim", "Dim others", on.dim, ed.dim)
    row[#row + 1] = btn("tb_flags", "Collision", on.show_flags, ed.show_flags or ed.tool == "flags")
    row[#row + 1] = ui.Spacer()
    local where = ""
    local mw, mh = map():size()
    if ed.hover and ed.hover.x >= 0 and ed.hover.y >= 0 and ed.hover.x < mw and ed.hover.y < mh then
        local hx, hy = ed.hover.x, ed.hover.y
        local floor = map():level_at(hx, hy)
        local depth, _, deep = map():water_at(hx, hy)
        where = floor < 0 and ("%d, %d   no floor"):format(hx, hy) or ("%d, %d   floor %d"):format(hx, hy, floor)
        if deep then where = where .. "   open water"
        elseif depth > 0 then where = where .. ("   water %d (%s)"):format(depth, depth <= 1 and "wade" or "deep") end
    end
    if flowing() then where = where .. ("   flow %d"):format(ed.flow_dir) end
    local st = map():mesh_stats()
    local view = st.far and "overview" or ("chunks %d/%d"):format(st.built, st.chunks)
    if st.pending > 0 then view = view .. (" (%d to build)"):format(st.pending) end
    row[#row + 1] = ui.Label { text = ("%s   %gx   %s   %s"):format(ed.status, cam.zoom, view, where),
                               skin = "label_dim" }
    return row
end

local function elevation_row()
    local height, tilt, run = map():elevation()
    local row = ui.Row {
        gap = 4, align = { y = "center" },
        ui.Label { text = ("map levels %d"):format(map():levels()), skin = "label_dim" },
        btn("levels_dn", "-", on.levels_dn, false, 30),
        btn("levels_up", "+", on.levels_up, false, 30),
        ui.Spacer(12),
    }
    if not on_terrain() then
        row[#row + 1] = ui.Label { text = ("layer level %d"):format(ed.levels[ed.layer] or 0), skin = "label_dim" }
        row[#row + 1] = btn("level_dn", "-", on.level_dn, false, 30)
        row[#row + 1] = btn("level_up", "+", on.level_up, false, 30)
        row[#row + 1] = ui.Spacer(12)
    end
    for _, e in ipairs {
        ui.Label { text = ("level height %d px"):format(height), skin = "label_dim" },
        btn("height_dn", "-", on.height_dn, false, 30),
        btn("height_up", "+", on.height_up, false, 30),
        ui.Spacer(12),
        ui.Label { text = ("incline run %d px"):format(run), skin = "label_dim" },
        btn("run_dn", "-", on.run_dn, false, 30),
        btn("run_up", "+", on.run_up, false, 30),
        ui.Spacer(12),
        ui.Label { text = ("map tilt %d deg"):format(math.floor(tilt + 0.5)), skin = "label_dim" },
        btn("tilt_dn", "-", on.tilt_dn, false, 30),
        btn("tilt_up", "+", on.tilt_up, false, 30),
        ui.Spacer(12),
        ui.Label { text = ("view turned %d deg"):format(math.floor(cam.rotation + 0.5) % 360), skin = "label_dim" },
        btn("rot_dn", "-", on.rot_dn, false, 30),
        btn("rot_up", "+", on.rot_up, false, 30),
        btn("rot_zero", "0", on.rot_zero, false, 30),
    } do row[#row + 1] = e end
    return row
end

local function flags_palette()
    return ui.Column {
        id = "palette_col", w = { grow = true }, h = { grow = true }, gap = 8,
        ui.Row { gap = 6, align = { y = "center" },
                 ui.Frame { bg = { 235, 70, 60, 255 }, layout = { w = 18, h = 18 } },
                 btn("flag_solid", "Obstruction", on.flag_solid, ed.flag == "solid"),
                 ui.Frame { bg = { 70, 150, 255, 255 }, layout = { w = 18, h = 18 } },
                 btn("flag_water", "Water", on.flag_water, ed.flag == "water") },
        ui.Row { gap = 4, align = { y = "center" },
                 ui.Label { text = ("brush %dx%d"):format(ed.brush, ed.brush), skin = "label_dim" },
                 btn("brush_dn", "-", on.brush_dn, false, 30),
                 btn("brush_up", "+", on.brush_up, false, 30) },
        ui.Label { text = "Left-drag sets the flag on this layer's cells, right-drag clears it. Walkers stop at obstructions and water unless they can pass them (map:can_step).",
                   skin = "label_dim" },
        ui.Label { text = "Strong = painted here; faint = from the terrain (lib/terrains: water, solid). Cliffs (2+ levels) block too, unless the higher cell is stairs.",
                   skin = "label_dim" },
    }
end

local function water_options()
    if not painting_water() then return ui.Spacer(0) end
    local row = ui.Row { gap = 4, align = { y = "center" }, ui.Spacer(16),
                         ui.Label { text = "water depth", skin = "label_dim" } }
    local labels = { [0] = "as painted", "1 (wade)", "2", "3 (bridge)" }
    for d = 0, 3 do row[#row + 1] = btn("depth_" .. d, labels[d], on["depth_" .. d], ed.water_depth == d) end
    row[#row + 1] = ui.Spacer(10)
    row[#row + 1] = ui.Label { text = "bed", skin = "label_dim" }
    for _, b in ipairs { "riverbed", "kelp", "keep" } do
        row[#row + 1] = btn("bed_" .. b, b == "keep" and "Keep" or display_name(b, TERRAINS), on["bed_" .. b], ed.water_bed == b)
    end
    return row
end

local function terrain_palette()
    if ed.tool == "flags" then return flags_palette() end
    local row = ui.Row { gap = 6, align = { y = "center" } }
    for i, t in ipairs(ed.terrains) do
        row[#row + 1] = ui.Row {
            gap = 4, align = { y = "center" },
            ui.Frame { bg = t.swatch, layout = { w = 18, h = 18 } },
            btn("tr_" .. i, display_name(t.id, TERRAINS), set_terrain(i), ed.terrain == i),
        }
    end
    if #ed.terrains == 0 then
        row[#row + 1] = ui.Label { text = "No terrains: add some to lib/terrains.lua", skin = "label_dim" }
    end
    return ui.Column {
        id = "palette_col", w = { grow = true }, h = { grow = true }, gap = 8,
        row,
        ui.Row { gap = 4, align = { y = "center" },
                 ui.Label { text = ("brush %dx%d"):format(ed.brush, ed.brush), skin = "label_dim" },
                 btn("brush_dn", "-", on.brush_dn, false, 30),
                 btn("brush_up", "+", on.brush_up, false, 30),
                 water_options() },
        elevation_row(),
        ui.Label { text = "Later terrains draw over earlier ones. Drag a river downstream to set its flow. A floor one level above its neighbor slopes down to it; two or more is a cliff (paint Stairs on its edge to climb it). Column fills the levels under a block and erases the ones above; Block paints one level: hollow caves, lay bridges (Cut above to see inside).",
                   skin = "label_dim" },
    }
end

local function palette()
    if on_terrain() then return terrain_palette() end
    local info = ed.tilesets[ed.tileset]
    local tabs = ui.Row { gap = 4, align = { y = "center" } }
    for i, t in ipairs(ed.tilesets) do
        tabs[#tabs + 1] = btn("ts_" .. i, display_name(t.id), set_tileset(i), ed.tileset == i)
    end
    tabs[#tabs + 1] = ui.Spacer()
    tabs[#tabs + 1] = ui.Label { text = ("selection %dx%d   zoom %dx"):format(ed.sel.w, ed.sel.h, ed.palette_scale),
                                 skin = "label_dim" }
    tabs[#tabs + 1] = btn("pal_out", "-", on.pal_out, false, 30)
    tabs[#tabs + 1] = btn("pal_in", "+", on.pal_in, false, 30)

    local pw = info and info.loaded and info.cols * info.tile_w * ed.palette_scale or 0
    local ph = info and info.loaded and info.rows * info.tile_h * ed.palette_scale or 0
    return ui.Column {
        id = "palette_col", w = { grow = true }, h = { grow = true }, gap = 6,
        tabs,
        elevation_row(),
        ui.ScrollFrame {
            id = "tm_palette_scroll", v_scroll = true, h_scroll = true,
            bg = { 14, 14, 22, 255 },
            layout = { w = { grow = true }, h = { grow = true } },
            { _type = "custom", id = "tm_palette", kind = tm.PALETTE_KIND, layout = { w = pw, h = ph } },
        },
    }
end

local function bottom_menu(sh)
    local handle_hot = bottom.dragging or gramarye.ui.hovered("bottom_handle")

    local panel = ui.Frame {
        id     = "bottom_menu",
        bg     = BAR_BG,
        layout = { dir = "column", w = { grow = true }, h = bottom_height(sh) },

        ui.Frame {
            bg     = handle_hot and { 90, 140, 220, 255 } or { 40, 40, 64, 255 },
            layout = { w = { grow = true }, h = EDGE_H },
        },

        ui.Frame {
            id       = "bottom_handle",
            floating = { to = "parent", z = 5,
                         attach = { element = "left_center", parent = "left_top" } },
            layout   = { w = gramarye.ui.screen_w(), h = GRAB_H },
        },

        ui.Frame {
            floating = { to = "parent", x = -8, z = 10,
                         attach = { element = "right_bottom", parent = "right_top" } },
            ui.Button {
                id       = "bottom_minimize",
                label    = bottom.minimized and "^" or "v",
                w        = 32, h = 24,
                on_click = toggle_minimized,
            },
        },
    }

    if not bottom.minimized then
        panel[#panel + 1] = ui.Row {
            id = "bottom_content",
            w = { grow = true }, h = { grow = true }, pad = 8, gap = 8,
            palette(),
        }
    end
    return panel
end

local function centered_x(w) return math.floor((gramarye.ui.screen_w() - w) / 2) end

local function stepper(label, field)
    local choices = field == "li" and LEVEL_CHOICES or SIZES
    return ui.Row {
        gap = 8, align = { y = "center" },
        ui.Frame { layout = { w = 70 }, ui.Label { text = label } },
        btn("tm_" .. field .. "_dn", "-", step(field, -1), false, 32),
        ui.Frame { layout = { w = 48, align = "center" }, ui.Label { text = tostring(choices[ed.form[field]]) } },
        btn("tm_" .. field .. "_up", "+", step(field, 1), false, 32),
    }
end

local function dialog_ui()
    if ed.dialog == "new" then
        return ui.Popup {
            id = "tm_dlg_new", title = "New map", x = centered_x(320), y = 160, w = 320,
            on_close = close_dialog,
            ui.Row { gap = 8, btn("tm_new_empty", "Empty", on.form_empty, not ed.form.gen),
                     btn("tm_new_gen", "Generated", on.form_gen, ed.form.gen) },
            stepper(ed.form.gen and "Size" or "Width", "wi"),
            ed.form.gen and ui.Spacer(0) or stepper("Height", "hi"),
            stepper("Levels", "li"),
            ui.Label { text = ed.form.gen
                           and "Rolling land with caves, rivers, lakes, plateaus, falls and a deep river with bridges (at least 16 levels). Go to... jumps between them."
                           or  "16 px tiles; a terrain layer per level (level 0 is the ground), then detail and objects",
                       skin = "label_dim" },
            ui.Row { gap = 8, btn("tm_new_ok", "Create", on.create), btn("tm_new_cancel", "Cancel", close_dialog) },
        }
    elseif ed.dialog == "busy" then
        return ui.Popup {
            id = "tm_dlg_busy", title = "Generating", x = centered_x(320), y = 200, w = 320,
            ui.Label { text = ("%d%%"):format(math.floor((ed.job and ed.job.progress or 1) * 100 + 0.5)) },
        }
    elseif ed.dialog == "open" then
        local body = ui.Popup {
            id = "tm_dlg_open", title = "Open map", x = centered_x(380), y = 120, w = 380,
            on_close = close_dialog,
        }
        if #ed.maps == 0 then
            body[#body + 1] = ui.Label { text = "No saved maps yet (maps/*.lua in the mounted packages).",
                                         skin = "label_dim" }
        else
            local list = ui.ScrollFrame {
                id = "tm_open_list",
                layout = { dir = "column", w = { grow = true }, h = math.min(#ed.maps * 34, 320), gap = 4 },
            }
            for i, entry in ipairs(ed.maps) do
                list[#list + 1] = btn("tm_open_" .. i, ("%s   (%s)"):format(entry.name, short_id(entry.package)),
                                      open_entry(i), entry.name == session.name, { grow = true })
            end
            body[#body + 1] = list
        end
        body[#body + 1] = btn("tm_open_cancel", "Cancel", close_dialog)
        return body
    elseif ed.dialog == "save_as" then
        return ui.Popup {
            id = "tm_dlg_save", title = "Save map as", x = centered_x(380), y = 160, w = 380,
            on_close = close_dialog,
            ui.TextBox { id = "tm_save_name", value = ed.form.name, w = 340,
                         placeholder = "click and type a name (letters, digits, _ -)",
                         on_change = on.form_name, on_submit = submit_save_as },
            ui.Row { gap = 8, align = { y = "center" },
                     ui.Label { text = "Package" },
                     btn("tm_target", short_id(target_id()), cycle_target) },
            ui.Label { text = ("writes %s/maps/<name>.lua"):format(short_id(target_id())), skin = "label_dim" },
            ui.Label { text = ed.status, skin = "label_dim" },
            ui.Row { gap = 8, btn("tm_save_ok", "Save", submit_save_as), btn("tm_save_cancel", "Cancel", close_dialog) },
        }
    elseif ed.dialog == "confirm" then
        return ui.Popup {
            id = "tm_dlg_confirm", title = "Are you sure?", x = centered_x(360), y = 180, w = 360,
            on_close = close_dialog,
            ui.Label { text = ed.confirm and ed.confirm.text or "" },
            ui.Row { gap = 8, btn("tm_yes", "Yes", confirm_yes), btn("tm_no", "Cancel", close_dialog) },
        }
    end
end

return {
    on_enter = function()
        if session.map then
            use_map(session.map, session.name, session.package)
        else
            new_map(32, 32, 8)
        end
    end,

    on_exit = function()
        tm.view(nil)
        tm.palette(nil)
    end,

    on_update = function(dt)
        if ed.job then
            ed.job_t = ed.job_t + dt
            if ed.job:step(0.03) then finish_generated() end
            return
        end
        if ed.dialog then
            if input.key_pressed(KEY.escape) then close_dialog() end
            return
        end
        local rect   = camera.rect_of("tm_viewport")
        local mx, my = input.mouse_pos()
        local over   = gramarye.ui.hovered("tm_viewport")

        if ed.fit and rect.w > 0 then
            local w, h = map():size()
            cam:fit(rect, w * map():tile(), h * map():tile())
            ed.fit = false
        end

        if update_keys(dt, rect) then return end

        local wheel = input.wheel()
        if over and wheel ~= 0 then cam:zoom_by(wheel > 0 and 1 or -1, rect, mx, my) end
        cam:ease_orbit(dt)

        if ed.pan then
            if input.mouse_down(ed.pan.button) then
                if ed.pan.orbit then
                    cam:orbit_by((mx - ed.pan.x) * ORBIT_TURN, (my - ed.pan.y) * ORBIT_TILT)
                else
                    cam:pan_screen(mx - ed.pan.x, my - ed.pan.y, rect, ed.pan.x, ed.pan.y)
                end
                ed.pan.x, ed.pan.y = mx, my
            else
                ed.pan = nil
            end
        elseif over and not ed.stroke then
            if input.mouse_pressed(MOUSE_MIDDLE) then
                ed.pan = { button = MOUSE_MIDDLE, x = mx, y = my, orbit = not shift() }
            elseif input.key_down(KEY.space) and input.mouse_pressed(MOUSE_LEFT) then
                ed.pan = { button = MOUSE_LEFT, x = mx, y = my }
            end
        end

        ed.hover = nil
        if over or ed.stroke then
            local cx, cy, wx, wy = cell_at(rect, mx, my)
            ed.hover = { x = cx, y = cy }
            if ed.stroke then
                if input.mouse_down(ed.stroke.button) then
                    track_flow(wx, wy)
                    stroke_to(cx, cy)
                else
                    end_stroke()
                end
            elseif not ed.pan then
                if input.mouse_pressed(MOUSE_LEFT) then begin_stroke(MOUSE_LEFT, cx, cy)
                elseif input.mouse_pressed(MOUSE_RIGHT) then begin_stroke(MOUSE_RIGHT, cx, cy) end
            end
        end

        update_palette(mx, my)
    end,

    on_draw = function()
        local sh = gramarye.ui.screen_h()
        update_bottom_drag(sh)

        local v = cam:view { grid = ed.grid, bounds = true, layer = ed.layer, dim = ed.dim, bg = VIEW_BG,
                             flags = ed.show_flags or ed.tool == "flags",
                             arrows = flowing(), supersample = 2 }
        if ed.cut then v.cut_level = active_level() end
        local mw, mh = map():size()
        local w, h   = tool_size()
        local hx, hy = 0, 0
        if ed.hover then hx, hy = snap(ed.hover.x, ed.hover.y) end
        local on_map = ed.hover and hx + w > 0 and hy + h > 0 and hx < mw and hy < mh
        if on_map and not ed.dialog and not ed.pan then
            local erase = ed.tool == "erase" or (ed.stroke and ed.stroke.erase) or input.mouse_down(MOUSE_RIGHT)
            v.cursor = { x = hx, y = hy, w = w, h = h, erase = erase }
            if ed.tool == "brush" and not erase and on_terrain() then
                v.ghost = { x = hx, y = hy, w = w, h = h, terrain = ed.terrain }
            elseif ed.tool == "brush" and not erase then
                v.ghost = { x = hx, y = hy, tileset = ed.tileset, tx = ed.sel.x, ty = ed.sel.y, w = w, h = h }
            end
        end
        tm.view(map(), v)

        local pal = { scale = ed.palette_scale, select = ed.sel }
        if gramarye.ui.hovered("tm_palette") then
            local tx, ty, inside = palette_tile_at(input.mouse_pos())
            if inside then pal.hover = { x = tx, y = ty } end
        end
        tm.palette(map(), ed.tileset, pal)

        gramarye.ui.render(ui.Frame {
            id     = "editor_root",
            layout = { dir = "column", w = { grow = true }, h = { grow = true } },
            top_bar(),
            tool_bar(),
            { _type = "custom", id = "tm_viewport", kind = tm.VIEW_KIND,
              layout = { w = { grow = true }, h = { grow = true } } },
            bottom_menu(sh),
        })
        local dlg = dialog_ui()
        if dlg then gramarye.ui.render(dlg) end
    end,
}
