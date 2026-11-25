local M = {}

M.DEFAULT = { move = 4, range = 3, up = 1, down = 2, peak = 10, lob = 14, swim = false, phase = false, obstacle = 12,
              height = 2, wade = 1, wade_cost = 2, swim_cost = 2 }
M.HAND = 10
M.GRAVITY = 600

function M.body(t)
    local b = {}
    for k, v in pairs(M.DEFAULT) do b[k] = v end
    for k, v in pairs(t or {}) do b[k] = v end
    return b
end

function M.cell_point(map, cx, cy, level)
    local t = map:tile()
    local x, y = (cx + 0.5) * t, (cy + 0.5) * t
    return x, y, (map:height_at(x, y, level))
end

local function cell_of(map, x, y)
    local t = map:tile()
    return math.floor(x / t), math.floor(y / t)
end

function M.level_of(map, x, y, z)
    local cx, cy = cell_of(map, x, y)
    local lh = map:elevation()
    return (map:level_at(cx, cy, math.max(0, math.ceil(z / lh - 0.01))))
end

local function aim_level(map, cx, cy, la, body)
    local l = map:level_at(cx, cy, la + body.up)
    if l < 0 then l = map:level_at(cx, cy) end
    return l
end

local function too_deep(map, cx, cy, level, body)
    local depth, _, deep = map:water_at(cx, cy, level)
    return deep or depth > (body.wade or 1)
end
M.too_deep = too_deep

local function cells_apart(ax, ay, bx, by)
    return math.sqrt((bx - ax) ^ 2 + (by - ay) ^ 2)
end

local function cut_at(points, ax, ay, bx, by, hx, hy, hz)
    local total = math.sqrt((bx - ax) ^ 2 + (by - ay) ^ 2)
    if total < 1e-6 then return points end
    local th = math.sqrt((hx - ax) ^ 2 + (hy - ay) ^ 2) / total
    local n, out = #points // 3, {}
    for i = 0, n - 1 do
        if i / (n - 1) >= th then break end
        out[#out + 1], out[#out + 2], out[#out + 3] = points[i * 3 + 1], points[i * 3 + 2], points[i * 3 + 3]
    end
    out[#out + 1], out[#out + 2], out[#out + 3] = hx, hy, hz
    return out
end

function M.jump(map, ax, ay, cx, cy, body, z, tz)
    body = body or M.DEFAULT
    local fx, fy = cell_of(map, ax, ay)
    local la = z or map:level_at(fx, fy)
    local az = map:height_at(ax, ay, la)
    local lb, _, solid, water = map:surface(cx, cy, tz or aim_level(map, cx, cy, la, body))
    local bx, by, bz = M.cell_point(map, cx, cy, lb)
    local r = { from = { ax, ay, az }, to = { bx, by, bz }, level = lb, ok = false,
                up = math.max(0, lb - la), down = math.max(0, la - lb), distance = cells_apart(fx, fy, cx, cy) }
    local pts, hx, hy, hz = map:arc(ax, ay, az, bx, by, bz, { peak = body.peak, obstacle = body.obstacle })
    r.points = pts
    if hx then r.hit = { hx, hy, hz } end
    if lb < 0 then r.why = "nothing to land on"
    elseif fx == cx and fy == cy then r.why = "already there"
    elseif r.distance > body.range + 1e-3 then r.why = "out of range"
    elseif r.up > body.up then r.why = "too high"
    elseif r.down > body.down then r.why = "too far down"
    elseif solid and not body.phase then r.why = "obstruction"
    elseif water and too_deep(map, cx, cy, lb, body) and not body.swim then r.why = "water"
    elseif r.hit then r.why = "blocked"
    else r.ok = true end
    return r
end

function M.throw(map, ax, ay, cx, cy, body, z, tz)
    body = body or M.DEFAULT
    local fx, fy = cell_of(map, ax, ay)
    local la = z or map:level_at(fx, fy)
    local az = map:height_at(ax, ay, la) + M.HAND
    local bx, by, bz = M.cell_point(map, cx, cy, tz or aim_level(map, cx, cy, la, body))
    local r = { ok = false, distance = cells_apart(fx, fy, cx, cy) }
    local pts, hx, hy, hz = map:arc(ax, ay, az, bx, by, bz, { peak = body.lob, obstacle = body.obstacle })
    if hx then
        r.points, r.land, r.why = cut_at(pts, ax, ay, bx, by, hx, hy, hz), { hx, hy, hz }, "blocked"
    else
        r.points, r.land = pts, { bx, by, bz }
    end
    if r.distance > body.range + 1e-3 then r.why = "out of range"
    elseif not hx then r.ok = true end
    return r
end

function M.fall(map, ax, ay, vx, vy, body, z)
    body = body or M.DEFAULT
    local fx, fy = cell_of(map, ax, ay)
    local la = z or map:level_at(fx, fy)
    local speed = math.sqrt(vx * vx + vy * vy)
    local r = { points = { ax, ay, (map:height_at(ax, ay, la)) }, ok = false }
    r.land = { r.points[1], r.points[2], r.points[3] }
    if speed < 1e-6 then r.why = "no edge that way" return r end
    local ux, uy = vx / speed, vy / speed
    local ex, ey, ez = ax, ay, r.points[3]
    local edge = false
    for _ = 1, map:tile() do
        local h = map:height_at(ex + ux, ey + uy, la)
        if h < ez - 2 then edge = true break end
        ex, ey, ez = ex + ux, ey + uy, h
    end
    if not edge then r.why = "no edge that way" return r end
    local pts, landed = map:trace(ex, ey, ez + 0.5, vx, vy, 0, { gravity = M.GRAVITY })
    r.points = { ax, ay, r.points[3] }
    for i = 1, #pts do r.points[3 + i] = pts[i] end
    local n = #r.points // 3
    r.land = { r.points[n * 3 - 2], r.points[n * 3 - 1], r.points[n * 3] }
    if not landed then r.why = "nowhere to land" return r end
    local lx, ly = cell_of(map, r.land[1], r.land[2])
    local lb, _, solid, water = map:surface(lx, ly, M.level_of(map, r.land[1], r.land[2], r.land[3]))
    r.level, r.drop = lb, math.max(0, la - lb)
    if r.drop > body.down then r.why = "too far down"
    elseif water and too_deep(map, lx, ly, lb, body) and not body.swim then r.why = "water"
    else r.ok = true end
    return r
end

function M.reach(map, ax, ay, body, z)
    body = body or M.DEFAULT
    local fx, fy = cell_of(map, ax, ay)
    local w, h = map:size()
    local r, out = math.floor(body.range), {}
    for cy = math.max(0, fy - r), math.min(h - 1, fy + r) do
        for cx = math.max(0, fx - r), math.min(w - 1, fx + r) do
            if (cx ~= fx or cy ~= fy) and cells_apart(fx, fy, cx, cy) <= body.range + 1e-3 then
                local j = M.jump(map, ax, ay, cx, cy, body, z)
                out[#out + 1] = { x = cx, y = cy, z = j.level, ok = j.ok, why = j.why }
            end
        end
    end
    return out
end

return M
