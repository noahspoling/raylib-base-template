local ui    = gramarye.require("lib/ui")
local theme = gramarye.require("gramarye.theme")
local t     = 0

theme.apply {
    continue       = { bg = {50, 65, 120, 255}, text = {255, 255, 255, 255},
                        radius = 6, font_size = 18, pad = { h = 20, v = 10 } },
    continue_hover = { bg = {25, 32, 60, 255},  text = {230, 230, 230, 255},
                        radius = 6, font_size = 18, pad = { h = 20, v = 10 } },
}

local function apply_skin()
    local prefix = gramarye.asset_prefix or ""
    local atlas  = gramarye.ui.load_texture(prefix .. "textures/ui_atlas.png")
    if not atlas then return end
    local w, h = gramarye.ui.texture_size(atlas)
    local np   = gramarye.ui.ninepatch(atlas, 0, 0, w, h, 12, 12, 12, 12)
    theme.bind_image("button", { nine = np })
    theme.bind_image("button_hover", { nine = np })
end

local sprites = {}

local function spawn_sprites()
    local colors = {
        { 200,  80,  80 }, { 220, 160,  60 }, {  90, 190,  90 },
        {  80, 140, 220 }, { 170,  90, 200 },
    }
    for i, c in ipairs(colors) do
        local e = gramarye.entities.spawn()
        gramarye.entities.set_transform(e, (i - 3) * 90, 190)
        gramarye.entities.set_sprite(e, 0, 40, 40, c[1], c[2], c[3], 255)
        sprites[i] = e
    end
end

local function goto_main() gramarye.scene.change("main") end

local function build_tree()
    return ui.Frame {
        id     = "splash_root",
        layout = { w = { grow = true }, h = { grow = true },
                   dir = "column", align = "center", gap = 20 },

        ui.Title  "GRAMARYE",
        ui.Separator { h = 2 },
        ui.Label  { text = "Clay + Lua UI online", skin = "hint" },

        ui.Spacer(24),

        ui.Button {
            id       = "splash_continue",
            label    = "Continue  >",
            w        = 200,
            skin     = "continue",
            on_click = goto_main,
        },
    }
end

return {
    on_enter = function()
        t = 0
        apply_skin()
        gramarye.systems.add("global")
        spawn_sprites()
    end,

    on_exit = function()
        for _, e in ipairs(sprites) do
            gramarye.entities.despawn(e)
        end
        sprites = {}
    end,

    on_update = function(dt)
        t = t + dt
        for i, e in ipairs(sprites) do
            gramarye.entities.set_transform(e,
                (i - 3) * 90,
                190 + math.sin(t * 2 + i) * 14,
                t * 60 + i * 30)
        end
    end,

    on_draw = function()
        gramarye.ui.render(build_tree())
    end,
}
