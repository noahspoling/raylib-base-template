-- Splash scene: full-screen Clay UI demo over C-rendered sprite entities.
-- Shows the engine title, waits 2 s or responds to click → main.
local ui    = gramarye.require("lib/ui")
local theme = gramarye.require("gramarye.theme")
local t     = 0

-- Opt-in image skinning: if the UI atlas is present, skin buttons with a
-- nine-patch frame. If the atlas is missing, load_texture returns nil and
-- buttons keep their color skin — no crash, pure graceful fallback. Runs once;
-- theme bindings persist across scenes (the module is cached).
local function apply_skin()
    local prefix = gramarye.asset_prefix or ""
    local atlas  = gramarye.ui.load_texture(prefix .. "textures/ui_atlas.png")
    if not atlas then return end
    local w, h = gramarye.ui.texture_size(atlas)
    local np   = gramarye.ui.ninepatch(atlas, 0, 0, w, h, 12, 12, 12, 12)
    theme.bind_image("button", { nine = np })
    theme.bind_image("button_hover", { nine = np })
end

-- Sprite entity demo: quads simulated/moved via gramarye.entities and drawn
-- by the C sprite_render system (under the camera, below the UI). The UI tree
-- here has no opaque background, so they show through.
local sprites = {}

local function spawn_sprites()
    local colors = {
        { 200,  80,  80 }, { 220, 160,  60 }, {  90, 190,  90 },
        {  80, 140, 220 }, { 170,  90, 200 },
    }
    for i, c in ipairs(colors) do
        local e = gramarye.entities.spawn()
        -- World (0,0) is screen center (camera offset). Row below the title.
        gramarye.entities.set_transform(e, (i - 3) * 90, 190)
        gramarye.entities.set_sprite(e, 0, 40, 40, c[1], c[2], c[3], 255)
        sprites[i] = e
    end
end

local function goto_main() gramarye.scene.change("main") end

-- Static UI: no per-frame data, so the tree is built once and re-rendered.
-- (Tables/closures built inside on_draw are per-frame garbage; hoist what
-- doesn't change.)
local splash_tree = nil

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
            on_click = goto_main,
        },

        ui.Label { text = "or wait 2 s", skin = "label_dim" },
    }
end

return {
    on_enter = function()
        t = 0
        apply_skin()
        gramarye.systems.add("global")
        spawn_sprites()
        splash_tree = build_tree()
    end,

    on_exit = function()
        for _, e in ipairs(sprites) do
            gramarye.entities.despawn(e)
        end
        sprites = {}
    end,

    on_update = function(dt)
        t = t + dt
        -- Bob and spin the quads; motion state lives in the ECS, not Lua.
        for i, e in ipairs(sprites) do
            gramarye.entities.set_transform(e,
                (i - 3) * 90,
                190 + math.sin(t * 2 + i) * 14,
                t * 60 + i * 30)
        end
        if t > 2.0 then gramarye.scene.change("main") end
    end,

    on_draw = function()
        gramarye.ui.render(splash_tree)
    end,
}
