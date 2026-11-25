local base = gramarye.require_base("lib/widgets")
local M    = setmetatable({}, { __index = base })

function M.HeaderBar(props)
    props.bg    = props.bg or { 90, 40, 110, 255 }
    props.title = (props.title or "") .. "  (reskinned)"
    return base.HeaderBar(props)
end

return M
