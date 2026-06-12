-- Engine-level splash: first scene on every target.
-- Advances to main after 2s, or on click/tap.
local t = 0

return {
    on_enter = function()
        t = 0
        gramarye.systems.add("global")
        gramarye.log("splash: enter")
    end,

    on_update = function(dt)
        t = t + dt
        if t > 2.0 or gramarye.input.mouse_pressed() then
            gramarye.scene.change("main")
        end
    end,

    on_draw = function()
        gramarye.draw.text("GRAMARYE", 270, 250, 48)
        gramarye.draw.text("click or wait...", 330, 320, 20, 150, 150, 150)
    end,

    on_exit = function()
        gramarye.log("splash: exit")
    end,
}
