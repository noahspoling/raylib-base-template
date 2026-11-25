local ui = gramarye.require("lib/ui")

local KEY_SPACE  = gramarye.input.key("space")
local KEY_ESCAPE = gramarye.input.key("escape")

local frames = {}
local skins = {}
local skin_names = { "Default", "Alt palette" }

local entity = nil
local frame_i = 1
local skin_i = 1
local anim_t = 0
local FRAME_TIME = 0.35

local function goto_main() gramarye.scene.change("main") end

return {
    on_enter = function()
        gramarye.systems.add("global")

        frames[1] = gramarye.textures.load("skins/blob_wave_00.png")
        frames[2] = gramarye.textures.load("skins/blob_wave_01.png")
        skins[1]  = gramarye.textures.load("skins/blob.skin.png")
        skins[2]  = gramarye.textures.load("skins/blob.skin.alt.png")

        entity = gramarye.entities.spawn()
        gramarye.entities.set_transform(entity, 0, 0)
        gramarye.entities.set_skinned_sprite(entity, frames[1], skins[1], 128, 160)

        frame_i, skin_i, anim_t = 1, 1, 0
    end,

    on_exit = function()
        if entity then
            gramarye.entities.despawn(entity)
            entity = nil
        end
    end,

    on_update = function(dt)
        if gramarye.input.key_pressed(KEY_ESCAPE) then
            goto_main()
            return
        end
        if gramarye.input.key_pressed(KEY_SPACE) then
            skin_i = skin_i % #skins + 1
        end

        anim_t = anim_t + dt
        if anim_t >= FRAME_TIME then
            anim_t = anim_t - FRAME_TIME
            frame_i = frame_i % #frames + 1
        end

        gramarye.entities.set_skinned_sprite(entity, frames[frame_i], skins[skin_i], 128, 160)
    end,

    on_draw = function()
        gramarye.ui.render(ui.Frame {
            id     = "skin_demo_root",
            layout = { w = { grow = true }, h = { grow = true },
                       dir = "column", align = { x = "center" }, pad = 20, gap = 8 },

            ui.Title "UV-Remap Skinning",
            ui.Label { text = "skin: " .. skin_names[skin_i], skin = "label_dim" },
            ui.Spacer(0),
            ui.Label { text = "space = swap skin   esc = back", skin = "hint" },
        })
    end,
}
