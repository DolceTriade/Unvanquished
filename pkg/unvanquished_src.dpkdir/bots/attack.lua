STATUS_FAILURE = 0
STATUS_SUCCESS = 1
STATUS_RUNNING = 2

local PMF_QUEUED = 1 << 12

local cache = require("bots/cache.lua")
local common = require("bots/common.lua")
local task_runtime = require("bots/task.lua")
local weapons = Unv.weapons
local is_alien = common.is_alien
local is_human = common.is_human
local should_spawn = common.should_spawn
local target_entity = common.target_entity
local building_distance = common.building_distance
local health_fraction = common.health_fraction
local use_medkit_if_low = common.use_medkit_if_low
local try_evolve_targets = common.try_evolve_targets
local roam_buildings = common.roam_buildings
local unstick = common.unstick
local TASKS = task_runtime.new_runtime()
local STATE = {
    human_equip_retry_at = {},
}

local function target_is_buildable(target)
    local entity = target_entity(target)
    return entity and entity.buildable ~= nil or false
end

local function is_hostile_target(team, target)
    if not target or target.kind ~= "entity" then
        return false
    end

    local entity = target_entity(target)
    local target_team = entity and entity.team or target.team
    if not target_team then
        return false
    end

    if is_human(team) then
        return is_alien(target_team)
    end

    if is_alien(team) then
        return is_human(target_team)
    end

    return false
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

    if client.hasUpgrade and client:hasUpgrade("firebomb") then
        local status = ctx:activateUpgrade("firebomb")
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    if client.hasUpgrade and client:hasUpgrade("gren") then
        return ctx:activateUpgrade("gren")
    end

    return STATUS_FAILURE
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

local function attack_alert_active(state)
    if state.enemy ~= nil or state.hostile_goal then
        return true
    end

    if is_human(state.team) then
        return false
    end

    local enemy_last_seen = state.mind.enemyLastSeen
    if not enemy_last_seen or enemy_last_seen <= 0 then
        return false
    end

    return common.elapsed_since(state.level.time, enemy_last_seen) < cache.cvar_number("g_bot_chasetime")
end

local function maybe_human_attack(client, weapon_attr, alert_active, enemy_visible, mind, ctx)
    if not alert_active then
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

local function human_armoury_distance(mind)
    return building_distance(mind, "arm")
end

local function maybe_equip_human_near_armoury(number, team, level, mind, ctx)
    if not is_human(team) then
        return STATUS_FAILURE
    end

    local retry_at = STATE.human_equip_retry_at[number]
    if retry_at and retry_at > level.time then
        return STATUS_FAILURE
    end

    local armoury_distance = human_armoury_distance(mind)
    if not armoury_distance or armoury_distance >= 500 then
        return STATUS_FAILURE
    end

    local status = ctx:equip()
    if status == STATUS_FAILURE then
        STATE.human_equip_retry_at[number] = level.time + 2000
        return STATUS_FAILURE
    end

    STATE.human_equip_retry_at[number] = nil
    return status
end

local function human_resupply_phase(client, weapon_attr, mind)
    if out_of_human_ammo(client, weapon_attr) then
        local armoury_distance = human_armoury_distance(mind)
        if armoury_distance and armoury_distance < 500 then
            return "equip"
        end

        return "move_to_arm"
    end

    return nil
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

local COMBAT_TASK = {
    run = function(_, state, ctx)
        if not attack_alert_active(state) then
            return STATUS_FAILURE
        end

        if is_human(state.team) then
            return maybe_human_attack(state.client, state.weapon_attr,
                attack_alert_active(state), state.enemy_visible, state.mind, ctx)
        end

        return ctx:fight()
    end,
}

local RESUPPLY_TASK = {
    should_preempt = function(_, state)
        return attack_alert_active(state)
    end,
    preempt_cooldown_ms = 1000,
    run = function(task, state, ctx)
        if not is_human(state.team) then
            return STATUS_FAILURE
        end

        task.phase = task.phase or human_resupply_phase(state.client, state.weapon_attr, state.mind)
        if task.phase == nil then
            return STATUS_FAILURE
        end

        if task.phase == "move_to_arm" then
            if building_distance(state.mind, "arm") and building_distance(state.mind, "arm") < 500 then
                task.phase = "equip"
            else
                local status = ctx:moveTo("arm")
                if status ~= STATUS_FAILURE then
                    return status
                end

                task.phase = human_resupply_phase(state.client, state.weapon_attr, state.mind)
                return task.phase and STATUS_RUNNING or STATUS_FAILURE
            end
        end

        if task.phase == "equip" then
            local status = ctx:equip()
            if status ~= STATUS_FAILURE then
                return status
            end

            task.phase = human_resupply_phase(state.client, state.weapon_attr, state.mind)
            return task.phase and STATUS_RUNNING or STATUS_FAILURE
        end

        return STATUS_FAILURE
    end,
}

local ASSAULT_TASK = {
    should_preempt = function(_, state)
        return attack_alert_active(state)
    end,
    run = function(_, state, ctx)
        local status = nil

        if is_human(state.team) then
            status = maybe_equip_human_near_armoury(state.number, state.team, state.level, state.mind, ctx)
            if status ~= STATUS_FAILURE then
                return status
            end

            status = maybe_reload(state.team, state.weapon_attr, state.enemy_visible, state.client, ctx)
            if status ~= STATUS_FAILURE then
                return status
            end
        else
            status = maybe_evolve(state.self, state.team, state.client, state.enemy_visible, ctx)
            if status ~= STATUS_FAILURE then
                return status
            end
        end

        status = maybe_rush(ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        return roam_enemy_base(state.team, ctx)
    end,
}

local function select_task(state)
    if attack_alert_active(state) then
        return COMBAT_TASK
    end

    if is_human(state.team) and human_resupply_phase(state.client, state.weapon_attr, state.mind) then
        return RESUPPLY_TASK
    end

    return ASSAULT_TASK
end

return function(self, ctx)
    cache.refresh()

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
    local hostile_goal = is_hostile_target(self.team, mind.goal)
    local weapon_attr = weapons[client.weapon]
    local state = {
        self = self,
        team = self.team,
        number = self.number,
        level = sgame.level,
        client = client,
        mind = mind,
        enemy = enemy,
        enemy_visible = enemy_visible,
        hostile_goal = hostile_goal,
        weapon_attr = weapon_attr,
    }

    mind.stuckTimer = sgame.level.time

    local status = unstick(sgame.level.time, ctx, mind, enemy, enemy_visible)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = maybe_use_medkit(self.team, client, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    local task = TASKS.current(self.number)
    if task then
        status = TASKS.maybe_preempt(state, ctx, select_task)
        if status ~= STATUS_FAILURE then
            return status
        end

        task = TASKS.current(self.number)
        if task then
            status = TASKS.run(task, state, ctx)
            if status ~= STATUS_FAILURE then
                return status
            end
        end
    end

    task = TASKS.start(self.number, select_task(state))
    return TASKS.run(task, state, ctx)
end
