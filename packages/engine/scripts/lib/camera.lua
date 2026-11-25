local M = {}
M.__index = M

M.ZOOMS = { 0.0625, 0.125, 0.25, 0.5, 1, 1.5, 2, 3, 4, 6, 8 }
M.FOV = 30
M.MAX_TILT = 85
M.ORBIT_RATE = 18
local ORTHO_DISTANCE = 20000

local function clamp(v, lo, hi) return math.max(lo, math.min(v, hi)) end

function M.new(opts)
    opts = opts or {}
    local cam = setmetatable({
        x          = opts.x or 0,
        y          = opts.y or 0,
        zoom_index = opts.zoom_index or 7,
        smoothing  = opts.smoothing or 10,
        bounds     = opts.bounds,
        rotation   = opts.rotation or 0,
        tilt       = opts.tilt or 40,
        fov        = opts.fov or M.FOV,
    }, M)
    cam.zoom = M.ZOOMS[cam.zoom_index]
    cam.turn_to, cam.tilt_to = cam.rotation, cam.tilt
    cam.seen_rotation, cam.seen_tilt = cam.rotation, cam.tilt
    return cam
end

function M.rect_of(id)
    local x, y, w, h = gramarye.ui.element_rect(id)
    return { x = x, y = y, w = w, h = h }
end

local function frame(self)
    local th, ph = math.rad(self.rotation), math.rad(clamp(self.tilt, 0, M.MAX_TILT))
    local st, ct, sp, cp = math.sin(th), math.cos(th), math.sin(ph), math.cos(ph)
    return { sp * st, cp, sp * ct }, { -st * cp, sp, -ct * cp }, { ct, 0, -st }
end

local function size(rect)
    return math.floor(rect.w + 0.5), math.floor(rect.h + 0.5)
end

function M:ray(rect, sx, sy)
    local c, u, r = frame(self)
    local w, h = size(rect)
    local ax, ay, aspect = 2 * (sx - rect.x) / w - 1, 1 - 2 * (sy - rect.y) / h, w / h
    local ox, oy = ax * aspect, ay
    if self.fov > 0 then
        local tf = math.tan(math.rad(self.fov) / 2)
        local d = h / (2 * self.zoom * tf)
        local o = { self.x + c[1] * d, c[2] * d, self.y + c[3] * d }
        return o, { -c[1] + (r[1] * ox + u[1] * oy) * tf,
                    -c[2] + (r[2] * ox + u[2] * oy) * tf,
                    -c[3] + (r[3] * ox + u[3] * oy) * tf }
    end
    local half = h / self.zoom / 2
    return { self.x + c[1] * ORTHO_DISTANCE + (r[1] * ox + u[1] * oy) * half,
             c[2] * ORTHO_DISTANCE + (r[2] * ox + u[2] * oy) * half,
             self.y + c[3] * ORTHO_DISTANCE + (r[3] * ox + u[3] * oy) * half }, { -c[1], -c[2], -c[3] }
end

function M:board_ray(rect, sx, sy)
    local o, d = self:ray(rect, sx, sy)
    return o[1], o[3], o[2], d[1], d[3], d[2]
end

function M:screen_to_world(rect, sx, sy, height)
    local o, d = self:ray(rect, sx, sy)
    local t = d[2] < -1e-6 and ((height or 0) - o[2]) / d[2] or 1e6
    t = math.min(t, 1e6)
    return o[1] + d[1] * t, o[3] + d[3] * t
end

function M:world_to_screen(rect, wx, wy, height)
    local c, u, r = frame(self)
    local w, h = size(rect)
    local aspect = w / h
    local px, py, pz = wx - self.x, (height or 0), wy - self.y
    local x, y = px * r[1] + py * r[2] + pz * r[3], px * u[1] + py * u[2] + pz * u[3]
    local ax, ay
    if self.fov > 0 then
        local tf = math.tan(math.rad(self.fov) / 2)
        local dist = h / (2 * self.zoom * tf) - (px * c[1] + py * c[2] + pz * c[3])
        ax, ay = x / (dist * tf * aspect), y / (dist * tf)
    else
        local half = h / self.zoom / 2
        ax, ay = x / (half * aspect), y / half
    end
    return rect.x + (ax + 1) / 2 * w, rect.y + (1 - ay) / 2 * h
end

function M:unrotate(dx, dy)
    local a = math.rad(self.rotation)
    local c, s = math.cos(a), math.sin(a)
    return dx * c + dy * s, -dx * s + dy * c
