local ui      = gramarye.require("gramarye.ui")
local widgets = gramarye.require("lib/widgets")

local mods      = {}
local selection = {}
local status    = ""

local function index_of(list, value)
    for i, v in ipairs(list) do if v == value then return i end end
    return nil
end

local function refresh()
    mods = {}
    for _, p in ipairs(gramarye.mods.available("game")) do
        if not p.builtin then mods[#mods + 1] = p end
    end
    selection = {}
    for _, id in ipairs(gramarye.mods.selected()) do
        for _, p in ipairs(mods) do
            if p.id == id then selection[#selection + 1] = id end
        end
    end
end

local toggle_fns, up_fns, down_fns = {}, {}, {}

local function toggle(id)
    toggle_fns[id] = toggle_fns[id] or function()
        local i = index_of(selection, id)
        if i then table.remove(selection, i) else selection[#selection + 1] = id end
    end
    return toggle_fns[id]
end

local function move(fns, id, delta)
    fns[id] = fns[id] or function()
        local i = index_of(selection, id)
        local j = i and i + delta
        if j and j >= 1 and j <= #selection then
            selection[i], selection[j] = selection[j], selection[i]
        end
    end
    return fns[id]
end

local function launch(app)
    return function()
        local ok, err = gramarye.app.launch(app, { mods = selection })
        status = ok and ("started " .. app) or ("could not start " .. app .. ": " .. tostring(err))
    end
end
local play, edit = launch("game"), launch("editor")

local function mod_row(p)
    local order = index_of(selection, p.id)
    local note  = (not p.ok and p.reason)
               or (#p.replaces > 0 and ("replaces " .. table.concat(p.replaces, ", ")))
               or p.version
    local row = ui.Row {
        id = "mod_" .. p.folder, w = { grow = true }, gap = 8, align = { y = "center" },
        ui.Label { text = order and ("#" .. order) or "  ", size = 14, skin = "label_dim" },
        ui.Frame {
            layout = { w = { grow = true } },
            widgets.Checkbox { id = "mod_cb_" .. p.folder, label = p.name,
                               checked = order ~= nil, on_click = toggle(p.id) },
        },
        ui.Label { text = note or "", size = 13,
                   color = p.ok and { 140, 150, 170, 255 } or { 230, 120, 110, 255 } },
    }
    if order then
        row[#row + 1] = ui.Button { id = "mod_up_" .. p.folder, label = "^", w = 30, h = 28,
                                    on_click = move(up_fns, p.id, -1) }
        row[#row + 1] = ui.Button { id = "mod_dn_" .. p.folder, label = "v", w = 30, h = 28,
                                    on_click = move(down_fns, p.id, 1) }
    end
    return row
end

local function mod_list()
    local col = ui.Column { id = "mod_list", w = { grow = true }, gap = 6 }
    if #mods == 0 then
        col[1] = ui.Label { text = "No mods found. Put mod folders (with a mod.lua) in mods/.",
                            skin = "label_dim" }
    end
    for _, p in ipairs(mods) do col[#col + 1] = mod_row(p) end
    return col
end

local function child_text()
    local c = gramarye.app.child()
    if not c then return status end
    if c.running then return c.app .. " is running" end
    return c.app .. " exited (code " .. c.exit_code .. ")"
end

return {
    on_enter = function()
        gramarye.systems.add("global")
        refresh()
    end,

    on_draw = function()
        gramarye.ui.render(ui.Frame {
            id     = "launcher_root",
            layout = { w = { grow = true }, h = { grow = true }, dir = "column",
                       align = { x = "center" }, pad = 32, gap = 16 },

            ui.Title "Launcher",
            ui.Label { text = gramarye.app.is_dev() and "dev build: scripts reload on save, editor has the shader playground" or " ",
                       skin = "label_dim" },

            ui.Panel {
                id     = "mods_panel",
                layout = { dir = "column", w = 640, pad = 16, gap = 10 },
                ui.Label { text = "Mods (load order: higher # wins)", size = 16 },
                ui.Separator {},
                mod_list(),
            },

            ui.Row {
                gap = 12,
                ui.Button { id = "launch_play", label = "Play",   w = 160, on_click = play },
                ui.Button { id = "launch_edit", label = "Edit",   w = 160, on_click = edit },
                ui.Button { id = "launch_quit", label = "Quit",   w = 160, on_click = gramarye.app.quit },
            },
            ui.Label { text = child_text(), skin = "label_dim" },
        })
    end,
}
