local PMF_QUEUED = 1 << 12

local common = require("bots/common.lua")

local random = math.random

local M = {}

local GRANGER_STATE = {}
local MISSILE_HOOKS = {}
local REGISTERED_MISSILE_TYPES = {}
local PENDING_SPECS = {}
local BOT_SPECS = {}

local GRANGER_SPAWN_CANDIDATES = {
    "eggpod",
    "acid_tube",
    "trapper",
    "hive",
    "spiker",
}

local function boss_state()
    return {
        flamer = {},
        lucifer = {},
        granger = GRANGER_STATE,
    }
end

local BOSS_STATE = boss_state()

local function pos_str(vec)
    return string.format("{%f,%f,%f}", vec[1] or 0, vec[2] or 0, vec[3] or 0)
end

local function on_granger_spit_impact(missile, hit_ent)
    if not missile or not missile.missile then
        return
    end

    local owner = missile.missile.parent
    if not owner or not owner.number or not GRANGER_STATE[owner.number] then
        return
    end

    local origin = missile.missile.origin
    if not origin then
        return
    end

    if hit_ent
        and owner.team
        and hit_ent.team
        and owner.team ~= "none"
        and hit_ent.team ~= "none"
        and owner.team ~= hit_ent.team then
        hit_ent:kill("MOD_SLOWBLOB", owner)
    end

    local buildable = GRANGER_SPAWN_CANDIDATES[random(#GRANGER_SPAWN_CANDIDATES)]
    sgame.TrySpawnBuildableAt(buildable, origin)
end

local function ensure_missile_hook(missile_type)
    if REGISTERED_MISSILE_TYPES[missile_type] then
        return
    end

    sgame.hooks.RegisterMissileSpawnedHook(function(missile)
        local m = missile and missile.missile
        if not m or m.type ~= missile_type then
            return
        end

        local parent = m.parent
        if not parent or not parent.number then
            return
        end

        local handlers = MISSILE_HOOKS[missile_type]
        if not handlers then
            return
        end

        for _, handler in ipairs(handlers) do
            if handler.owners[parent.number] then
                missile.missile.impact = handler.impact
                return
            end
        end
    end)

    REGISTERED_MISSILE_TYPES[missile_type] = true
end

local function register_missile_handler(missile_type, owners, impact)
    local handlers = MISSILE_HOOKS[missile_type]
    if not handlers then
        handlers = {}
        MISSILE_HOOKS[missile_type] = handlers
    end

    handlers[#handlers + 1] = {
        owners = owners,
        impact = impact,
    }

    ensure_missile_hook(missile_type)
end

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

local function has_all_upgrades(client, upgrades)
    if not upgrades or #upgrades == 0 then
        return true
    end

    if not client or not client.hasUpgrade then
        return false
    end

    for _, upgrade in ipairs(upgrades) do
        if not client:hasUpgrade(upgrade) then
            return false
        end
    end

    return true
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
    local weapon_ready = not spec.weapon or client.weapon == spec.weapon
    local upgrades_ready = armor
        and client:hasUpgrade(armor)
        or has_all_upgrades(client, spec.required_upgrades)
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

local BOSSES = {
    flamer = {
        behavior = "boss_flamer.lua",
        team = "humans",
        spawn = "rifle",
        weapon = "flamer",
        armor_options = { "bsuit", "marmour", "larmour" },
        spawn_credits = 2000,
        damage_dealt_multiplier = 3.0,
        damage_received_multiplier = 0.35,
        ignore_self_damage = true,
        state_store = BOSS_STATE.flamer,
        on_load = function()
            Cmd.exec("set g_bot_flamer 1")
        end,
        on_init = function()
            sgame.overload.force_unlock("humans", "weapon", "flamer")
        end,
    },
    granger = {
        behavior = "boss_granger.lua",
        team = "aliens",
        class = "builderupg",
        damage_dealt_multiplier = 10.0,
        damage_received_multiplier = 0.15,
        startup_delay_ms = 200,
        state_store = BOSS_STATE.granger,
        on_load = function(spec)
            register_missile_handler("slowblob", spec.state_store, on_granger_spit_impact)
        end,
    },
}

local function ensure_spec_loaded(spec)
    if spec.on_load and not spec.loaded then
        spec.on_load(spec)
        spec.loaded = true
    end
end

local function get_base_boss(id)
    local spec = BOSSES[id]
    if not spec then
        error("unknown boss: " .. tostring(id))
    end

    ensure_spec_loaded(spec)
    return spec
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

local function merge_spec(spec, overrides)
    local merged = copy_shallow(spec)
    for key, value in pairs(overrides or {}) do
        merged[key] = value
    end
    merged.loaded = nil
    return merged
end

local function enqueue_pending_spec(id, entry)
    local pending = PENDING_SPECS[id]
    if not pending then
        pending = {}
        PENDING_SPECS[id] = pending
    end

    pending[#pending + 1] = entry
end

local function find_pending_spec(id, self)
    local pending = PENDING_SPECS[id]
    if not pending or #pending == 0 then
        return nil
    end

    local client = self.client
    local bot_name = client and client.clean_name or nil
    local wildcard_index = nil

    for index, entry in ipairs(pending) do
        if not entry.name then
            if not wildcard_index then
                wildcard_index = index
            end
        elseif bot_name and entry.name == bot_name then
            table.remove(pending, index)
            if #pending == 0 then
                PENDING_SPECS[id] = nil
            end
            return entry.spec
        end
    end

    if wildcard_index then
        local entry = table.remove(pending, wildcard_index)
        if #pending == 0 then
            PENDING_SPECS[id] = nil
        end
        return entry.spec
    end

    return nil
end

local function resolve_boss_spec(id, self)
    local number = self.number
    local spec = BOT_SPECS[number]
    if spec then
        return spec
    end

    spec = find_pending_spec(id, self) or get_base_boss(id)
    ensure_spec_loaded(spec)
    BOT_SPECS[number] = spec
    return spec
end

function M.add(id, opts)
    local base = get_base_boss(id)
    opts = opts or {}

    local name = opts.name or "*"
    local skill = tonumber(opts.skill) or 9
    local spec = merge_spec(base, spec_overrides(opts))
    enqueue_pending_spec(id, {
        name = name ~= "*" and name or nil,
        spec = spec,
    })
    Cmd.exec(("bot add %s %s %d %s"):format(name, spec.team, skill, spec.behavior))
end

function M.prepare(id, self, ctx)
    local spec = resolve_boss_spec(id, self)
    local state_store = spec.state_store

    if common.should_spawn(self, PMF_QUEUED) then
        state_store[self.number] = nil

        return ctx:spawnAs(spec.spawn or spec.class)
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

        if spec.damage_dealt_multiplier ~= nil then
            client.damage_dealt_multiplier = spec.damage_dealt_multiplier
        end

        if spec.damage_received_multiplier ~= nil then
            client.damage_received_multiplier = spec.damage_received_multiplier
        end

        if spec.ignore_self_damage ~= nil then
            client.ignore_self_damage = spec.ignore_self_damage
        end

        if spec.on_init then
            spec.on_init(self, ctx)
        end

        if spec.startup_delay_ms and spec.startup_delay_ms > 0 then
            state.ready_at = sgame.level.time + spec.startup_delay_ms
        end

        self.die = function(ent, inflictor, attacker, mod)
            state_store[ent.number] = nil
            BOT_SPECS[ent.number] = nil

            if spec.on_die then
                spec.on_die(ent, inflictor, attacker, mod)
            end

            Timer.add(1, function() Cmd.exec("bot del " .. ent.number) end)
            return true
        end
    end

    if state.ready_at and sgame.level.time < state.ready_at then
        return STATUS_RUNNING
    end

    if common.is_human(spec.team) and not state.loadout_ready then
        local status = ensure_human_loadout(spec, self, ctx, state)
        if status ~= nil then
            return status
        end
    end

    return nil
end

return M