end

function M:rotate(dx, dy)
    local a = math.rad(self.rotation)
    local c, s = math.cos(a), math.sin(a)
    return dx * c - dy * s, dx * s + dy * c
end

function M:rotate_by(degrees)
    self.rotation = (self.rotation + degrees) % 360
end

function M:tilt_by(degrees)
    self.tilt = clamp(self.tilt + degrees, 0, M.MAX_TILT)
end

function M:orbit_by(dturn, dtilt)
    self.turn_to = self.turn_to + (dturn or 0)
    self.tilt_to = clamp(self.tilt_to + (dtilt or 0), 0, M.MAX_TILT)
end

function M:orbit_to(turn, tilt)
    if turn then self.turn_to = turn end
    if tilt then self.tilt_to = clamp(tilt, 0, M.MAX_TILT) end
end

function M:snap_turn(step)
    self.turn_to = math.floor(self.turn_to / step + 0.5) * step
end

function M:ease_orbit(dt)
    if self.rotation ~= self.seen_rotation then self.turn_to = self.rotation end
    if self.tilt ~= self.seen_tilt then self.tilt_to = self.tilt end
    local k = 1 - math.exp(-M.ORBIT_RATE * dt)
    local turn = (self.turn_to - self.rotation + 540) % 360 - 180
    if math.abs(turn) < 0.01 then
        self.rotation = self.turn_to % 360
        self.turn_to = self.rotation
    else
        self.rotation = (self.rotation + turn * k) % 360
    end
    local tilt = self.tilt_to - self.tilt
    self.tilt = math.abs(tilt) < 0.01 and self.tilt_to or self.tilt + tilt * k
    self.seen_rotation, self.seen_tilt = self.rotation, self.tilt
end

function M:set_zoom_index(i, rect, sx, sy)
    i = clamp(i, 1, #M.ZOOMS)
    if i == self.zoom_index then return end
    local wx, wy
    if rect and sx then wx, wy = self:screen_to_world(rect, sx, sy) end
    self.zoom_index = i
    self.zoom = M.ZOOMS[i]
    if wx then
        local nx, ny = self:screen_to_world(rect, sx, sy)
        self.x, self.y = self.x + (wx - nx), self.y + (wy - ny)
    end
end

function M:zoom_by(steps, rect, sx, sy)
    self:set_zoom_index(self.zoom_index + steps, rect, sx, sy)
end

function M:fit(rect, world_w, world_h)
    local best = 1
    for i, z in ipairs(M.ZOOMS) do
        if world_w * z <= rect.w and world_h * z <= rect.h then best = i end
    end
    self.zoom_index, self.zoom = best, M.ZOOMS[best]
    self.x, self.y = world_w / 2, world_h / 2
end

function M:pan_screen(dx, dy, rect, sx, sy)
    if rect and sx then
        local ax, ay = self:screen_to_world(rect, sx, sy)
        local bx, by = self:screen_to_world(rect, sx + dx, sy + dy)
        self.x, self.y = self.x - (bx - ax), self.y - (by - ay)
        return
    end
    local ty = math.max(math.cos(math.rad(clamp(self.tilt, 0, M.MAX_TILT))), 0.2)
    local wx, wy = self:unrotate(dx / self.zoom, dy / self.zoom / ty)
    self.x, self.y = self.x - wx, self.y - wy
end

function M:follow(tx, ty, dt)
    if self.smoothing <= 0 then
        self.x, self.y = tx, ty
        return
    end
    local k = 1 - math.exp(-self.smoothing * dt)
    self.x = self.x + (tx - self.x) * k
    self.y = self.y + (ty - self.y) * k
end

function M:clamp(rect)
    local b = self.bounds
    if not b or rect.w <= 0 or rect.h <= 0 then return end
    local function axis(v, lo, hi, half)
        if hi - lo <= half * 2 then return (lo + hi) * 0.5 end
        return clamp(v, lo + half, hi - half)
    end
    self.x = axis(self.x, b[1], b[3], rect.w * 0.5 / self.zoom)
    self.y = axis(self.y, b[2], b[4], rect.h * 0.5 / self.zoom)
end

function M:view(extra)
    local v = extra or {}
    v.x, v.y, v.zoom, v.rotation, v.tilt, v.fov = self.x, self.y, self.zoom, self.rotation, self.tilt, self.fov
    return v
end

return M
