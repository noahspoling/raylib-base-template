local ui      = gramarye.require("gramarye.ui")
local widgets = gramarye.require("lib/widgets")

local GLOBE_KIND       = 1
local CONTROLS_KIND    = 2
local CHUNK_DEBUG_KIND = 3
local KEY_ESCAPE       = gramarye.input.key("escape")
local W  = gramarye.world
local LC = gramarye.local_chunk

local function back_to_main() gramarye.scene.change("main") end

local VIEW_NAMES = { "Terrain", "Temperature", "Rainfall", "Flow", "Region",
                     "Wind", "Elevation", "Moisture", "Plates" }

local FAULT_NAMES = { [1] = "Convergent", [2] = "Divergent", [3] = "Transform" }

local function on_regen()  if not W.generating() then W.regenerate() end end
local function on_algo()   W.next_algorithm() end
local function on_hydro()  W.set_hydrology(not W.hydrology()) end
local function on_view()   W.set_view((W.view() + 1) % #VIEW_NAMES) end

local seed_text = nil

local function on_seed_change(s)
    s = s:gsub("%D", "")
    seed_text = s
    if s ~= "" then W.set_seed(tonumber(s)) end
end

local function on_new_seed()
    W.reroll()
    seed_text = tostring(W.seed())
end

local chunk_preview_cell = nil

local function on_preview_chunk()
    local sel = W.selection()
    if not sel then return end
    if LC.generate(sel.cell) then chunk_preview_cell = sel.cell end
end

local function on_close_chunk_preview()
    LC.clear()
    chunk_preview_cell = nil
end

local function control_panel()
    local dirty = W.dirty()
    local busy  = W.generating()

    local regen_label = busy and "Generating..."
                     or (dirty and "Apply changes" or "Regenerate")

    local kids = {
        ui.Title "World",
        ui.Separator {},

        ui.Label { text = "Algorithm: " .. W.algorithm() },
        ui.Label { text = "Resolution: L" .. W.level() .. "  (" .. W.cells() .. " tiles)" },

        ui.Label { text = "Seed (staged, then Apply)", skin = "label_dim" },
        ui.Row {
            w = { grow = true }, gap = 8, align = { y = "center" },
            ui.TextBox { id = "tb_seed", w = { grow = true },
                         value = seed_text or tostring(W.seed()),
                         on_change = on_seed_change },
            ui.Button { id = "btn_new_seed", label = "New seed", on_click = on_new_seed },
        },

        ui.Label { text = "Generation", skin = "label_dim" },
        ui.Button { id = "btn_regen",
                    label = regen_label,
                    bg    = (dirty and not busy) and { 70, 120, 180, 255 } or nil,
                    on_click = on_regen },
        ui.Button { id = "btn_algo",   label = "Next algorithm", on_click = on_algo   },

        ui.Button { id = "btn_view", label = "View: " .. VIEW_NAMES[W.view() + 1],
                    on_click = on_view },

        ui.Separator {},
        ui.Label { text = "Environment (adjust, then Regenerate)", skin = "label_dim" },

        widgets.Checkbox { id = "cb_hydro", label = "Rivers & lakes (hydrology)",
                           checked = W.hydrology(), on_click = on_hydro },

        { _type = "custom", kind = CONTROLS_KIND,
          layout = { w = { grow = true }, h = 350 } },
    }

    if not W.selection() then
        kids[#kids + 1] = ui.Separator {}
        kids[#kids + 1] = ui.Label { text = "click a tile to inspect", skin = "hint" }
    end

    local avail = gramarye.ui.screen_h() - 52
    local scroll = { id = "planet_controls",
                     bg = { 20, 22, 34, 255 },
                     v_scroll = true,
                     layout = { dir = "column", w = 340, h = avail, pad = 16, gap = 8 } }
    for i, v in ipairs(kids) do scroll[i] = v end
    return ui.ScrollFrame(scroll)
end

local function neighbor_row(n, sel)
    return ui.Label { skin = "label_dim",
        text = string.format("  %d: %s  elev %+.2f", n, sel.terrain, sel.elevation) }
end

local function tile_popup()
    local sel = W.selection()
    if not sel then return nil end
    local pos = W.screen_pos()
    if not pos then return nil end

    local kids = {
        ui.Label { text = "Terrain: " .. sel.terrain },
        ui.Label { skin = "label_dim",
            text = string.format("Cell #%d  (%d neighbors)", sel.cell, sel.neighbors) },
        ui.Label { skin = "label_dim",
            text = string.format("Elevation  %+.2f", sel.elevation) },
        ui.Label { skin = "label_dim",
            text = string.format("Temp %.2f   Humidity %.2f", sel.temperature, sel.humidity) },
        ui.Label { skin = "label_dim",
            text = string.format("Rainfall %.2f   Region %d", sel.rainfall, sel.region) },
        ui.Label { skin = "label_dim", text = string.format("Plate %d", sel.plate) },
    }
    if sel.fault and sel.fault > 0 then
        kids[#kids + 1] = ui.Label { skin = "label_dim",
            text = string.format("Fault: %s (stress %.2f)", FAULT_NAMES[sel.fault] or "?", sel.stress) }
    end
    if sel.river and sel.river > 0 then
        kids[#kids + 1] = ui.Label { skin = "label_dim",
            text = string.format("River (width %d, flow %.1f)", sel.river, sel.flow) }
    elseif sel.flow and sel.flow > 0 then
        kids[#kids + 1] = ui.Label { skin = "label_dim", text = string.format("Water flow %.1f", sel.flow) }
    end
    kids[#kids + 1] = ui.Label { skin = "label_dim", text = string.format("Lat %.1f   Lon %.1f", sel.lat, sel.lon) }

    kids[#kids + 1] = ui.Separator {}
    kids[#kids + 1] = ui.Label { text = "Neighbors", skin = "label_dim" }
    for i = 1, W.neighbor_count() do
        local n = W.neighbor(i)
        if n then kids[#kids + 1] = neighbor_row(i, n) end
    end

    kids[#kids + 1] = ui.Separator {}
    kids[#kids + 1] = ui.Button { id = "btn_preview_chunk", label = "Preview local chunk",
                                   on_click = on_preview_chunk }

    local popup = { id = "tile_popup_" .. sel.cell, title = "Tile", x = pos.x, y = pos.y, w = 260,
                     on_close = function() W.clear_selection(); on_close_chunk_preview() end }
    for i, v in ipairs(kids) do popup[i] = v end
    return ui.Popup(popup)
end

local CHUNK_PREVIEW_SIZE = 420

local function chunk_preview_panel()
    if not chunk_preview_cell then return nil end
    return ui.Popup {
        id = "chunk_preview", title = "Local map #" .. chunk_preview_cell,
        x = 24, y = gramarye.ui.screen_h() - (CHUNK_PREVIEW_SIZE + 60),
        w = CHUNK_PREVIEW_SIZE + 24,
        on_close = on_close_chunk_preview,
        { _type = "custom", kind = CHUNK_DEBUG_KIND,
          layout = { w = CHUNK_PREVIEW_SIZE, h = CHUNK_PREVIEW_SIZE } },
    }
end

local function mouse_over_overlays()
    local mx, my = gramarye.ui.mouse_pos()
    local function over(id)
        local rx, ry, rw, rh = gramarye.ui.element_rect(id)
        return rw > 0 and rh > 0 and mx >= rx and mx <= rx + rw and my >= ry and my <= ry + rh
    end
    local sel = W.selection()
    if sel and over("tile_popup_" .. sel.cell) then return true end
    if chunk_preview_cell and over("chunk_preview") then return true end
    return false
end

return {
    on_enter = function() gramarye.systems.add("global") end,

    on_update = function(dt)
        if gramarye.input.key_pressed(KEY_ESCAPE) then back_to_main() end
    end,

    on_draw = function()
        W.set_input_suppressed(mouse_over_overlays())

        gramarye.ui.render(ui.Column {
            id = "planet_root",
            w  = { grow = true },
            h  = { grow = true },

            widgets.HeaderBar {
                title   = "Planet",
                actions = {
                    ui.Button { id = "planet_back", label = "×", w = 36, h = 36,
                                on_click = back_to_main },
                },
            },

            ui.Row {
                id = "planet_body",
                w  = { grow = true },
                h  = { grow = true },

                { _type = "custom", kind = GLOBE_KIND,
                  layout = { w = { grow = true }, h = { grow = true } } },

                control_panel(),
            },
        })

        local popup = tile_popup()
        if popup then gramarye.ui.render(popup) end

        local preview = chunk_preview_panel()
        if preview then gramarye.ui.render(preview) end
    end,
}
