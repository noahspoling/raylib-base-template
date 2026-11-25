local ui     = gramarye.require("gramarye.ui")
local theme  = gramarye.require("gramarye.theme")
local camera = gramarye.require("lib/camera")
local arcs   = gramarye.require("lib/arcs")
local moves  = gramarye.require("lib/moves")
local inside = gramarye.require("lib/inside")
local water  = gramarye.require("lib/water")
local tm     = gramarye.tilemap
local input  = gramarye.input

local MAP        = "caves"
local STEP_TIME  = 0.16
local TILT_STEP  = 5
local ORBIT_TURN, ORBIT_TILT = 0.4, 0.3
local MOUSE_LEFT, MOUSE_MIDDLE = 0, 2
local OK, BAD    = { 120, 235, 130, 255 }, { 240, 95, 85, 255 }
local WALK, THROW = { 120, 200, 255, 255 }, { 255, 215, 110, 255 }

theme.apply {
    bd_btn          = { bg = { 40, 40, 64, 255 },   text = { 210, 210, 220, 255 }, radius = 4, font_size = 14, pad = { h = 10, v = 4 } },
    bd_btn_hover    = { bg = { 64, 64, 110, 255 },  text = { 255, 255, 255, 255 }, radius = 4, font_size = 14, pad = { h = 10, v = 4 } },
    bd_btn_on       = { bg = { 70, 110, 180, 255 }, text = { 255, 255, 255, 255 }, radius = 4, font_size = 14, pad = { h = 10, v = 4 } },
    bd_btn_on_hover = { bg = { 90, 135, 210, 255 }, text = { 255, 255, 255, 255 }, radius = 4, font_size = 14, pad = { h = 10, v = 4 } },
}

local KEY = {}
for _, k in ipairs { "escape", "w", "a", "s", "d", "n", "v", "lbracket", "rbracket", "1", "2", "3" } do
    KEY[k] = input.key(k)
end

local cam = camera.new { zoom_index = 8, smoothing = 8 }
local s = {
    map = nil, err = nil,
    body = arcs.body { move = 6, range = 3, up = 1, down = 2, height = 2 },
    mode = "walk", moves_left = 0,
    unit = { x = 0, y = 0, z = 0 },
    preview = nil, anim = nil, marks = nil, marks_key = nil,
    orbit = nil,
    inside = "cutaway",
    shown = nil,
    clock = 0,
}

local function cell_of(x, y) local t = s.map:tile() return math.floor(x / t), math.floor(y / t) end
local function place(cx, cy, z)
    local t = s.map:tile()
    s.unit.x, s.unit.y = (cx + 0.5) * t, (cy + 0.5) * t
    s.unit.z = z or math.max(0, (s.map:level_at(cx, cy)))
end
local function new_turn() s.moves_left, s.marks_key = s.body.move, nil end

local function along(points, u)
    local n = #points // 3
    local f = math.max(0, math.min(u, 1)) * (n - 1)
    local i = math.min(math.floor(f), n - 2)
    local k, a, b = f - i, i * 3, (i + 1) * 3
    return points[a + 1] + (points[b + 1] - points[a + 1]) * k,
           points[a + 2] + (points[b + 2] - points[a + 2]) * k,
           points[a + 3] + (points[b + 3] - points[a + 3]) * k
end

