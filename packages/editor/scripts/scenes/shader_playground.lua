local ui      = gramarye.require("gramarye.ui")
local widgets = gramarye.require("lib/widgets")
local SP      = gramarye.shader_playground

local KEY_ESCAPE  = gramarye.input.key("escape")
local PREVIEW_KIND = 7
local EDITOR_KIND  = 8

local ACTIVE_BG = { 70, 120, 180, 255 }
local ERROR_BG  = { 120, 40, 40, 255 }

local naming   = false
local new_name = ""
local new_err  = ""

local function back_to_main() gramarye.scene.change(gramarye.app.home()) end

local function create()
    local ok, err = SP.new(new_name)
    if ok then
        naming, new_name, new_err = false, "", ""
        SP.focus_editor(true)
    else
        new_err = err
    end
end

local function new_controls()
    if not naming then
        return ui.Button { id = "sp_new", label = "+ New shader", w = { grow = true },
                           on_click = function() naming, new_err = true, "" end }
    end
    local col = { gap = 6,
        ui.TextBox { id = "sp_new_name", w = { grow = true }, value = new_name, placeholder = "name (click to type)",
                     on_change = function(v) new_name = v end, on_submit = create } }
    if new_err ~= "" then col[#col + 1] = ui.Label { text = new_err, skin = "label_dim" } end
    col[#col + 1] = ui.Row { gap = 6,
                             ui.Button { id = "sp_new_ok", label = "Create", on_click = create },
                             ui.Button { id = "sp_new_cancel", label = "Cancel",
                                         on_click = function() naming = false end } }
    return ui.Column(col)
end

local function side_panel()
    local status, ok = SP.status()
    local panel = { id = "sp_side", bg = { 20, 22, 34, 255 },
                    layout = { dir = "column", w = 230, h = { grow = true }, pad = 12, gap = 6 } }
    local function add(node) if node then panel[#panel + 1] = node end end

    add(ui.Label { text = "dev/shaders/", skin = "label_dim" })
    for i = 1, SP.count() do
        local label = SP.name(i)
        if i == SP.current() and SP.dirty() then label = label .. " *" end
        add(ui.Button { id = "sp_entry_" .. i, label = label, w = { grow = true },
                        bg = (i == SP.current()) and ACTIVE_BG or nil,
                        on_click = function() SP.select(i) end })
    end
    if SP.count() == 0 then add(ui.Label { text = "no .fs files yet" }) end
    add(new_controls())
    add(ui.Separator {})
    add(ui.Row { gap = 6,
                 ui.Button { id = "sp_save", label = "Save", on_click = SP.save },
                 ui.Button { id = "sp_revert", label = "Revert", on_click = SP.revert } })
    add(ui.Button { id = "sp_rescan", label = "Rescan folder", w = { grow = true }, on_click = SP.rescan })
    add(ui.Panel { id = "sp_status", bg = (not ok) and ERROR_BG or nil, pad = 6, gap = 2,
                   ui.Label { text = status ~= "" and status or "pick a shader" } })
    for _, hint in ipairs { "Ctrl+S save, Ctrl+Z/Y undo/redo", "Ctrl+/ comment, Tab/Shift+Tab indent",
                            "Ctrl + / - font size", "Ctrl+Enter recompile now", "click an error to jump to it",
                            "esc: leave editor, again: back" } do
        add(ui.Label { text = hint, skin = "hint" })
    end
    return ui.Panel(panel)
end

return {
    on_enter = function()
        if not SP then return end
        SP.capture_escape(true)
        SP.rescan()
        if SP.current() < 1 and SP.count() > 0 then SP.select(1) end
    end,

    on_exit = function()
        if not SP then return end
        SP.capture_escape(false)
        SP.unload()
    end,

    on_update = function(dt)
        if not SP then
            if gramarye.input.key_pressed(KEY_ESCAPE) then back_to_main() end
            return
        end
        if SP.editor_focused() then naming = false end
        if gramarye.input.key_pressed(KEY_ESCAPE) and not SP.editor_focused() then
            if naming then naming = false else back_to_main() end
        end
    end,

    on_draw = function()
        local body
        if SP then
            local grow = { w = { grow = true }, h = { grow = true } }
            body = ui.Row { id = "sp_body", w = { grow = true }, h = { grow = true }, gap = 2,
                            side_panel(),
                            { _type = "custom", kind = EDITOR_KIND, layout = grow },
                            { _type = "custom", kind = PREVIEW_KIND, layout = grow } }
        else
            body = ui.Label { text = "Shader playground is not built (GRAMARYE_DEV_TOOLS is off)." }
        end
        gramarye.ui.render(ui.Column {
            id = "sp_root", w = { grow = true }, h = { grow = true },
            widgets.HeaderBar {
                title   = "Shader Playground",
                actions = { ui.Button { id = "sp_back", label = "×", w = 36, h = 36, on_click = back_to_main } },
            },
            body,
        })
    end,
}
