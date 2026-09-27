local ui      = gramarye.require("gramarye.ui")
local widgets = gramarye.require("lib/widgets")
local tab     = 1
local show_inv_popup = true

local KEY_SPACE  = gramarye.input.key("space")
local KEY_ESCAPE = gramarye.input.key("escape")
local KEY_K      = gramarye.input.key("k")

local function goto_splash()  gramarye.scene.change("splash") end
local function goto_planet()  gramarye.scene.change("planet") end
local function push_pause()   gramarye.scene.push("pause")    end
local function log_reload()   gramarye.log("reload pressed")  end

local items = {
    { label = "Sword",   value = "80g"  },
    { label = "Shield",  value = "60g"  },
    { label = "Potion",  value = "15g"  },
    { label = "Ring",    value = "200g" },
    { label = "Scroll",  value = "30g"  },
    { label = "Boots",   value = "45g"  },
}

local function overview_panel()
    return ui.Panel {
        id = "overview_panel",
        layout = { dir = "column", w = { grow = true }, h = { grow = true },
                   pad = 20, gap = 14 },

        ui.Title "Welcome",
        ui.Separator {},
        ui.Label  { text = "This is the gramarye-ui component demo.", size = 16 },
        ui.Label  { text = "scene: " .. gramarye.scene.current(),   skin = "label_dim" },
        ui.Label  { text = string.format("time: %.1f s  frames: %d",
                        gramarye.time.total(), gramarye.time.frames()), skin = "label_dim" },

        ui.Spacer(0),

        ui.Row {
            gap = 10,
            ui.Button {
                id       = "btn_splash",
                label    = "< Splash",
                on_click = goto_splash,
            },
            ui.Button {
                id       = "btn_pause",
                label    = "Pause (Esc)",
                on_click = push_pause,
            },
            ui.Button {
                id       = "btn_reload",
                label    = "Reload (F5)",
                on_click = log_reload,
            },
            ui.Button {
                id       = "btn_planet",
                label    = "Planet >",
                on_click = goto_planet,
            },
        },

        ui.Label { text = "space = splash   esc = pause   k = skin demo   F5 = reload script (desktop)", skin = "hint" },
    }
end

local function settings_panel()
    return ui.Panel {
        id = "settings_panel",
        layout = { dir = "column", w = { grow = true }, h = { grow = true },
                   pad = 16, gap = 4 },

        ui.Title "Settings",
        ui.Separator {},

        widgets.OptionRow {
            id    = "opt_platform",
            label = "Platform",
            value = gramarye.platform,
        },
        ui.Separator {},

        widgets.OptionRow {
            id       = "opt_volume",
            label    = "Volume",
            value    = 0 .. "%",
            on_click = function()
                settings_volume = (0  + 10) % 110
                gramarye.log("volume: " .. 0)
            end,
        },
        ui.Separator {},

        widgets.OptionRow {
            id    = "opt_res",
            label = "Resolution",
            value = gramarye.ui.screen_w() .. "×" .. gramarye.ui.screen_h(),
        },

        ui.Spacer(0),
    }
end

local search_text = ""
local sel_row     = 1

local function widgets_panel()
    return ui.Panel {
        id = "widgets_panel",
        layout = { dir = "column", w = { grow = true }, h = { grow = true },
                   pad = 16, gap = 12 },

        ui.Title "Widgets",
        ui.Separator {},

        ui.Row {
            gap = 10, align = { y = "center" },
            ui.Label { text = "Search:", size = 16 },
            ui.TextBox {
                id = "search", value = search_text, w = 240,
                placeholder = "type here...",
                on_change = function(s) search_text = s end,
            },
            ui.Label { text = (search_text ~= "" and ("= " .. search_text) or ""),
                       skin = "label_dim" },
        },

        ui.Row {
            gap = 10, align = { y = "center" },
            ui.Button { id = "tip_btn", label = "Hover me" },
            ui.Tooltip { to_id = "tip_btn", text = "A floating tooltip (passthrough)" },
            ui.Label { text = "has a tooltip", skin = "label_dim" },
        },

        ui.Label { text = "Grid (12 cells, 6 cols):", skin = "label_dim" },
        ui.Grid {
            id = "demo_grid", count = 12, cols = 6, cell_h = 40, gap = 6,
            cell = function(i)
                return ui.Frame {
                    bg = { 40, 40, 70, 255 }, radius = 4,
                    layout = { w = 40, h = 40, align = "center" },
                    ui.Label { text = tostring(i), size = 14 },
                }
            end,
        },

        ui.Label { text = "List (" .. gramarye.demo.count()
                          .. " C-backed rows, virtualized):", skin = "label_dim" },
        ui.List {
            id = "demo_list", count = gramarye.demo.count(),
            row_h = 28, h = 170, selected = sel_row,
            on_select = function(i) sel_row = i; gramarye.log("selected row " .. i) end,
            cell = function(i, st)
                local name, value = gramarye.demo.row(i)
                return ui.Row {
                    gap = 8, align = { y = "center" }, w = { grow = true },
                    ui.Label { text = name, size = 14,
                               color = st.selected and { 255, 255, 255, 255 } or nil },
                    ui.Spacer(),
                    ui.Label { text = value, size = 13, color = { 150, 175, 140, 255 } },
                }
            end,
        },
    }
end

local panels     = { overview_panel, settings_panel, widgets_panel }
local tabs       = { "Overview", "Settings", "Widgets" }
local tab_clicks = {}
for i = 1, #tabs do
    tab_clicks[i] = function() tab = i end
end

local function tab_bar()
    local btns = {}
    for i, label in ipairs(tabs) do
        btns[i] = ui.Button {
            id       = "tab_" .. i,
            label    = label,
            h        = 34,
            bg       = tab == i and {50,50,90,255} or nil,
            on_click = tab_clicks[i],
        }
    end
    return ui.Row {
        id  = "tab_bar",
        bg  = {12,12,24,255},
        layout = { w = { grow = true }, h = 44, gap = 2,
                   pad = { h = 8, v = 4 } },
        table.unpack(btns),
    }
end

return {
    on_enter = function()
        gramarye.systems.add("global")
        tab = 1
    end,

    on_pause  = function() gramarye.log("main paused")  end,
    on_resume = function() gramarye.log("main resumed") end,

    on_update = function(dt)
        if gramarye.input.key_pressed(KEY_SPACE) then
            gramarye.scene.change("splash")
        end
        if gramarye.input.key_pressed(KEY_ESCAPE) then
            gramarye.scene.push("pause")
        end
        if gramarye.input.key_pressed(KEY_K) then
            gramarye.scene.change("skin_demo")
        end
    end,

    on_draw = function()
        gramarye.ui.render(ui.Column {
            id     = "main_root",
            layout = { dir = "column", w = { grow = true }, h = { grow = true } },

            widgets.HeaderBar {
                title   = "Gramarye Demo",
                actions = {
                    ui.Button {
                        id       = "hdr_exit",
                        label    = "×",
                        w        = 36, h = 36,
                        on_click = goto_splash,
                    },
                },
            },

            tab_bar(),

            panels[tab](),
        })
        if show_inv_popup then
            gramarye.ui.render(ui.Popup {
                id = "inv_win", title = "Inventory", x = 120, y = 80, w = 320,
                pulse = { beam = { 110, 160, 255, 255 } },
                on_close = function() show_inv_popup = false end,
                ui.Label "contents go here",
            })
        end
    end,
}
