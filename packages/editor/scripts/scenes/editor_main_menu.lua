local ui = gramarye.require("gramarye.ui")

local function scene_push(scene)      return function() gramarye.scene.push(scene)   end end
local function scene_change_to(scene) return function() gramarye.scene.change(scene) end end

local open_tilemap_editor = scene_push("tilemap_editor")
local open_map_viewer     = scene_push("tilemap_view")
local goto_shaders        = scene_change_to("shader_playground")
local goto_demo           = scene_change_to("main")

local KEY_H = gramarye.input.key("h")

local function shader_button(col)
    if gramarye.shader_playground then
        col[#col + 1] = ui.Button { id = "em_shaders", label = "Shader playground", w = 240,
                                    on_click = goto_shaders }
    end
    return col
end

return {
    on_enter = function()
        gramarye.systems.add("global")
    end,

    on_update = function(dt)
        if gramarye.shader_playground and gramarye.input.key_pressed(KEY_H) then
            goto_shaders()
        end
    end,

    on_draw = function()
        local tools = shader_button(ui.Column {
            gap = 16, align = { x = "center" },
            ui.Button { id = "em_tilemap", label = "Tilemap editor", w = 240,
                        on_click = open_tilemap_editor },
            ui.Button { id = "em_map_view", label = "Map viewer", w = 240,
                        on_click = open_map_viewer },
        })
        gramarye.ui.render(ui.Frame {
            id     = "editor_menu_root",
            layout = { w = { grow = true }, h = { grow = true },
                       dir = "column", align = "center", gap = 16 },

            ui.Title "Editor",
            ui.Label { text = "editing with " .. #gramarye.mods.active() .. " packages mounted",
                       skin = "label_dim" },
            ui.Spacer(12),
            tools,
            ui.Button { id = "em_demo",    label = "Demo hub (preview)", w = 240,
                        on_click = goto_demo },
            ui.Button { id = "em_quit",    label = "Quit", w = 240, on_click = gramarye.app.quit },
        })
    end,
}
