-- Behaviors bind these tables once when loaded and use direct local references
-- in their think loops.
local M = {}
local BEHAVIORS = {}

function M.new_behavior(name, initial, per_slot_fields)
    BEHAVIORS[name] = {
        values = initial,
        per_slot_fields = per_slot_fields or {},
    }
    return initial
end

function M.clear_slot(name, slot)
    local behavior = BEHAVIORS[name]
    if not behavior then return end
    for _, field in ipairs(behavior.per_slot_fields) do
        local values = behavior.values[field]
        if values then values[slot] = nil end
    end
end

function M.reset_behavior(name)
    BEHAVIORS[name] = nil
end

return M
