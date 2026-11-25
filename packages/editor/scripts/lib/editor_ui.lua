local ui    = gramarye.require("lib/ui")
local theme = gramarye.require("gramarye.theme")

theme.apply {
    ed_btn          = { bg = { 40, 40, 64, 255 },   text = { 210, 210, 220, 255 }, radius = 4, font_size = 14, pad = { h = 10, v = 4 } },
    ed_btn_hover    = { bg = { 64, 64, 110, 255 },  text = { 255, 255, 255, 255 }, radius = 4, font_size = 14, pad = { h = 10, v = 4 } },
    ed_btn_on       = { bg = { 70, 110, 180, 255 }, text = { 255, 255, 255, 255 }, radius = 4, font_size = 14, pad = { h = 10, v = 4 } },
    ed_btn_on_hover = { bg = { 90, 135, 210, 255 }, text = { 255, 255, 255, 255 }, radius = 4, font_size = 14, pad = { h = 10, v = 4 } },
}

local M = {}

M.BAR_BG = { 20, 20, 34, 255 }

function M.button(id, label, on_click, active, w)
    return ui.Button { id = id, label = label, on_click = on_click, w = w, h = 30,
                       skin = active and "ed_btn_on" or "ed_btn" }
end

function M.short_id(id)
    return id and (id:match("([^.]+)$") or id) or "none"
end

function M.leave()
    if gramarye.scene.depth() > 1 then gramarye.scene.pop()
    else gramarye.scene.change(gramarye.app.home()) end
end

return M
