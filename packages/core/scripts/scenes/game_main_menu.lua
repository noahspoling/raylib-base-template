local ui = gramarye.require("gramarye.ui")

local function scene_change_to(scene) return function() gramarye.scene.change(scene) end end

local goto_demo   = scene_change_to("main")
local goto_planet = scene_change_to("planet")
local push_board  = function() gramarye.scene.push("board_demo") end

local function mods_text()
    local names = {}
    for _, p in ipairs(gramarye.mods.active()) do
        if not p.builtin then names[#names + 1] = p.name end
    end
    return #names > 0 and table.concat(names, ", ") or "none"
end

return {
    on_enter = function()
        gramarye.systems.add("global")
    end,

    on_draw = function()
        gramarye.ui.render(ui.Frame {
            id     = "game_menu_root",
            layout = { w = { grow = true }, h = { grow = true },
                       dir = "column", align = "center", gap = 16 },

            ui.Title "Game",
            ui.Label { text = "mods: " .. mods_text(), skin = "label_dim" },
            ui.Spacer(12),
            ui.Button { id = "gm_demo",   label = "Demo hub", w = 240, on_click = goto_demo },
            ui.Button { id = "gm_planet", label = "Planet",   w = 240, on_click = goto_planet },
            ui.Button { id = "gm_board",  label = "Board demo", w = 240, on_click = push_board },
            ui.Button { id = "gm_quit",   label = "Quit",     w = 240, on_click = gramarye.app.quit },
        })
    end,
}
