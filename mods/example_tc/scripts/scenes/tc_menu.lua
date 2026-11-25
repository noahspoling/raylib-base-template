local ui = gramarye.require("gramarye.ui")

local function stack_text()
    local ids = {}
    for _, p in ipairs(gramarye.mods.active()) do ids[#ids + 1] = p.id end
    return table.concat(ids, "  <  ")
end

return {
    on_draw = function()
        gramarye.ui.render(ui.Frame {
            id     = "tc_root",
            bg     = { 30, 12, 12, 255 },
            layout = { w = { grow = true }, h = { grow = true },
                       dir = "column", align = "center", gap = 16 },
            ui.Title "A Different Game",
            ui.Label { text = "core is not mounted. stack: " .. stack_text(), skin = "label_dim" },
            ui.Spacer(12),
            ui.Button { id = "tc_quit", label = "Quit", w = 200, on_click = gramarye.app.quit },
        })
    end,
}