local function walk(path)
    local steps, cost = #path - 1, moves.cost(s.map, path, s.body)
    if steps < 1 or cost > s.moves_left then return end
    s.moves_left = s.moves_left - cost
    s.anim = { points = moves.points(s.map, path), t = 0, dur = steps * STEP_TIME, lands = path[#path].z }
end

local function hovered(rect, mx, my)
    local hx, hy, hz = inside.raycast(s.map, cam, rect, mx, my, s.shown)
    if not hx then return nil end
    local cx, cy = cell_of(hx, hy)
    return { x = cx, y = cy, z = arcs.level_of(s.map, hx, hy, hz) }
end

local function update_preview(h)
    s.preview = nil
    if not h or s.anim then return end
    local cx, cy = cell_of(s.unit.x, s.unit.y)
    local z = s.unit.z
    if h.x == cx and h.y == cy and h.z == z then return end
    if s.mode == "walk" then
        local path = moves.path(s.map, cx, cy, h.x, h.y, s.body, s.moves_left, z, h.z)
        if path then
            local pts = moves.points(s.map, path)
            for i = 3, #pts, 3 do pts[i] = pts[i] + 1.5 end
            local last = path[#path]
            s.preview = { ok = true, path = path, points = pts, to = { arcs.cell_point(s.map, last.x, last.y, last.z) } }
        end
    elseif s.mode == "jump" then
        s.preview = arcs.jump(s.map, s.unit.x, s.unit.y, h.x, h.y, s.body, z, h.z)
    else
        s.preview = arcs.throw(s.map, s.unit.x, s.unit.y, h.x, h.y, s.body, z, h.z)
    end
end

local function act()
    local r = s.preview
    if not r then return end
    if s.mode == "walk" and r.ok then
        walk(r.path)
    elseif s.mode == "jump" and r.ok then
        s.anim = { points = r.points, t = 0, dur = 0.35 + 0.06 * r.distance, lands = r.level }
    elseif s.mode == "throw" then
        s.anim = { points = r.points, t = 0, dur = 0.5 + 0.05 * r.distance, thrown = true }
    end
end

local function update_anim(dt)
    local a = s.anim
    if not a then return end
    a.t = a.t + dt / math.max(a.dur, 1e-3)
    if a.t < 1 then return end
    if a.lands then
        local n = #a.points // 3
        local cx, cy = cell_of(a.points[n * 3 - 2], a.points[n * 3 - 1])
        place(cx, cy, a.lands)
    end
    s.anim, s.marks_key = nil, nil
end

local function step_key()
    local dx = (input.key_pressed(KEY.d) and 1 or 0) - (input.key_pressed(KEY.a) and 1 or 0)
    local dy = (input.key_pressed(KEY.s) and 1 or 0) - (input.key_pressed(KEY.w) and 1 or 0)
    if dx == 0 and dy == 0 then return end
    local bx, by = cam:unrotate(dx, dy)
    if math.abs(bx) >= math.abs(by) then bx, by = bx > 0 and 1 or -1, 0 else bx, by = 0, by > 0 and 1 or -1 end
    local cx, cy = cell_of(s.unit.x, s.unit.y)
    local ok, _, level = s.map:can_step(cx, cy, cx + bx, cy + by, moves.walker(s.body, s.unit.z))
    if ok then walk { { x = cx, y = cy, z = s.unit.z }, { x = cx + bx, y = cy + by, z = level } } end
end

local function marks()
    if s.anim or s.mode == "throw" then return nil end
    local cx, cy = cell_of(s.unit.x, s.unit.y)
    local key = ("%s,%d,%d,%d,%d"):format(s.mode, cx, cy, s.unit.z, s.moves_left)
    if key ~= s.marks_key then
        s.marks, s.marks_key = {}, key
        if s.mode == "walk" then
            for _, c in ipairs((moves.range(s.map, cx, cy, s.body, s.moves_left, s.unit.z))) do
                s.marks[#s.marks + 1] = { x = c.x, y = c.y, level = c.z, color = { 110, 190, 255, 55 } }
            end
        else
            for _, c in ipairs(arcs.reach(s.map, s.unit.x, s.unit.y, s.body, s.unit.z)) do
                if c.ok then s.marks[#s.marks + 1] = { x = c.x, y = c.y, level = c.z, color = { 110, 230, 130, 60 } } end
            end
        end
    end
    return s.marks
end

local function button(id, label, on_click, active)
    return ui.Button { id = id, label = label, on_click = on_click, h = 30, skin = active and "bd_btn_on" or "bd_btn" }
end

local mode_fns = {}
local function set_mode(m)
    mode_fns[m] = mode_fns[m] or function() s.mode, s.marks_key = m, nil end
    return mode_fns[m]
end

local function next_inside() s.inside = inside.next(s.inside) end

local function leave()
    if gramarye.scene.depth() > 1 then gramarye.scene.pop() else gramarye.scene.change(gramarye.app.home()) end
end

local function status()
    local r = s.preview
    if not r then return "" end
    if s.mode == "walk" then return ("walk: %d step%s"):format(#r.path - 1, #r.path == 2 and "" or "s") end
    return ("%s: %s"):format(s.mode, r.ok and "ok" or (r.why or "no"))
end

return {
    on_enter = function()
        local m, err = tm.load(MAP)
        s.map, s.err = m, err
        if not m then return end
        local w, h = m:size()
        local t = m:tile()
        place(25, 19)
        cam.x, cam.y = s.unit.x, s.unit.y
        cam.tilt = select(2, m:elevation())
        cam.bounds = { 0, 0, w * t, h * t }
        new_turn()
    end,

    on_exit = function()
        tm.view(nil)
    end,

    on_update = function(dt)
        if input.key_pressed(KEY.escape) then leave() return end
        if not s.map then return end
        for i, m in ipairs { "walk", "jump", "throw" } do
            if input.key_pressed(KEY[tostring(i)]) then set_mode(m)() end
        end
        if input.key_pressed(KEY.n) then new_turn() end
        if input.key_pressed(KEY.v) then next_inside() end
        if input.key_pressed(KEY.rbracket) then cam:orbit_by(0, TILT_STEP) end
        if input.key_pressed(KEY.lbracket) then cam:orbit_by(0, -TILT_STEP) end

        local rect = camera.rect_of("board_view")
        local over = gramarye.ui.hovered("board_view")
        local mx, my = input.mouse_pos()
        local wheel = input.wheel()
        if over and wheel ~= 0 then cam:zoom_by(wheel > 0 and 1 or -1, rect, mx, my) end
        if s.orbit then
            if input.mouse_down(MOUSE_MIDDLE) then
                cam:orbit_by((mx - s.orbit.x) * ORBIT_TURN, (my - s.orbit.y) * ORBIT_TILT)
                s.orbit.x, s.orbit.y = mx, my
            else
                s.orbit = nil
                cam:snap_turn(90)
            end
        elseif over and input.mouse_pressed(MOUSE_MIDDLE) then
            s.orbit = { x = mx, y = my }
        end
        cam:ease_orbit(dt)

        s.clock = s.clock + dt
        update_anim(dt)
        if not s.anim then step_key() end
        cam:follow(s.unit.x, s.unit.y, dt)
        cam:clamp(rect)
        update_preview(over and hovered(rect, mx, my) or nil)
        if over and input.mouse_pressed(MOUSE_LEFT) then act() end
    end,

    on_draw = function()
        if s.map then
            local v = cam:view { bounds = true, bg = { 12, 12, 18, 255 } }
            local x, y, z, level = s.unit.x, s.unit.y, s.map:height_at(s.unit.x, s.unit.y, s.unit.z), s.unit.z
            local a = s.anim
            if a and not a.thrown then
                x, y, z = along(a.points, a.t)
                level = arcs.level_of(s.map, x, y, z)
            end
            local st = water.state(s.map, x, y, level, s.body)
            v.marker = { x = x, y = y, z = water.stand_z(st, z, s.map) }
            s.shown = inside.apply(v, s.map, s.inside, x, y, z, level)
            v.marks = marks()
            local r = s.preview
            if r and r.points then
                local color = s.mode == "walk" and WALK or s.mode == "throw" and (r.ok and THROW or BAD) or (r.ok and OK or BAD)
                local land = r.land or r.to
                v.arcs = { { points = r.points, color = color, width = 3 } }
                v.rings = { { x = land[1], y = land[2], z = land[3], r = 6, color = color } }
            end
            if a and a.thrown then
                local tx, ty, tz = along(a.points, a.t)
                v.rings = { { x = tx, y = ty, z = tz, r = 3, color = THROW } }
            end
            water.effects(v, st, x, y, s.clock, a ~= nil and not a.thrown)
            tm.view(s.map, v)
        else
            tm.view(nil)
        end
        gramarye.ui.render(ui.Frame {
            id = "board_root", layout = { dir = "column", w = { grow = true }, h = { grow = true } },
            ui.Row {
                bg = { 20, 20, 34, 255 }, w = { grow = true }, h = 44, pad = { h = 10 }, gap = 6, align = { y = "center" },
                ui.Label { text = "Board demo", size = 16 },
                ui.Spacer(12),
                button("bd_walk", "Walk", set_mode("walk"), s.mode == "walk"),
                button("bd_jump", "Jump", set_mode("jump"), s.mode == "jump"),
                button("bd_throw", "Throw", set_mode("throw"), s.mode == "throw"),
                ui.Spacer(12),
                ui.Label { text = ("moves %d / %d"):format(s.moves_left, s.body.move) },
                button("bd_turn", "New turn", new_turn, false),
                ui.Spacer(12),
                ui.Label { text = "Inside:", skin = "label_dim" },
                button("bd_inside", inside.LABELS[s.inside], next_inside, s.shown ~= nil),
                ui.Spacer(12),
                ui.Label { text = s.map and status() or ("map '%s' not found: %s"):format(MAP, tostring(s.err)), skin = "label_dim" },
                ui.Spacer(),
                ui.Button { id = "bd_close", label = "×", w = 32, h = 32, on_click = leave },
            },
            { _type = "custom", id = "board_view", kind = tm.VIEW_KIND, layout = { w = { grow = true }, h = { grow = true } } },
            ui.Row {
                bg = { 20, 20, 34, 255 }, w = { grow = true }, h = 28, pad = { h = 10 }, align = { y = "center" },
                ui.Label { text = "1-3 walk / jump / throw   click acts   WASD step   N new turn   V inside view   middle-drag turn / tilt   [ ] tilt   wheel zoom   Esc back",
                           skin = "hint" },
            },
        })
    end,
}
