local M = {}

M.SPLASH = { image = "textures/fx/splash.png", frames = 6, w = 24, h = 20, fps = 14 }
M.RIPPLE_PERIOD = 1.4
M.RIPPLE_REACH = 9
M.SWIM_SINK = 0.7

function M.state(map, x, y, level, body)
    local t = map:tile()
    local depth, surface, deep = map:water_at(math.floor(x / t), math.floor(y / t), level)
    local st = { mode = "dry", depth = depth, surface = surface, deep = deep }
    if depth > 0 or deep then
        st.mode = (not deep and depth <= ((body and body.wade) or 1)) and "wade" or "swim"
    end
    return st
end

function M.stand_z(st, z, map)
    if st.mode ~= "swim" then return z end
    local lh = map and (map:elevation()) or 12
    return math.max(z, st.surface - lh * M.SWIM_SINK)
end

function M.effects(v, st, x, y, clock, moving)
    if st.mode == "dry" then return v end
    v.rings = v.rings or {}
    for i = 0, 1 do
        local u = ((clock / M.RIPPLE_PERIOD) + i * 0.5) % 1
        local a = math.floor(230 * (1 - u) ^ 0.7)
        v.rings[#v.rings + 1] = { x = x, y = y, z = st.surface + 0.3, r = 4 + u * M.RIPPLE_REACH,
                                  color = { 240, 250, 255, a } }
    end
    if moving then
        local s = M.SPLASH
        local f = math.floor(clock * s.fps) % s.frames
        v.sprites = v.sprites or {}
        v.sprites[#v.sprites + 1] = { image = s.image, x = x, y = y - 0.5, z = st.surface - 3, w = s.w * 0.85,
                                      h = s.h * 0.85, src = { x = f * s.w, y = 0, w = s.w, h = s.h } }
    end
    return v
end

return M
