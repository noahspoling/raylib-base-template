local ui      = gramarye.require("lib/ui")
local edui    = gramarye.require("lib/editor_ui")
local camera  = gramarye.require("lib/camera")
local arcs    = gramarye.require("lib/arcs")
local moves   = gramarye.require("lib/moves")
local inside  = gramarye.require("lib/inside")
local water   = gramarye.require("lib/water")
local stress  = gramarye.require("lib/stress_map")
local session = gramarye.require("lib/tilemap_session")
local tm      = gramarye.tilemap
local input   = gramarye.input

local MOUSE_LEFT, MOUSE_MIDDLE = 0, 2
local ORBIT_TURN = 0.4
local ORBIT_TILT = 0.3
local STEP_TIME  = 0.16
local PAN_SPEED  = 600
local TILT_STEP  = 5
local DROP_SPEED = 70
local VIEW_BG    = { 12, 12, 18, 255 }

local OK_COLOR, BAD_COLOR = { 120, 235, 130, 255 }, { 240, 95, 85, 255 }
local WALK_COLOR = { 120, 200, 255, 255 }
local THROW_COLOR, DROP_COLOR = { 255, 215, 110, 255 }, { 120, 190, 255, 255 }

local KEY = {}
for _, k in ipairs { "escape", "f", "w", "a", "s", "d", "up", "down", "left", "right", "minus", "equal",
                     "lbracket", "rbracket", "r", "p", "n", "v", "g", "1", "2", "3", "4" } do
    KEY[k] = input.key(k)
end

local MODES = { { "walk", "Walk" }, { "jump", "Jump" }, { "throw", "Throw" }, { "drop", "Drop" } }

local cam = camera.new { zoom_index = 8, smoothing = 8 }

local view = {
    map      = nil,
    label    = "",
    follow   = true,
    player   = { x = 0, y = 0, z = 0 },
    maps     = {},
    names    = {},
    selected = nil,
    drag     = nil,
    status   = "",
    orbit    = nil,
    inside   = "cutaway",
    shown    = nil,
    clock    = 0,
    god      = false,
    pois     = {},
    poi_names = {},
    poi      = nil,
    fps      = 60,

    mode     = "walk",
    body     = arcs.body(),
    moves_left = 0,
    blocked  = nil,
    target   = nil,
    preview  = nil,
    reach    = nil,
    anim     = nil,
}

local function clamp(v, lo, hi) return math.max(lo, math.min(v, hi)) end

local function player_cell()
    local t = view.map:tile()
    return math.floor(view.player.x / t), math.floor(view.player.y / t)
end

local function place(cx, cy, z)
    local t = view.map:tile()
    view.player.x, view.player.y = (cx + 0.5) * t, (cy + 0.5) * t
    view.player.z = z or math.max(0, (view.map:level_at(cx, cy)))
end

local function new_turn()
    view.moves_left = view.body.move
    view.reach = nil
end

