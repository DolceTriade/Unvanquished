local PMF_QUEUED = 1 << 12

local common = require("bots/common.lua")

local M = {}
local CONTROLLERS = {}
local PENDING_ADDS = {}

local function copy_shallow(src)
    local dst = {}
    if not src then
        return dst
    end

    for key, value in pairs(src) do
        dst[key] = value
    end

    return dst
end

local function missing_upgrades(client, upgrades)
    local missing = {}
    for _, upgrade in ipairs(upgrades or {}) do
        if not client:hasUpgrade(upgrade) then
            missing[#missing + 1] = upgrade
        end
    end
    return missing
end

local function selected_armor(spec, client, state)
    if not spec.armor_options or #spec.armor_options == 0 then
        return nil
    end

    if state.armor_upgrade and client:hasUpgrade(state.armor_upgrade) then
        return state.armor_upgrade
    end

    for _, armor in ipairs(spec.armor_options) do
        if client:hasUpgrade(armor) then
            state.armor_upgrade = armor
            return armor
        end
    end

    local attempt = state.armor_attempt or 1
    return spec.armor_options[attempt]
end

local function ensure_human_loadout(spec, self, ctx, state)
    local client = self.client
    local mind = self.bot and self.bot.mind or nil
    if not client or not mind then
        return STATUS_RUNNING
    end

    if not state.spawn_credits_granted then
        client.credits = math.max(client.credits or 0, spec.spawn_credits or 0)
        state.spawn_credits_granted = true
    end

    local armor = selected_armor(spec, client, state)
    local required_upgrades = missing_upgrades(client, spec.required_upgrades)
    local weapon_ready = not spec.weapon or client.weapon == spec.weapon
    local upgrades_ready = armor
        and client:hasUpgrade(armor)
        or #required_upgrades == 0
    if weapon_ready and upgrades_ready then
        state.loadout_ready = true
        return nil
    end

    local armoury = mind:closestBuilding("arm")
    if not armoury or not armoury.distance then
        return ctx:roam()
    end

    if armoury.distance > 100 then
        local status = ctx:moveTo("arm")
        if status ~= STATUS_FAILURE then
            return STATUS_RUNNING
        end
        return STATUS_RUNNING
    end

    if spec.weapon and armor then
        local status = ctx:buy(spec.weapon, armor)
        if status ~= STATUS_FAILURE then
            return STATUS_RUNNING
        end
        state.armor_attempt = (state.armor_attempt or 1) + 1
        if state.armor_attempt > #spec.armor_options then
            state.armor_attempt = #spec.armor_options
        end
    elseif spec.weapon and #required_upgrades > 0 then
        local args = { spec.weapon }
        for _, upgrade in ipairs(required_upgrades) do
            args[#args + 1] = upgrade
        end
        local status = ctx:buy(table.unpack(args, 1, 4))
        if status ~= STATUS_FAILURE then
            return STATUS_RUNNING
        end
    elseif spec.weapon then
        local status = ctx:buyPrimary(spec.weapon)
        if status ~= STATUS_FAILURE then
            return STATUS_RUNNING
        end
    else
        local status = ctx:equip()
        if status ~= STATUS_FAILURE then
            return STATUS_RUNNING
        end
    end

    return STATUS_RUNNING
end

local function spec_overrides(opts)
    local overrides = {}
    if not opts then
        return overrides
    end

    for key, value in pairs(opts) do
        if key ~= "name" and key ~= "skill" then
            overrides[key] = value
        end
    end

    return overrides
end

local function new(spec)
    assert(type(spec) == "table", "boss.new requires a specification")
    assert(spec.behavior, "boss specification requires behavior")
    assert(spec.team, "boss specification requires team")

    local state_store = {}
    local pending_specs = {}
    local bot_specs = {}
    local missile_handlers = {}
    local registered_missile_types = {}
    local controller = {}

    local function merge_spec(overrides)
        local merged = copy_shallow(spec)
        for key, value in pairs(overrides or {}) do
            merged[key] = value
        end
        return merged
    end

    local function find_pending_spec(self)
        if #pending_specs == 0 then
            return nil
        end

        local client = self.client
        local bot_name = client and client.clean_name or nil
        local wildcard_index = nil

        for index, entry in ipairs(pending_specs) do
            if not entry.name then
                if not wildcard_index then
                    wildcard_index = index
                end
            elseif bot_name and entry.name == bot_name then
                table.remove(pending_specs, index)
                return entry.spec
            end
        end

        if wildcard_index then
            local entry = table.remove(pending_specs, wildcard_index)
            return entry.spec
        end

        return nil
    end

    local function resolve_spec(self)
        local number = self.number
        local resolved = bot_specs[number]
        if resolved then
            return resolved
        end

        resolved = find_pending_spec(self) or spec
        bot_specs[number] = resolved
        return resolved
    end

    local function ensure_missile_hook(missile_type)
        if registered_missile_types[missile_type] then
            return
        end

        sgame.hooks.RegisterMissileSpawnedHook(function(missile)
            local m = missile and missile.missile
            if not m or m.type ~= missile_type then
                return
            end

            local parent = m.parent
            if not parent or not parent.number or not state_store[parent.number] then
                return
            end

            for _, impact in ipairs(missile_handlers[missile_type] or {}) do
                missile.missile.impact = impact
                return
            end
        end)

        registered_missile_types[missile_type] = true
    end

    function controller:registerMissileHandler(missile_type, impact)
        local handlers = missile_handlers[missile_type]
        if not handlers then
            handlers = {}
            missile_handlers[missile_type] = handlers
        end

        handlers[#handlers + 1] = impact
        ensure_missile_hook(missile_type)
    end

    local function queue_add(opts)
        opts = opts or {}
        local name = opts.name or "*"
        local added_spec = merge_spec(spec_overrides(opts))
        pending_specs[#pending_specs + 1] = {
            name = name ~= "*" and name or nil,
            spec = added_spec,
        }
        return name, added_spec
    end

    function controller:queue(opts)
        queue_add(opts)
    end

    function controller:add(opts)
        opts = opts or {}
        local skill = tonumber(opts.skill) or 9
        local name, added_spec = queue_add(opts)
        local name_arg = name == "*" and name or ("%q"):format(name)
        Cmd.exec(("bot add %s %s %d %s"):format(name_arg, added_spec.team, skill, added_spec.behavior))
    end

    function controller:prepare(self, ctx)
        local active_spec = resolve_spec(self)

        if common.should_spawn(self, PMF_QUEUED) then
            state_store[self.number] = nil
            return ctx:spawnAs(active_spec.spawn or active_spec.class)
        end

        local client = self.client
        if not client then
            return nil
        end
        client.notarget = true

        local state = state_store[self.number]
        if not state then
            state = {}
            state_store[self.number] = state

            if active_spec.damage_dealt_multiplier ~= nil then
                client.damage_dealt_multiplier = active_spec.damage_dealt_multiplier
            end

            if active_spec.damage_received_multiplier ~= nil then
                client.damage_received_multiplier = active_spec.damage_received_multiplier
            end

            if active_spec.ignore_self_damage ~= nil then
                client.ignore_self_damage = active_spec.ignore_self_damage
            end

            if active_spec.on_init then
                active_spec.on_init(self, ctx, state)
            end

            if active_spec.startup_delay_ms and active_spec.startup_delay_ms > 0 then
                state.ready_at = sgame.level.time + active_spec.startup_delay_ms
            end

            self.die = function(ent, inflictor, attacker, mod)
                state_store[ent.number] = nil
                bot_specs[ent.number] = nil

                if active_spec.on_die then
                    active_spec.on_die(ent, inflictor, attacker, mod)
                end

                Timer.add(1, function() Cmd.exec("bot del " .. ent.number) end)
                return true
            end
        end

        if state.ready_at and sgame.level.time < state.ready_at then
            return STATUS_RUNNING
        end

        if common.is_human(active_spec.team) and not state.loadout_ready then
            local status = ensure_human_loadout(active_spec, self, ctx, state)
            if status ~= nil then
                return status
            end
        end

        return nil
    end

    if spec.on_load then
        spec.on_load(controller, spec)
    end

    return controller
end

M.new = new

function M.register(id, controller)
    CONTROLLERS[id] = controller
    local pending = PENDING_ADDS[id]
    if pending then
        for _, opts in ipairs(pending) do
            controller:queue(opts)
        end
        PENDING_ADDS[id] = nil
    end
end

function M.add(id, opts)
    local controller = CONTROLLERS[id]
    if controller then
        return controller:add(opts)
    end

    opts = opts or {}
    assert(opts.team, "boss.add requires team until the boss behavior is loaded")
    assert(opts.behavior, "boss.add requires behavior until the boss behavior is loaded")

    local pending = PENDING_ADDS[id]
    if not pending then
        pending = {}
        PENDING_ADDS[id] = pending
    end
    pending[#pending + 1] = copy_shallow(opts)

    local name = opts.name or "*"
    local skill = tonumber(opts.skill) or 9
    local name_arg = name == "*" and name or ("%q"):format(name)
    Cmd.exec(("bot add %s %s %d %s"):format(name_arg, opts.team, skill, opts.behavior))
end

return M
