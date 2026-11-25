local M = {}

function M.walker(body, z, to_z)
    body = body or {}
    return { water = body.swim or false, solid = body.phase or false, height = body.height or 1,
             wade = body.wade or 1, wade_cost = body.wade_cost or 2, swim_cost = body.swim_cost or 2,
             level = z, to_level = to_z }
end

local function budget(body, left)
    return math.max(0, math.min(left or (body and body.move) or 4, 32))
end

function M.key(x, y, z) return x .. "," .. y .. "," .. z end

function M.range(map, cx, cy, body, left, z)
    local all = map:reach(cx, cy, budget(body, left), M.walker(body, z))
    local cells, by = {}, {}
    for i = 2, #all do
        local c = all[i]
        cells[#cells + 1] = c
        by[M.key(c.x, c.y, c.z)] = c
    end
    return cells, by
end

function M.path(map, cx, cy, tx, ty, body, left, z, tz)
    return map:path(cx, cy, tx, ty, budget(body, left), M.walker(body, z, tz))
end

function M.cost(map, cells, body)
    local total = 0
    for i = 2, #cells do
        local p, c = cells[i - 1], cells[i]
        local ok, _, _, cost = map:can_step(p.x, p.y, c.x, c.y, M.walker(body, p.z, c.z))
        total = total + (ok and cost or 1)
    end
    return total
end

function M.points(map, cells)
    local t, out = map:tile(), {}
    local function add(x, y, level)
        out[#out + 1], out[#out + 2], out[#out + 3] = x, y, (map:height_at(x, y, level))
    end
    for i, c in ipairs(cells) do
        local x, y = (c.x + 0.5) * t, (c.y + 0.5) * t
        if i > 1 then
            local p = cells[i - 1]
            local px, py = (p.x + 0.5) * t, (p.y + 0.5) * t
            add((px + x) * 0.5 + (x - px) * 0.01, (py + y) * 0.5 + (y - py) * 0.01, c.z)
        end
        add(x, y, c.z)
    end
    return out
end

return M
