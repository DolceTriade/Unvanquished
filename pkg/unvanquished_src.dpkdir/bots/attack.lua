STATUS_FAILURE = 0
STATUS_SUCCESS = 1
STATUS_RUNNING = 2

local PMF_QUEUED = 1 << 12

local common = require("bots/common.lua")
local weapons = Unv.weapons
local is_alien = common.is_alien
local is_human = common.is_human
local should_spawn = common.should_spawn
local target_entity = common.target_entity
local building_distance = common.building_distance
local health_fraction = common.health_fraction
local use_medkit_if_low = common.use_medkit_if_low
local try_evolve_targets = common.try_evolve_targets
local resume_running_action = common.resume_running_action
local run_latched_action = common.run_latched_action
local roam_buildings = common.roam_buildings
local unstick = common.unstick

local function target_is_buildable(target)
    local entity = target_entity(target)
    return entity and entity.buildable ~= nil or false
end

local function min_distance(...)
    local best = nil
    for i = 1, select("#", ...) do
        local distance = select(i, ...)
        if distance and (best == nil or distance < best) then
            best = distance
        end
    end
    return best
end

local function maybe_use_medkit(team, client, ctx)
    return use_medkit_if_low(team, client, ctx, 50)
end

local function has_human_ammo(client, weapon_attr)
    if not weapon_attr or weapon_attr.ammo <= 0 then
        return true
    end

    return (client.ammo or 0) > 0
end

local function out_of_human_ammo(client, weapon_attr)
    if not weapon_attr or weapon_attr.ammo <= 0 then
        return false
    end

    return (client.ammo or 0) <= 0
end

local function maybe_throw_explosive(client, mind, ctx)
    local near_friendly = min_distance(
        building_distance(mind, "reactor"),
        building_distance(mind, "arm"),
        building_distance(mind, "medistat"),
        building_distance(mind, "telenode")
    )
    local goal = mind.goal
    local close_enemy_building = goal.distance and goal.distance < 400 and target_is_buildable(goal)

    if health_fraction(client) >= 0.3 and not close_enemy_building then
        return STATUS_FAILURE
    end

    if near_friendly and near_friendly <= 400 then
        return STATUS_FAILURE
    end

    local status = ctx:activateUpgrade("firebomb")
    if status ~= STATUS_FAILURE then
        return status
    end

    return ctx:activateUpgrade("gren")
end

local function maybe_reload(team, weapon_attr, enemy_visible, client, ctx)
    if not is_human(team) or enemy_visible or not weapon_attr or weapon_attr.ammo <= 0 then
        return STATUS_FAILURE
    end

    if (client.ammo or 0) / weapon_attr.ammo >= 0.4 then
        return STATUS_FAILURE
    end

    return ctx:reload()
end

local function maybe_human_attack(client, weapon_attr, enemy_visible, mind, ctx)
    if not enemy_visible then
        return STATUS_FAILURE
    end

    if has_human_ammo(client, weapon_attr) then
        local status = maybe_throw_explosive(client, mind, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        return ctx:fight()
    end

    return ctx:fight()
end

local function continue_human_resupply(self, ctx)
    local status = resume_running_action(self, "attack_human_equip", function()
        return ctx:equip()
    end)
    if status ~= STATUS_FAILURE then
        return status
    end

    return resume_running_action(self, "attack_human_move_to_arm", function()
        return ctx:moveTo("arm")
    end)
end

local function maybe_human_resupply(self, client, weapon_attr, mind, ctx)
    local armoury_distance = building_distance(mind, "arm")

    if armoury_distance and armoury_distance < 500 then
        local status = run_latched_action(self, "attack_human_equip", function()
            return ctx:equip()
        end)
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    if out_of_human_ammo(client, weapon_attr) then
        return run_latched_action(self, "attack_human_move_to_arm", function()
            return ctx:moveTo("arm")
        end)
    end

    return STATUS_FAILURE
end

local ALIEN_EVOLVE_TARGETS = common.ALIEN_EVOLVE_TARGETS

local function maybe_evolve(self, team, client, enemy_visible, ctx)
    if not is_alien(team) then
        return STATUS_FAILURE
    end

    if enemy_visible then
        return STATUS_FAILURE
    end

    return try_evolve_targets(self, ctx, client, ALIEN_EVOLVE_TARGETS)
end

local function maybe_rush(ctx)
    return ctx:rush()
end

local function roam_enemy_base(team, ctx)
    if is_human(team) then
        local status = roam_buildings(ctx, { "overmind", "eggpod" }, 500)
        return status ~= STATUS_FAILURE and status or ctx:roam()
    end

    if is_alien(team) then
        local status = roam_buildings(ctx, { "reactor", "telenode" }, 500)
        return status ~= STATUS_FAILURE and status or ctx:roam()
    end

    return STATUS_FAILURE
end

return function(self, ctx)
    if should_spawn(self, PMF_QUEUED) then
        return common.spawn_as_team_default(self.team, ctx, "level0", "rifle")
    end

    local client = self.client
    local bot = self.bot
    local mind = bot and bot.mind or nil
    if not client or not mind then
        return ctx:roam()
    end

    local enemy_target = mind.bestEnemy
    local enemy = target_entity(enemy_target)
    local enemy_visible = enemy and ctx:isVisibleEntity(enemy) or false
    local weapon_attr = weapons[client.weapon]

    mind.stuckTimer = sgame.level.time

    local status = unstick(sgame.level.time, ctx, mind, enemy, enemy_visible)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = maybe_use_medkit(self.team, client, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    if is_human(self.team) then
        status = continue_human_resupply(self, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_human_attack(client, weapon_attr, enemy_visible, mind, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_reload(self.team, weapon_attr, enemy_visible, client, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_human_resupply(self, client, weapon_attr, mind, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        if enemy_visible then
            return ctx:fight()
        end
    end

    if is_alien(self.team) then
        status = maybe_evolve(self, self.team, client, enemy_visible, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        if enemy_visible then
            return ctx:fight()
        end
    end

    status = maybe_rush(ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    return roam_enemy_base(self.team, ctx)
end
