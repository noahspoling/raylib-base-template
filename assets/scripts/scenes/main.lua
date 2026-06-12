-- Main scene: replace this with your game. Lua orchestrates (scenes, UI,
-- transitions); per-frame heavy lifting belongs in C systems added via
-- gramarye.systems.add("name") after registering them in main.c.

return {
    on_enter = function()
        gramarye.systems.add("global")
        gramarye.log("main: enter")
    end,

    on_update = function(dt)
        if gramarye.input.key_pressed("space") then
            gramarye.scene.change("splash")
        end
    end,

    on_draw = function()
        gramarye.draw.text("hello from lua", 24, 24, 32)
        gramarye.draw.text("scene: " .. gramarye.scene.current(), 24, 72, 20, 180, 180, 180)
        gramarye.draw.text(string.format("time: %.1fs  frames: %d",
            gramarye.time.total(), gramarye.time.frames()), 24, 98, 20, 180, 180, 180)
        gramarye.draw.text("space = splash   F5 = reload script (desktop)", 24, 132, 20, 120, 200, 255)
    end,
}
