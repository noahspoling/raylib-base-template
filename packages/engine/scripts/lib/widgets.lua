local ui    = gramarye.require("gramarye.ui")
local theme = gramarye.require("gramarye.theme")

local is_mobile = gramarye.platform == "android"

theme.apply {
    slot         = { bg={22,22,40,255},
                     border={50,50,75,255}, border_hover={80,130,220,255},
                     border_selected={200,160,60,255}, radius=4 },
    badge        = { bg={180,60,60,255},  text={255,255,255,255}, size=11 },
    panel_header = { bg={22,22,42,255},   radius=0 },
    tooltip      = { bg={10,10,22,240}, text={200,200,200,255}, radius=4, font_size=14 },
}

local M = {}

function M.HeaderBar(props)
    local s    = theme.skins.panel_header
    local node = {
        _type  = "frame",
        id     = props.id or "header_bar",
        bg     = props.bg or s.bg,
        layout = {
            w     = { grow = true },
            h     = props.h or 52,
            pad   = { h = 14, v = 0 },
            gap   = 10,
            align = { y = "center" },
        },
        { _type = "text",
          text  = props.title or "",
          size  = props.title_size or 20,
          color = props.title_color or {220, 220, 220, 255} },
        ui.Spacer(),
    }
    if props.actions then
        for _, a in ipairs(props.actions) do
            node[#node + 1] = a
        end
    end
    return node
end

function M.Slot(props)
    local hov  = gramarye.ui.hovered(props.id)
    local s    = theme.skins.slot
    local sz   = props.size or (is_mobile and 72 or 56)
    local bc   = props.selected and s.border_selected
               or hov           and s.border_hover
               or                   s.border
    local bw   = (hov or props.selected) and 2 or 1

    local node = ui.Frame {
        id     = props.id,
        bg     = props.bg or s.bg,
        radius = s.radius,
        border = { color = bc, width = bw },
        layout = { w = sz, h = sz, align = "center" },
        on_click = props.on_click,
        on_hover = props.on_hover,
    }
    if props.icon  then node[#node + 1] = props.icon  end
    if props.badge then node[#node + 1] = props.badge end
    return node
end

function M.Badge(props)
    local s = theme.skins.badge
    return ui.Frame {
        bg     = props.bg or s.bg,
        radius = 4,
        layout = { pad = { h=4, v=2 } },
        { _type = "text",
          text  = tostring(props.count or props.text or ""),
          size  = props.size or s.size,
          color = s.text },
    }
end

function M.ItemCard(props)
    local sz   = props.size or (is_mobile and 72 or 56)
    local node = ui.Column {
        id     = props.id,
        gap    = 4,
        w      = sz + 8,
        align  = { x = "center" },
        on_click = props.on_click,
    }
    node[1] = M.Slot {
        id       = props.id .. "_slot",
        size     = sz,
        icon     = props.icon,
        badge    = props.badge,
        selected = props.selected,
        on_click = props.on_click,
    }
    if props.label then
        node[2] = ui.Label { text = props.label, size = 12 }
    end
    return node
end

function M.TradeRow(props)
    return ui.Row {
        id     = props.id,
        bg     = props.bg,
        h      = theme.MIN_TAP_H + 8,
        pad    = { h = 8, v = 4 },
        gap    = 12,
        align  = { y = "center" },
        w      = { grow = true },
        border = { color = { 40,40,60,255 }, width = 1 },
        on_click = props.on_click,

        props.icon and M.Slot { id = props.id.."_icon", size = 40, icon = props.icon } or nil,

        ui.Column {
            gap = 2, w = { grow = true },
            props.name and ui.Label { text = props.name, size = 16 } or nil,
            props.desc and ui.Label { text = props.desc, size = 13,
                                      color = {140,140,160,255} } or nil,
        },

        props.value and ui.Label { text = props.value, size = 15,
                                   color = {200,180,80,255} } or nil,
        props.action,
    }
end

function M.OptionRow(props)
    local hov = gramarye.ui.hovered(props.id)
    return ui.Row {
        id     = props.id,
        bg     = hov and {30,30,50,200} or {0,0,0,0},
        h      = theme.MIN_TAP_H,
        pad    = { h = 16, v = 0 },
        gap    = 12,
        align  = { y = "center" },
        w      = { grow = true },
        on_click = props.on_click,
        ui.Label { text = props.label or "", size = 16 },
        ui.Spacer(),
        props.value and ui.Label { text = props.value, size = 16,
                                   color = {100,160,220,255} } or nil,
        props.right,
    }
end

function M.Checkbox(props)
    local on  = props.checked
    local hov = gramarye.ui.hovered(props.id)
    return ui.Row {
        id       = props.id,
        w        = { grow = true },
        h        = 30,
        gap      = 10,
        align    = { y = "center" },
        on_click = props.on_click,
        ui.Frame {
            id     = (props.id or "cb") .. "_box",
            bg     = on and {70,120,180,255} or {36,40,50,255},
            radius = 3,
            border = { color = hov and {110,150,210,255} or {80,90,110,255}, width = 1 },
            layout = { w = 18, h = 18, align = "center" },
            on and { _type = "text", text = "x", size = 14, color = {255,255,255,255} } or nil,
        },
        ui.Label { text = props.label or "", size = 16 },
    }
end

local ptr_down, ptr_pressed = false, false
local slider_drag = nil
local slider_released = nil

function M.begin_frame()
    local down = gramarye.ui.mouse_down()
    ptr_pressed = down and not ptr_down
    ptr_down = down
    slider_released = nil
    if not down and slider_drag then
        slider_released = slider_drag
        slider_drag = nil
    end
end

local function clamp01(t) return t < 0 and 0 or (t > 1 and 1 or t) end

function M.Slider(props)
    local id       = props.id
    local track_id = id .. "_track"
    local lo, hi   = props.min or 0, props.max or 1
    local log      = props.log and lo > 0
    local v        = props.value or lo

    local function to_t(x)
        if log then return math.log(x / lo) / math.log(hi / lo) end
        return (x - lo) / (hi - lo)
    end
    local function from_t(t)
        if log then return lo * (hi / lo) ^ t end
        return lo + (hi - lo) * t
    end

    if ptr_pressed and gramarye.ui.hovered(track_id) then slider_drag = id end
    if slider_drag == id then
        local rx, _, rw = gramarye.ui.element_rect(track_id)
        if rw > 0 then
            local mx = gramarye.ui.mouse_pos()
            local nv = from_t(clamp01((mx - rx) / rw))
            if props.step then nv = math.floor(nv / props.step + 0.5) * props.step end
            if nv ~= v then
                v = nv
                if props.on_change then props.on_change(nv) end
            end
        end
    end

    if slider_released == id and props.on_commit then props.on_commit(v) end

    local active = slider_drag == id
    local hov    = active or gramarye.ui.hovered(track_id)
    return ui.Row {
        id = id, w = { grow = true }, h = 24, gap = 8, align = { y = "center" },
        { _type = "frame", layout = { w = props.label_w or 92 },
          ui.Label { text = props.label or "", size = 13, color = { 175, 180, 200, 255 } } },
        { _type  = "frame",
          id     = track_id,
          bg     = { 30, 34, 48, 255 },
          radius = 3,
          border = { color = hov and { 110, 150, 210, 255 } or { 55, 62, 82, 255 }, width = 1 },
          layout = { w = { grow = true }, h = 14 },
          { _type = "frame",
            bg     = active and { 120, 170, 240, 255 } or { 70, 120, 190, 255 },
            radius = 3,
            layout = { w = { pct = clamp01(to_t(v)) }, h = { grow = true } } },
        },
        { _type = "frame", layout = { w = props.value_w or 70 },
          ui.Label { text = string.format(props.fmt or "%.2f", v), size = 13,
                     color = { 130, 185, 240, 255 } } },
    }
end

function M.Chip(props)
    local hov = gramarye.ui.hovered(props.id)
    return {
        _type  = "frame",
        id     = props.id,
        bg     = props.selected and (props.selected_bg or { 70, 120, 180, 255 })
              or hov and { 52, 58, 82, 255 } or (props.bg or { 36, 40, 56, 255 }),
        radius = 4,
        layout = { w = props.w, h = props.h or 28, pad = { h = 10, v = 0 }, align = { y = "center" } },
        on_click = props.on_click,
        { _type = "text", text = props.label or "", size = props.size or 13,
          color = { 225, 228, 240, 255 } },
    }
end

function M.Segmented(props)
    local row = ui.Row { gap = 4, align = { y = "center" } }
    for i, item in ipairs(props.items) do
        row[i] = M.Chip {
            id       = props.id .. "_" .. i,
            label    = props.labels and props.labels[i] or item,
            selected = props.selected == i,
            on_click = function() if props.on_select then props.on_select(i, item) end end,
        }
    end
    return row
end

return M
