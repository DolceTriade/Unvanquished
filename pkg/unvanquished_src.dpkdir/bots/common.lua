local M = {}
local RUNNING_ACTIONS = {}
local constants = require("bots/consts.lua")
local STATUS_FAILURE = constants.STATUS_FAILURE
local STATUS_SUCCESS = constants.STATUS_SUCCESS
local STATUS_RUNNING = constants.STATUS_RUNNING
local cache = require("bots/cache.lua")

M.ALIEN_EVOLVE_TARGETS = {
    "level4",
    "level3upg",
    "level3",
    "level2upg",
    "level2",
    "level1",
}

M.ALIEN_COMBAT_TARGETS = {
    "level4",
    "level3upg",
    "level3",
    "level2upg",
    "level2",
    "level1",
    "level0",
}

local function bot_number(self)
    if type(self) == "number" then
        return self
    end

    return self and self.number or nil
end

function M.is_alien(team)
    return team == "alien" or team == "aliens"
end

function M.is_human(team)
    return team == "human" or team == "humans"
end

function M.maybe_extinguish_fire(team, client, ctx)
    if not M.is_alien(team) or client.class ~= "builderupg" or cache.cvar("g_bot_extinguishFire") == "0" then
        return STATUS_FAILURE
    end

    return ctx:extinguishFire()
end

function M.alive(self)
    local client = self.client
    return client and client.health > 0
end

function M.can_attempt_purchase(client)
    return client ~= nil and (client.credits or 0) >= 200
end

function M.can_attempt_evolve(client)
    return client ~= nil and (client.evos or 0) >= 2
end

function M.queued_for_spawn(self, queued_flag)
    local client = self.client
    return client and (client.pm_flags & queued_flag) ~= 0
end

function M.should_spawn(self, queued_flag)
    return M.queued_for_spawn(self, queued_flag) and not M.alive(self)
end

function M.target_entity(target)
    if target and target.kind == "entity" and target.entity then
        return target.entity
    end

    return nil
end

function M.class_attr(class_name)
    return class_name and Unv.classes[class_name] or nil
end

function M.class_health(class_name)
    local class = M.class_attr(class_name)
    return class and class.health or 100
end

function M.health_fraction(client)
    local max_health = M.class_health(client.class)
    if max_health <= 0 then
        return 1
    end

    return client.health / max_health
end

function M.building_distance(mind, name)
    local target = mind:closestBuilding(name)
    return target and target.distance or nil
end

function M.elapsed_since(now, timestamp)
    if not timestamp or timestamp <= 0 then
        return 0
    end

    local elapsed = now - timestamp
    if elapsed < 0 then
        return 0
    end

    return elapsed
end

function M.spawn_as_team_default(team, ctx, alien_selection, human_selection)
    if M.is_alien(team) then
        return ctx:spawnAs(alien_selection or "level0")
    end

    if M.is_human(team) then
        return ctx:spawnAs(human_selection or "rifle")
    end

    return STATUS_FAILURE
end

function M.use_medkit_if_low(team, client, ctx, threshold)
    threshold = threshold or 50
    if not M.is_human(team) or not client or client.health > threshold then
        return STATUS_FAILURE
    end

    if not client.hasUpgrade or not client:hasUpgrade("medkit") then
        return STATUS_FAILURE
    end

    return ctx:activateUpgrade("medkit")
end

function M.recently_attacked(level, mind, window)
    if not level or not mind then
        return false
    end

    local pain_time = mind.painTime or 0
    if pain_time <= 0 then
        return false
    end

    return (level.time - pain_time) < window
end

function M.heal_to_full_unless_attacked(level, mind, client, ctx, opts)
    opts = opts or {}

    local full_health = opts.full_health or 100
    local attacked_window = opts.attacked_window or 2000
    local heal_anchor = opts.heal_anchor
    local heal_radius = opts.heal_radius or 200

    if not client or client.health >= full_health then
        return STATUS_FAILURE
    end

    if M.recently_attacked(level, mind, attacked_window) then
        return STATUS_FAILURE
    end

    local status = ctx:heal()
    if status ~= STATUS_FAILURE then
        return status
    end

    if heal_anchor then
        status = ctx:roamInRadius(heal_anchor, heal_radius)
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    return STATUS_RUNNING
end

