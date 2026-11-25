local M = {}

M.METHODS = { "off", "cutaway", "section", "see" }
M.LABELS  = { off = "Off", cutaway = "Cutaway", section = "Section", see = "See-through" }
M.RADIUS  = 140

function M.is_inside(map, x, y, level)
    if not level or level < 0 then return false end
    local t = map:tile()
    return map:covered(math.floor(x / t), math.floor(y / t), level)
end

function M.next(method)
    for i, m in ipairs(M.METHODS) do
        if m == method then return M.METHODS[i % #M.METHODS + 1] end
    end
    return M.METHODS[1]
end

function M.apply(v, map, method, x, y, z, level)
    if method == "off" or not M.is_inside(map, x, y, level) then return nil end
    local t = map:tile()
    local room = { x = math.floor(x / t), y = math.floor(y / t), level = level }
    v.room = room
    if method == "see" then
        v.see_through = { x = x, y = y, z = z, r = M.RADIUS }
    elseif method == "section" then
        v.section = true
    end
    return { method = method, level = level, room = room, x = x, y = y, z = z }
end

function M.raycast(map, cam, rect, mx, my, shown)
    local ox, oy, oz, dx, dy, dz = cam:board_ray(rect, mx, my)
    local room = shown and shown.room
    if shown and shown.method == "see" then
        local sx, sy = cam:world_to_screen(rect, shown.x, shown.y, shown.z)
        if (mx - sx) ^ 2 + (my - sy) ^ 2 > M.RADIUS ^ 2 then room = nil end
    end
    return map:raycast(ox, oy, oz, dx, dy, dz, nil, room)
end

return M
