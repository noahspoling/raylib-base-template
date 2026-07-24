-- Pause scene: demo of the scene stack. main.lua pushes this with
-- gramarye.scene.push("pause"); the scene below keeps all its state (Lua
-- table, C systems, entities) but stops updating/drawing until we pop.
local ui = gramarye.require("gramarye.ui")

local KEY_ESCAPE = gramarye.input.key("escape")

local function resume() gramarye.scene.pop() end
local function quit_to_splash() gramarye.scene.change("splash") end

return {
    on_update = function(dt)
        if gramarye.input.key_pressed(KEY_ESCAPE) then
            gramarye.scene.pop()
        end
    end,

    on_draw = function()
        gramarye.ui.render(ui.Frame {
            id     = "pause_root",
            bg     = { 6, 6, 14, 235 },
            layout = { w = { grow = true }, h = { grow = true },
                       dir = "column", align = "center", gap = 16 },

            ui.Title "Paused",
            ui.Label { text = "scene stack depth: " .. gramarye.scene.depth(),
                       skin = "label_dim" },
            ui.Spacer(12),
            ui.Button { id = "pause_resume", label = "Resume  (Esc)", w = 220,
                        on_click = resume },
            ui.Button { id = "pause_quit", label = "Quit to Splash", w = 220,
                        on_click = quit_to_splash },
        })
    end,
}