function M.human_repair_target_info(mind, opts)
    opts = opts or {}

    local target = mind.closestDamagedBuilding
    local entity = M.target_entity(target)
    local max_distance = opts.max_distance or 1000
    if not entity or not target.distance or target.distance >= max_distance then
        return nil
    end

    local armoury_name = opts.armoury_name or "arm"
    local armoury_max_distance = opts.armoury_max_distance or 1000
    local armoury = mind:closestBuilding(armoury_name)
    if not armoury or not armoury.distance or armoury.distance >= armoury_max_distance then
        return nil
    end

    local buildable = entity.buildable
    local attr = buildable and Unv.buildables[buildable.name] or nil
    if not buildable or not attr or attr.health <= 0 then
        return nil
    end

    local min_damage_ratio = opts.min_damage_ratio or 0.7
    if buildable.health / attr.health >= min_damage_ratio then
        return nil
    end

    return {
        target = target,
        entity = entity,
        buildable = buildable,
        attr = attr,
    }
end

function M.can_evolve_to_class(self, client, level, class_name)
    if class_name == nil then
        class_name = level
        level = client
        client = self
        self = nil
    end

    local current = M.class_attr(client.class)
    local target = M.class_attr(class_name)
    if not current or not target or client.class == class_name then
        return false
    end

    if client.class == "level3upg" or client.class == "level4" then
        return false
    end

    if target.price <= current.price then
        return false
    end

    return true
end

function M.best_alien_evolve_target(self, client, level, targets)
    for _, class_name in ipairs(targets or M.ALIEN_EVOLVE_TARGETS) do
        if M.can_evolve_to_class(self, client, level, class_name) then
            return class_name
        end
    end

    return nil
end

function M.try_evolve_targets(self, ctx, client, level, targets)
    if targets == nil then
        targets = level
        level = nil
    end

    local candidates = targets or M.ALIEN_EVOLVE_TARGETS
    if not M.can_attempt_evolve(client) then
        return STATUS_FAILURE
    end

    local terminal_candidates = {}
    for _, class_name in ipairs(candidates) do
        if (class_name == "level3upg" or class_name == "level4")
            and M.can_evolve_to_class(self, client, level, class_name)
            and ctx:canEvolveTo(class_name) then
            table.insert(terminal_candidates, class_name)
        end
    end

    if #terminal_candidates > 0 then
        local status = ctx:evolveTo(terminal_candidates[math.random(#terminal_candidates)])
        if status == STATUS_SUCCESS then
            M.buy_overload_upgrades(client, ctx)
        end
        return status
    end

    for _, class_name in ipairs(candidates) do
        if M.can_evolve_to_class(self, client, level, class_name) and ctx:canEvolveTo(class_name) then
            local status = ctx:evolveTo(class_name)
            if status == STATUS_SUCCESS then
                M.buy_overload_upgrades(client, ctx)
            end
            return status
        end
    end

    return STATUS_FAILURE
end

function M.buy_overload_upgrades(client, ctx)
    if not client or not ctx then
        return STATUS_FAILURE
    end

    local spend = math.max(0, math.floor(client.credits or 0))
    if spend <= 0 then
        return STATUS_SUCCESS
    end

    return ctx:buyOverloadUpgrades(spend)
end

local EQUIP_RETRY_COOLDOWN_MS = 5000

function M.equip_and_buy_overload(client, ctx, retry_times, slot, now)
    local retry_at = retry_times and slot and retry_times[slot]
    if now and retry_at and retry_at > now then
        return STATUS_FAILURE
    end

    local status = ctx:equip()
    if status == STATUS_SUCCESS then
        M.buy_overload_upgrades(client, ctx)
    end
    if status == STATUS_SUCCESS and retry_times and slot and now then
        retry_times[slot] = now + EQUIP_RETRY_COOLDOWN_MS
    end
    return status
end

function M.resume_running_action(self, key, fn)
    local number = bot_number(self)
    if not number or RUNNING_ACTIONS[number] ~= key then
        return STATUS_FAILURE
    end

    local status = fn()
    if status ~= STATUS_RUNNING then
        RUNNING_ACTIONS[number] = nil
    end

    return status
end

function M.run_latched_action(self, key, fn)
    local status = fn()
    local number = bot_number(self)
    if not number then
        return status
    end

    if status == STATUS_RUNNING then
        RUNNING_ACTIONS[number] = key
    elseif RUNNING_ACTIONS[number] == key then
        RUNNING_ACTIONS[number] = nil
    end

    return status
end

function M.roam_buildings(ctx, names, radius)
    for _, name in ipairs(names) do
        local status = ctx:roamInRadius(name, radius)
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    return STATUS_FAILURE
end

function M.best_alien_combat_target(self, client, level, targets)
    if targets == nil then
        targets = level
        level = nil
    end

    for _, class_name in ipairs(targets or M.ALIEN_COMBAT_TARGETS) do
        if M.can_evolve_to_class(self, client, level, class_name) then
            return class_name
        end
    end

    return nil
end

return M