local function show(m, label, pois)
    view.map, view.label = m, label
    view.pois, view.poi_names, view.poi = pois or {}, {}, nil
    for i, p in ipairs(view.pois) do view.poi_names[i] = p.name end
    local _, tilt = m:elevation()
    cam.tilt = tilt
    local w, h = m:size()
    local t = m:tile()
    place(w // 2, h // 2)
    cam.x, cam.y = view.player.x, view.player.y
    cam.bounds = { 0, 0, w * t, h * t }
    view.reach, view.anim, view.preview = nil, nil, nil
    new_turn()
end

local function load_index(i)
    local entry = view.maps[i]
    if not entry then return end
    local m, pkg = tm.load(entry.name)
    if not m then
        view.status = "load failed: " .. tostring(pkg)
        return
    end
    view.selected = i
    show(m, ("%s (%s)"):format(entry.name, edui.short_id(pkg)))
end

local function show_editor_map()
    if not session.map then return end
    view.selected = nil
    show(session.map, "editor: " .. (session.name or "untitled") .. (session.map:dirty() and " *" or ""))
end

local function toggle_follow()
    view.follow = not view.follow
    if view.follow then
        view.player.x, view.player.y = cam.x, cam.y
        place(player_cell())
    end
end

local function teleport(cx, cy, z)
    place(cx, cy, z)
    cam.x, cam.y = view.player.x, view.player.y
    view.anim, view.reach, view.blocked = nil, nil, nil
end

local function goto_poi(i)
    local p = view.pois[i]
    if not p then return end
    view.poi = i
    if not view.follow then view.follow = true end
    teleport(p.x, p.y, p.level)
    view.status = p.name
end

local function toggle_god()
    view.god = not view.god
    view.status = view.god and "god mode: click to teleport, walking is free" or ""
end

local STRESS_NAMES = {}
for i, n in ipairs(stress.SIZES) do STRESS_NAMES[i] = ("%dx%d"):format(n, n) end

local function make_stress(i)
    view.job, view.job_t = stress.job(stress.SIZES[i]), 0
end

local function step_stress(dt)
    local job = view.job
    view.job_t = view.job_t + dt
    if not job:step(0.03) then return end
    view.job = nil
    view.selected = nil
    show(job.map, ("stress %dx%d"):format(job.size, job.size), job.pois)
    view.status = ("built in %.1f s, %d points of interest"):format(view.job_t, #job.pois)
    goto_poi(1)
end

local LIMITS = { move = { 1, 12 }, range = { 1, 8 }, up = { 0, 7 }, down = { 0, 7 }, peak = { 2, 48 }, height = { 1, 6 } }
local STEPS  = { move = 1, range = 1, up = 1, down = 1, peak = 2, height = 1 }
local adjust_fns = {}

local function adjust(field, sign)
    local key = field .. sign
    if not adjust_fns[key] then
        adjust_fns[key] = function()
            local lim = LIMITS[field]
            view.body[field] = clamp(view.body[field] + sign * STEPS[field], lim[1], lim[2])
            if field == "peak" then view.body.lob = view.body.peak + 4 end
            if field == "move" then view.moves_left = view.body.move end
            view.reach = nil
        end
    end
    return adjust_fns[key]
end

local mode_fns = {}
local function set_mode(m)
    if not mode_fns[m] then mode_fns[m] = function() view.mode = m end end
    return mode_fns[m]
end
local function toggle_swim()  view.body.swim = not view.body.swim; view.reach = nil end
local function toggle_phase() view.body.phase = not view.body.phase; view.reach = nil end
local function next_inside()  view.inside = inside.next(view.inside) end

local function step_key()
    local dx = ((input.key_pressed(KEY.right) or input.key_pressed(KEY.d)) and 1 or 0)
             - ((input.key_pressed(KEY.left)  or input.key_pressed(KEY.a)) and 1 or 0)
    local dy = ((input.key_pressed(KEY.down)  or input.key_pressed(KEY.s)) and 1 or 0)
             - ((input.key_pressed(KEY.up)    or input.key_pressed(KEY.w)) and 1 or 0)
    if dx == 0 and dy == 0 then return nil end
    local bx, by = cam:unrotate(dx, dy)
    if math.abs(bx) >= math.abs(by) then return bx > 0 and 1 or -1, 0 end
    return 0, by > 0 and 1 or -1
end

local function walk(path)
    local steps = #path - 1
    if steps < 1 then return end
    if not view.god then view.moves_left = view.moves_left - moves.cost(view.map, path, view.body) end
    view.anim = { points = moves.points(view.map, path), t = 0, dur = steps * STEP_TIME, kind = "walk",
                  level = path[#path].z }
    view.reach = nil
end

local function update_follow(dt)
    if view.anim == nil then
        local sx, sy = step_key()
        if sx then
            local cx, cy = player_cell()
            local z = view.player.z
            local ok, why, level, cost = view.map:can_step(cx, cy, cx + sx, cy + sy, moves.walker(view.body, z))
            if ok and not view.god and cost > view.moves_left then
                view.blocked = view.moves_left < 1 and "no moves left (N: new turn)" or "not enough moves left"
            elseif not ok then
                view.blocked = why
            else
                view.blocked = nil
                walk { { x = cx, y = cy, z = z }, { x = cx + sx, y = cy + sy, z = level } }
            end
        end
    end
    cam:follow(view.player.x, view.player.y, dt)
end

local function move_axis()
    local dx = ((input.key_down(KEY.right) or input.key_down(KEY.d)) and 1 or 0)
             - ((input.key_down(KEY.left)  or input.key_down(KEY.a)) and 1 or 0)
    local dy = ((input.key_down(KEY.down)  or input.key_down(KEY.s)) and 1 or 0)
             - ((input.key_down(KEY.up)    or input.key_down(KEY.w)) and 1 or 0)
    return dx, dy
end

local function update_free(dt, rect, over)
    local dx, dy = move_axis()
    if dx ~= 0 or dy ~= 0 then cam:pan_screen(-dx * PAN_SPEED * dt, -dy * PAN_SPEED * dt) end
    local mx, my = input.mouse_pos()
    if view.drag then
        if input.mouse_down(view.drag.button) then
            cam:pan_screen(mx - view.drag.x, my - view.drag.y, rect, view.drag.x, view.drag.y)
            view.drag.x, view.drag.y = mx, my
        else
            view.drag = nil
        end
    elseif over then
        if input.mouse_pressed(MOUSE_LEFT) then view.drag = { button = MOUSE_LEFT, x = mx, y = my } end
    end
end

local function update_orbit(over, dt)
    local mx, my = input.mouse_pos()
    if view.orbit then
        if input.mouse_down(MOUSE_MIDDLE) then
            cam:orbit_by((mx - view.orbit.x) * ORBIT_TURN, (my - view.orbit.y) * ORBIT_TILT)
            view.orbit.x, view.orbit.y = mx, my
        else
            view.orbit = nil
        end
    elseif over and input.mouse_pressed(MOUSE_MIDDLE) then
        view.orbit = { x = mx, y = my }
    end
    cam:ease_orbit(dt)
end

local function hovered(rect, mx, my)
    local hx, hy, hz = inside.raycast(view.map, cam, rect, mx, my, view.shown)
    if not hx then return nil end
    local t = view.map:tile()
    return { x = math.floor(hx / t), y = math.floor(hy / t), z = arcs.level_of(view.map, hx, hy, hz),
             px = hx, py = hy, pz = hz }
end

local function update_preview(rect, mx, my, over)
    view.target, view.preview = nil, nil
    if not view.follow or view.anim or not over then return end
    local h = hovered(rect, mx, my)
    if not h then return end
    view.target = h
    local p, m = view.player, view.map
    if view.mode == "walk" then
        local cx, cy = player_cell()
        if h.x == cx and h.y == cy and h.z == p.z then return end
        local path = moves.path(m, cx, cy, h.x, h.y, view.body, view.god and 32 or view.moves_left, p.z, h.z)
        local r = { ok = path ~= nil, path = path }
        if path then
            r.points, r.steps = moves.points(m, path), #path - 1
            r.cost = moves.cost(m, path, view.body)
            local last = path[#path]
            r.to = { arcs.cell_point(m, last.x, last.y, last.z) }
        else
            r.why = view.moves_left < 1 and "no moves left" or "can't get there this turn"
        end
        view.preview = r
    elseif view.mode == "jump" then
        view.preview = arcs.jump(m, p.x, p.y, h.x, h.y, view.body, p.z, h.z)
    elseif view.mode == "throw" then
        view.preview = arcs.throw(m, p.x, p.y, h.x, h.y, view.body, p.z, h.z)
    else
        local dx, dy = h.px - p.x, h.py - p.y
        local len = math.sqrt(dx * dx + dy * dy)
        if len > 1e-3 then
            view.preview = arcs.fall(m, p.x, p.y, dx / len * DROP_SPEED, dy / len * DROP_SPEED, view.body, p.z)
        end
    end
end

local function act()
    if view.god and view.target then
        local h = view.target
        teleport(h.x, h.y, h.z)
        return
    end
    local r = view.preview
    if not r then return end
    if view.mode == "walk" and r.ok then
        walk(r.path)
    elseif view.mode == "jump" and r.ok then
        view.anim = { points = r.points, t = 0, dur = 0.35 + 0.06 * r.distance, kind = "jump", level = r.level }
    elseif view.mode == "throw" then
        view.anim = { points = r.points, t = 0, dur = 0.5 + 0.05 * r.distance, kind = "throw" }
    elseif view.mode == "drop" and r.ok then
        view.anim = { points = r.points, t = 0, dur = (#r.points // 3) / 60, kind = "fall", level = r.level }
    end
end

local function along(points, u)
    local n = #points // 3
    local f = clamp(u, 0, 1) * (n - 1)
    local i = math.min(math.floor(f), n - 2)
    local k = f - i
    local a, b = i * 3, (i + 1) * 3
    return points[a + 1] + (points[b + 1] - points[a + 1]) * k,
           points[a + 2] + (points[b + 2] - points[a + 2]) * k,
           points[a + 3] + (points[b + 3] - points[a + 3]) * k
end

local function update_anim(dt)
    local a = view.anim
    if not a then return end
    a.t = a.t + dt / math.max(a.dur, 1e-3)
    if a.t >= 1 then
        if a.kind ~= "throw" then
            local n, t = #a.points // 3, view.map:tile()
            place(math.floor(a.points[n * 3 - 2] / t), math.floor(a.points[n * 3 - 1] / t), a.level)
        end
        view.anim, view.reach = nil, nil
    end
end

local function reach_marks()
    if (view.mode ~= "jump" and view.mode ~= "walk") or not view.follow or view.anim then return nil end
    local cx, cy = player_cell()
    local b, z = view.body, view.player.z
    local key = ("%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s,%s"):format(view.mode, cx, cy, z, view.moves_left, b.move,
                    b.range, b.up, b.down, b.peak, b.height, tostring(b.swim), tostring(b.phase))
    if not view.reach or view.reach.key ~= key then
        local marks = {}
        if view.mode == "walk" then
            for _, c in ipairs((moves.range(view.map, cx, cy, b, view.moves_left, z))) do
                marks[#marks + 1] = { x = c.x, y = c.y, level = c.z, color = { 110, 190, 255, 55 } }
            end
        else
            for _, c in ipairs(arcs.reach(view.map, view.player.x, view.player.y, b, z)) do
                if c.ok then marks[#marks + 1] = { x = c.x, y = c.y, level = c.z, color = { 110, 230, 130, 60 } } end
            end
        end
        view.reach = { key = key, marks = marks }
    end
    return view.reach.marks
end

local function top_bar()
    local row = ui.Row {
        id = "view_bar", bg = edui.BAR_BG,
        w = { grow = true }, h = 48, pad = { h = 8 }, gap = 8,
        align = { y = "center" },
        ui.Label { text = "Map viewer", size = 16 },
        ui.Dropdown { id = "view_maps", items = view.names, selected = view.selected, w = 260,
                      placeholder = #view.names > 0 and "saved maps..." or "no saved maps",
                      on_select = load_index },
    }
    if session.map then
        row[#row + 1] = edui.button("view_editor_map", "Editor's map", show_editor_map, view.selected == nil and view.map ~= nil)
    end
    row[#row + 1] = edui.button("view_follow", "Follow", toggle_follow, view.follow)
    row[#row + 1] = edui.button("view_free", "Free", toggle_follow, not view.follow)
    row[#row + 1] = ui.Spacer(8)
    row[#row + 1] = ui.Label { text = "Inside:", skin = "label_dim" }
    row[#row + 1] = edui.button("view_inside", inside.LABELS[view.inside], next_inside, view.shown ~= nil)
    row[#row + 1] = ui.Spacer()
    row[#row + 1] = ui.Label { text = ("%s   %gx   tilt %d   turned %d   %s   %s"):format(view.label, cam.zoom,
                                   math.floor(cam.tilt + 0.5), math.floor(cam.rotation + 0.5) % 360,
                                   cam.fov > 0 and "perspective" or "flat", view.status),
                               skin = "label_dim" }
    row[#row + 1] = ui.Button { id = "view_close", label = "×", w = 32, h = 32, on_click = edui.leave }
    return row
end

local function stepper(id, label, field)
    return ui.Row {
        gap = 2, align = { y = "center" },
        ui.Label { text = label:format(view.body[field]), skin = "label_dim" },
        edui.button("b_" .. id .. "_dn", "-", adjust(field, -1), false, 26),
        edui.button("b_" .. id .. "_up", "+", adjust(field, 1), false, 26),
    }
end

local function body_bar()
    local row = ui.Row {
        id = "view_body", bg = { 26, 26, 40, 255 },
        w = { grow = true }, h = 38, pad = { h = 8 }, gap = 6,
        align = { y = "center" },
    }
    for _, m in ipairs(MODES) do
        row[#row + 1] = edui.button("mode_" .. m[1], m[2], set_mode(m[1]), view.mode == m[1])
    end
    row[#row + 1] = ui.Spacer(10)
    row[#row + 1] = stepper("move", "move %d", "move")
    row[#row + 1] = stepper("range", "range %d", "range")
    row[#row + 1] = stepper("up", "up %d", "up")
    row[#row + 1] = stepper("down", "down %d", "down")
    row[#row + 1] = stepper("peak", "peak %d px", "peak")
    row[#row + 1] = stepper("height", "height %d", "height")
    row[#row + 1] = edui.button("b_swim", "Swim", toggle_swim, view.body.swim)
    row[#row + 1] = edui.button("b_phase", "Phase", toggle_phase, view.body.phase)
    row[#row + 1] = ui.Spacer(10)
    row[#row + 1] = ui.Label { text = ("moves %d / %d"):format(view.moves_left, view.body.move) }
    row[#row + 1] = edui.button("b_turn", "New turn", new_turn)
    row[#row + 1] = ui.Spacer()
    local m, p = view.map, view.player
    local info = ""
    if m then
        local st = water.state(m, p.x, p.y, p.z, view.body)
        info = ("level %d  height %.1f%s%s"):format(p.z, (m:height_at(p.x, p.y, p.z)),
                                                    inside.is_inside(m, p.x, p.y, p.z) and "  inside" or "",
                                                    st.mode ~= "dry" and ("  %s (%d deep)"):format(st.mode, st.depth) or "")
        local r = view.preview
        if r then
            local verdict = r.ok and "ok" or (r.why or "no")
            if view.mode == "walk" then
                info = info .. (r.ok and ("   walk: %d step%s, cost %d"):format(r.steps, r.steps == 1 and "" or "s", r.cost)
                                      or ("   walk: " .. verdict))
            elseif view.mode == "jump" then
                info = info .. ("   jump: %s  (%.1f cells, up %d, down %d)"):format(verdict, r.distance, r.up, r.down)
            elseif view.mode == "throw" then
                info = info .. ("   throw: %s  (%.1f cells)"):format(verdict, r.distance)
            else
                info = info .. ("   drop: %s%s"):format(verdict, r.drop and ("  (%d levels)"):format(r.drop) or "")
            end
        elseif view.blocked then
            info = info .. "   blocked: " .. view.blocked
        end
    end
    row[#row + 1] = ui.Label { text = info, skin = "label_dim" }
    return row
end

local function test_bar()
    local row = ui.Row {
        id = "view_test", bg = { 22, 22, 34, 255 },
        w = { grow = true }, h = 36, pad = { h = 8 }, gap = 6,
        align = { y = "center" },
        ui.Label { text = "Stress test:", skin = "label_dim" },
        ui.Dropdown { id = "view_stress", items = STRESS_NAMES, selected = nil, w = 130,
                      placeholder = "generate...", on_select = make_stress },
        edui.button("view_god", "God (G)", toggle_god, view.god),
    }
    if #view.pois > 0 then
        row[#row + 1] = ui.Dropdown { id = "view_pois", items = view.poi_names, selected = view.poi, w = 260,
                                      placeholder = "teleport to...", on_select = goto_poi }
    end
    row[#row + 1] = ui.Spacer()
    local st = view.map and view.map:mesh_stats()
    if view.job then
        row[#row + 1] = ui.Label { text = ("generating %dx%d... %d%%"):format(view.job.size, view.job.size,
                                       math.floor(view.job.progress * 100 + 0.5)), skin = "label_dim" }
    elseif st then
        row[#row + 1] = ui.Label { text = ("%3d fps   chunks %d/%d%s   %.2fM vertices   build %.0f ms"):format(
                                       math.floor(view.fps + 0.5), st.built, st.chunks,
                                       st.pending > 0 and ("  (%d to build)"):format(st.pending) or "",
                                       st.vertices / 1e6, st.build_ms),
                                   skin = "label_dim" }
    end
    return row
end

local function overlays(v)
    local r = view.preview
    if r and r.points then
        local color = view.mode == "walk" and WALK_COLOR
                   or view.mode == "throw" and (r.ok and THROW_COLOR or BAD_COLOR)
                   or view.mode == "drop" and (r.ok and DROP_COLOR or BAD_COLOR)
                   or (r.ok and OK_COLOR or BAD_COLOR)
        local pts = r.points
        if view.mode == "walk" then
            pts = {}
            for i = 1, #r.points, 3 do
                pts[i], pts[i + 1], pts[i + 2] = r.points[i], r.points[i + 1], r.points[i + 2] + 1.5
            end
        end
        v.arcs = { { points = pts, color = color, width = 3 } }
        local land = r.land or r.to
        v.rings = { { x = land[1], y = land[2], z = land[3], r = 6, color = color } }
        if r.hit then v.rings[2] = { x = r.hit[1], y = r.hit[2], z = r.hit[3], r = 4, color = BAD_COLOR } end
    end
    local a = view.anim
    if a and a.kind == "throw" then
        local x, y, z = along(a.points, a.t)
        v.rings = { { x = x, y = y, z = z, r = 3, color = THROW_COLOR } }
        v.arcs = { { points = a.points, color = { 255, 215, 110, 90 }, width = 2 } }
    end
    v.marks = reach_marks()
    return v
end

return {
    on_enter = function()
        view.maps, view.names = tm.list(), {}
        for i, e in ipairs(view.maps) do view.names[i] = e.name end
        if session.map then
            show_editor_map()
        elseif #view.maps > 0 then
            load_index(1)
        end
    end,

    on_exit = function()
        tm.view(nil)
    end,

    on_update = function(dt)
        if input.key_pressed(KEY.escape) then edui.leave() return end
        if view.job then step_stress(dt) end
        if not view.map then return end
        if input.key_pressed(KEY.f) then toggle_follow() end
        if input.key_pressed(KEY.n) then new_turn() end
        if input.key_pressed(KEY.v) then next_inside() end
        if input.key_pressed(KEY.g) then toggle_god() end
        view.clock = view.clock + dt
        if dt > 0 then view.fps = view.fps + (1 / dt - view.fps) * math.min(1, dt * 3) end
        for i, m in ipairs(MODES) do
            if input.key_pressed(KEY[tostring(i)]) then view.mode = m[1] end
        end
        local dtilt = (input.key_pressed(KEY.rbracket) and TILT_STEP or 0) - (input.key_pressed(KEY.lbracket) and TILT_STEP or 0)
        if input.key_pressed(KEY.r) then cam:orbit_to(0, select(2, view.map:elevation())) end
        if input.key_pressed(KEY.p) then cam.fov = cam.fov > 0 and 0 or camera.FOV end
        if dtilt ~= 0 then cam:orbit_by(0, dtilt) end

        local rect = camera.rect_of("view_viewport")
        local over = gramarye.ui.hovered("view_viewport")
        local mx, my = input.mouse_pos()
        local wheel = input.wheel()
        if over and wheel ~= 0 then cam:zoom_by(wheel > 0 and 1 or -1, rect, mx, my) end
        if input.key_pressed(KEY.minus) then cam:zoom_by(-1) end
        if input.key_pressed(KEY.equal) then cam:zoom_by(1) end

        update_orbit(over, dt)
        update_anim(dt)
        if view.follow then update_follow(dt) else update_free(dt, rect, over) end
        cam:clamp(rect)
        update_preview(rect, mx, my, over)
        if over and view.follow and input.mouse_pressed(MOUSE_LEFT) then act() end
    end,

    on_draw = function()
        if view.map then
            local v = cam:view { bounds = true, bg = VIEW_BG, supersample = 2 }
            view.shown = nil
            if view.follow then
                local p, a = view.player, view.anim
                local x, y, z, level = p.x, p.y, view.map:height_at(p.x, p.y, p.z), p.z
                if a and a.kind ~= "throw" then
                    x, y, z = along(a.points, a.t)
                    level = arcs.level_of(view.map, x, y, z)
                end
                local st = water.state(view.map, x, y, level, view.body)
                v.marker = { x = x, y = y, z = water.stand_z(st, z, view.map) }
                view.shown = inside.apply(v, view.map, view.inside, x, y, z, level)
                overlays(v)
                water.effects(v, st, x, y, view.clock, a ~= nil and a.kind == "walk")
            end
            tm.view(view.map, v)
        else
            tm.view(nil)
        end
        local body = { _type = "custom", id = "view_viewport", kind = tm.VIEW_KIND,
                       layout = { w = { grow = true }, h = { grow = true } } }
        gramarye.ui.render(ui.Frame {
            id     = "view_root",
            layout = { dir = "column", w = { grow = true }, h = { grow = true } },
            top_bar(),
            body_bar(),
            test_bar(),
            body,
            ui.Row {
                bg = edui.BAR_BG, w = { grow = true }, h = 28, pad = { h = 10 }, align = { y = "center" },
                ui.Label { text = view.map
                               and "1-4 walk / jump / throw / drop   click acts   WASD step   N new turn   V inside view   G god mode   middle-drag turn / tilt   wheel zoom   [ ] tilt   R reset   P perspective   F follow / free   Esc back"
                               or  "No map to show. Save one in the tilemap editor (maps/<name>.lua).",
                           skin = "hint" },
            },
        })
    end,
}
