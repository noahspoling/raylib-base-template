local TERRAINS = gramarye.require("lib/terrains")
local tm = gramarye.tilemap

local M = {}

M.LEVELS = 16
M.SITE = 48
M.RIVER_LEVEL = 5
M.SIZES = { 128, 256, 512, 1024 }

local function hash(x, y)
    local h = (x * 374761393 + y * 668265263) & 0xffffffff
    h = ((h ~ (h >> 13)) * 1274126177) & 0xffffffff
    return (h & 0xffff) / 0xffff
end

local function vnoise(x, y)
    local ix, iy = math.floor(x), math.floor(y)
    local fx, fy = x - ix, y - iy
    fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    local a, b = hash(ix, iy), hash(ix + 1, iy)
    local c, d = hash(ix, iy + 1), hash(ix + 1, iy + 1)
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy
end

local function noise(x, y)
    return vnoise(x / 23, y / 23) * 0.7 + vnoise(x / 7 + 31, y / 7 + 17) * 0.3
end

local function generate(size, opts, yield)
    size = size or 256
    opts = opts or {}
    local L = math.max(M.LEVELS, math.min(opts.levels or M.LEVELS, 32))
    local layers = {}
    for k = 0, L - 1 do layers[#layers + 1] = { name = "level " .. k, kind = "terrain", elevation = k } end
    layers[#layers + 1] = "detail"
    layers[#layers + 1] = "objects"
    local map = tm.new(size, size, { tile = opts.tile or 16, levels = L, layers = layers })
    map:set_elevation(10, 45, 24)
    local slot = {}
    for _, def in ipairs(TERRAINS) do slot[def.id] = map:add_terrain(def) end
    local GRASS, DIRT, ROCK, SAND = slot.grass, slot.dirt, slot.rock, slot.sand
    local BED, KELP, RIVER, LAKE, STAIRS = slot.riverbed, slot.kelp, slot.river, slot.lake, slot.stairs

    local function inside(x, y) return x >= 0 and y >= 0 and x < size and y < size end
    local function put(x, y, k, v) if k >= 0 and k < L and inside(x, y) then map:set(k + 1, x, y, v or 0) end end
    local function rect(x0, y0, x1, y1, k, v)
        if k < 0 or k >= L then return end
        x0, y0, x1, y1 = math.max(0, x0), math.max(0, y0), math.min(size - 1, x1), math.min(size - 1, y1)
        if x1 >= x0 and y1 >= y0 then map:fill(k + 1, x0, y0, x1 - x0 + 1, y1 - y0 + 1, v or 0) end
    end
    local function columns(x0, y0, x1, y1, h, top, under)
        for k = 0, L - 1 do
            local v = nil
            if k == h then v = top elseif k < h then v = under or ROCK end
            rect(x0, y0, x1, y1, k, v)
        end
    end
    local function column(x, y, h, top, under) columns(x, y, x, y, h, top, under) end
    local function H(x, y) return math.max(0, (map:level_at(x, y))) end
    local function pool(x, y, bed, surface, bed_slot, water_slot)
        if not inside(x, y) then return end
        column(x, y, bed, bed_slot or BED, ROCK)
        for k = bed + 1, surface do put(x, y, k, water_slot or LAKE) end
    end

    local h, top = {}, {}
    for y = 0, size - 1 do
        local hi = 0
        for x = 0, size - 1 do
            h[x] = 4 + math.floor(noise(x, y) * 4)
            top[x] = (noise(x + 500, y) > 0.72) and DIRT or GRASS
            if h[x] > hi then hi = h[x] end
        end
        yield(0.5 * y / size)
        for k = 0, hi do
            local x = 0
            while x < size do
                local v = h[x] == k and top[x] or (h[x] > k and DIRT or 0)
                local e = x
                while e + 1 < size do
                    local nx = e + 1
                    local w = h[nx] == k and top[nx] or (h[nx] > k and DIRT or 0)
                    if w ~= v then break end
                    e = nx
                end
                if v ~= 0 then map:fill(k + 1, x, y, e - x + 1, 1, v) end
                x = e + 1
            end
        end
    end

    local pois = {}
    local function poi(name, x, y, level)
        x, y = math.max(0, math.min(size - 1, x)), math.max(0, math.min(size - 1, y))
        pois[#pois + 1] = { name = name, x = x, y = y, level = level }
    end

    local examples = {}

    function examples.cave(cx, cy, n)
        local b = 5
        local x0, y0 = cx - 14, cy - 11
        columns(x0 - 3, y0 - 3, x0 + 31, y0 + 27, b, GRASS, DIRT)
        columns(x0, y0, x0 + 28, y0 + 22, b + 8, GRASS, ROCK)
        columns(x0 + 3, y0 + 3, x0 + 25, y0 + 19, b + 9, GRASS, ROCK)
        local rx0, ry0, rx1, ry1, mid = x0 + 4, y0 + 5, x0 + 24, y0 + 16, x0 + 14
        for k = b + 1, b + 5 do rect(rx0, ry0, mid - 1, ry1, k, nil) end
        rect(rx0, ry0, mid - 1, ry1, b, DIRT)
        for k = b + 4, b + 5 do rect(mid, ry0, rx1, ry1, k, nil) end
        for k = b + 1, b + 3 do rect(mid, cy - 1, mid, cy, k, STAIRS) end
        rect(rx0 + 2, ry1 - 4, rx0 + 6, ry1 - 1, b - 1, BED)
        rect(rx0 + 2, ry1 - 4, rx0 + 6, ry1 - 1, b, LAKE)
        for k = b + 1, b + 3 do rect(cx - 6, ry1 + 1, cx - 4, y0 + 22, k, nil) end
        rect(cx - 6, ry1 + 1, cx - 4, y0 + 22, b, DIRT)
        poi(("Cave %d: floor"):format(n), cx - 6, cy, b)
        poi(("Cave %d: ledge"):format(n), cx + 4, cy, b + 3)
        poi(("Cave %d: pool"):format(n), rx0 + 4, ry1 - 2, b - 1)
        poi(("Cave %d: outside"):format(n), cx - 5, y0 + 25)
    end

    function examples.river(cx, cy, n)
        local half = M.SITE // 2
        for x = cx - half, cx + half - 1 do
            local yc = cy + math.floor(5 * math.sin(x / 7))
            for y = yc - 1, yc do
                if inside(x, y) then
                    local hh = H(x, y)
                    put(x, y, hh, RIVER)
                    put(x, y, hh - 1, (hash(x, y) > 0.7) and KELP or BED)
                end
            end
        end
        poi(("River %d: in the water"):format(n), cx, cy + math.floor(5 * math.sin(cx / 7)))
        poi(("River %d: bank"):format(n), cx + 3, cy + math.floor(5 * math.sin((cx + 3) / 7)) + 3)
    end

    function examples.lake(cx, cy, n)
        local s = 6
        columns(cx - 13, cy - 13, cx + 13, cy + 13, s, GRASS, DIRT)
        for y = cy - 12, cy + 12 do
            for x = cx - 12, cx + 12 do
                local r = math.sqrt((x - cx) ^ 2 + (y - cy) ^ 2) + (noise(x * 3, y * 3) - 0.5) * 3
                if r < 11 then
                    local depth = r < 4 and 3 or r < 7 and 2 or 1
                    local kelp = depth >= 2 and noise(x * 2 + 9, y * 2) > 0.5
                    pool(x, y, s - depth, s, kelp and KELP or BED, LAKE)
                elseif r < 12.5 then
                    put(x, y, s, SAND)
                end
            end
        end
        poi(("Lake %d: shore (wade)"):format(n), cx + 9, cy)
        poi(("Lake %d: deep middle"):format(n), cx, cy)
    end

    function examples.plateau(cx, cy, n)
        local b = H(cx, cy)
        columns(cx - 12, cy - 10, cx + 12, cy + 10, b, GRASS, DIRT)
        columns(cx - 10, cy - 8, cx + 10, cy + 6, b + 3, GRASS, DIRT)
        columns(cx - 8, cy - 7, cx, cy - 1, b + 5, GRASS, DIRT)
        for k = b + 1, b + 3 do rect(cx + 2, cy + 6, cx + 3, cy + 6, k, STAIRS) end
        for k = b + 4, b + 5 do rect(cx - 3, cy - 1, cx - 2, cy - 1, k, STAIRS) end
        poi(("Plateau %d: foot of the stairs"):format(n), cx + 2, cy + 8)
        poi(("Plateau %d: terrace"):format(n), cx - 4, cy - 4)
    end

    function examples.falls(cx, cy, n)
        columns(cx - 13, cy - 6, cx + 12, cy + 6, 5, GRASS, DIRT)
        local tiers = { { cx - 12, 9 }, { cx - 4, 7 }, { cx + 4, 5 } }
        for _, t in ipairs(tiers) do
            columns(t[1], cy - 6, t[1] + 7, cy + 6, t[2], GRASS, ROCK)
            rect(t[1], cy - 1, t[1] + 7, cy + 1, t[2], RIVER)
            rect(t[1], cy - 1, t[1] + 7, cy + 1, t[2] - 1, BED)
        end
        columns(cx + 12, cy - 2, cx + 18, cy + 2, 3, KELP, ROCK)
        for k = 4, 5 do rect(cx + 12, cy - 2, cx + 18, cy + 2, k, LAKE) end
        poi(("Falls %d: top"):format(n), cx - 8, cy + 3)
        poi(("Falls %d: bottom"):format(n), cx + 14, cy + 4)
    end

    local order = { "cave", "river", "lake", "plateau", "falls" }
    local count = {}
    local i = 0
    local sites = size // M.SITE
    for sy = 0, sites - 1 do
        for sx = 0, sites - 1 do
            local cx, cy = sx * M.SITE + M.SITE // 2, sy * M.SITE + M.SITE // 2
            if math.abs(cy - size // 2) > M.SITE // 2 then
                local kind = order[i % #order + 1]
                i = i + 1
                count[kind] = (count[kind] or 0) + 1
                examples[kind](cx, cy, count[kind])
                yield(0.5 + 0.4 * (sy * sites + sx) / (sites * sites))
            end
        end
    end

    local R = M.RIVER_LEVEL
    for x = 0, size - 1 do
        local yc = size // 2 + math.floor(6 * math.sin(x / 23))
        columns(x, yc - 6, x, yc + 6, R, GRASS, DIRT)
        rect(x, yc - 3, x, yc - 3, R, SAND)
        rect(x, yc + 3, x, yc + 3, R, SAND)
        for y = yc - 2, yc + 2 do pool(x, y, R - 3, R, noise(x * 2, y * 2) > 0.55 and KELP or BED, RIVER) end
        if x % 64 == 63 then yield(0.9 + 0.1 * x / size) end
    end
    local nb = 0
    for bx = M.SITE, size - M.SITE, M.SITE * 3 do
        nb = nb + 1
        for x = bx, bx + 1 do
            local yc = size // 2 + math.floor(6 * math.sin(x / 23))
            rect(x, yc - 2, x, yc + 2, R + 1, ROCK)
            columns(x, yc - 4, x, yc - 3, R + 1, ROCK, DIRT)
            columns(x, yc + 3, x, yc + 4, R + 1, ROCK, DIRT)
        end
        local yc = size // 2 + math.floor(6 * math.sin(bx / 23))
        poi(("Bridge %d: on the deck"):format(nb), bx, yc)
        poi(("Bridge %d: in the river beside it"):format(nb), bx + 4, yc)
    end

    for _, p in ipairs(pois) do p.level = p.level or H(p.x, p.y) end
    table.sort(pois, function(a, b) return a.name < b.name end)
    return map, pois
end

function M.build(size, opts)
    return generate(size, opts, function() end)
end

function M.job(size, opts)
    local co = coroutine.create(function()
        return generate(size, opts, function(p) coroutine.yield(p) end)
    end)
    local job = { done = false, progress = 0, size = size }
    function job:step(budget)
        if self.done then return true, 1 end
        local t0 = os.clock()
        repeat
            local ok, a, b = coroutine.resume(co)
            if not ok then error(a, 0) end
            if coroutine.status(co) == "dead" then
                self.done, self.progress, self.map, self.pois = true, 1, a, b
                return true, 1
            end
            self.progress = a or self.progress
        until os.clock() - t0 >= (budget or 0.03)
        return false, self.progress
    end
    return job
end

return M
