STATUS_FAILURE = 0
STATUS_SUCCESS = 1
STATUS_RUNNING = 2

local PMF_QUEUED = 1 << 12

package.loaded["bots/common.lua"] = nil

local cache = require("bots/cache.lua")
local common = require("bots/common.lua")
local task_runtime = require("bots/task.lua")
local weapons = Unv.weapons
local elapsed_since = common.elapsed_since
local is_alien = common.is_alien
local is_human = common.is_human
local should_spawn = common.should_spawn
local target_entity = common.target_entity
local building_distance = common.building_distance
local health_fraction = common.health_fraction
local use_medkit_if_low = common.use_medkit_if_low
local try_evolve_targets = common.try_evolve_targets
local human_repair_target_info = common.human_repair_target_info
local unstick = common.unstick

local STATE = {
    alien_builder_owner = nil,
}
local TASKS = task_runtime.new_runtime()

local BUILDER_ANCHOR_RADIUS = 700
local ALIEN_EVOLVE_TARGETS = common.ALIEN_EVOLVE_TARGETS
local ALIEN_COMBAT_TARGETS = common.ALIEN_COMBAT_TARGETS

local function entity_ref(entity)
    if not entity then
        return nil
    end

    return {
        number = entity.number,
        generation = entity.generation,
    }
end

local function resolve_entity_ref(ref)
    if not ref then
        return nil
    end

    local entity = sgame.entity[ref.number]
    if not entity or entity.generation ~= ref.generation then
        return nil
    end

    return entity
end

local function clear_alien_builder_owner(number)
    local owner = STATE.alien_builder_owner
    if not owner then
        return
    end

    if number == nil or owner.number == number then
        STATE.alien_builder_owner = nil
    end
end

local function alien_builder_owner()
    local owner = STATE.alien_builder_owner
    if not owner then
        return nil
    end

    local entity = resolve_entity_ref(owner)
    if not entity or not entity.client or entity.client.health <= 0 then
        STATE.alien_builder_owner = nil
        return nil
    end

    return owner
end

local function claim_alien_builder_owner(number)
    local entity = sgame.entity[number]
    if not entity then
        return false
    end

    STATE.alien_builder_owner = entity_ref(entity)
    return true
end

local function is_builder(team, client)
    if is_alien(team) then
        return client.class == "builder" or client.class == "builderupg"
    end

    if is_human(team) then
        return client.weapon == "ckit"
    end

    return false
end

local function team_level(team)
    if is_alien(team) then
        return sgame.level.aliens
    end

    if is_human(team) then
        return sgame.level.humans
    end

    return nil
end

local function main_building_name(team)
    if is_alien(team) then
        return "overmind"
    end

    if is_human(team) then
        return "reactor"
    end

    return nil
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

local function builder_needed(team_name, number, team_snapshot, level)
    if not team_snapshot or not level then
        return false
    end

    local level_time = sgame.level.match_time
    local chosen_cost = cache.chosen_buildable_cost(team_name)
    local usable_build_points = cache.usable_build_points(team_name)

    if is_alien(team_name) then
        return level_time >= 60000
            and level.num_players == 0
            and cache.cvar("g_bot_buildAliens") ~= "0"
            and usable_build_points >= chosen_cost
            and not cache.has_teammate_weapon(team_name, "abuild", number)
            and not cache.has_teammate_weapon(team_name, "abuildupg", number)
    end

    if is_human(team_name) then
        return level_time >= 60000
            and level.num_players == 0
            and cache.cvar("g_bot_buildHumans") ~= "0"
            and usable_build_points >= chosen_cost
            and not cache.has_teammate_weapon(team_name, "ckit", number)
    end

    return false
end

local function can_convert_to_builder_now(team, client, level)
    if not is_alien(team) then
        return true
    end

    return common.can_evolve_to_class(nil, client, level, "builderupg")
        or common.can_evolve_to_class(nil, client, level, "builder")
end

local function wants_builder(team_name, number, team_snapshot, level, builder, client)
    local wanted = builder_needed(team_name, number, team_snapshot, level)
    if not wanted then
        if is_alien(team_name) and not builder then
            clear_alien_builder_owner(number)
        end

        return false
    end

    if not is_alien(team_name) then
        return true
    end

    if builder then
        claim_alien_builder_owner(number)
        return true
    end

    if not can_convert_to_builder_now(team_name, client, level) then
        clear_alien_builder_owner(number)
        return false
    end

    local owner = alien_builder_owner()
    if owner then
        return owner.number == number
    end

    claim_alien_builder_owner(number)
    return true
end

local function wants_builder_spawn(team_name, number, team_snapshot, level)
    local wanted = builder_needed(team_name, number, team_snapshot, level)
    if not wanted then
        if is_alien(team_name) then
            clear_alien_builder_owner(number)
        end

        return false
    end

    if not is_alien(team_name) then
        return true
    end

    local owner = alien_builder_owner()
    if owner then
        return owner.number == number
    end

    claim_alien_builder_owner(number)
    return true
end

local function choose_spawn(team, number, team_snapshot, level, ctx)
    if is_alien(team) then
        if wants_builder_spawn(team, number, team_snapshot, level) then
            local status = ctx:spawnAs("builderupg")
            if status ~= STATUS_FAILURE then
                return status
            end

            return ctx:spawnAs("builder")
        end

        return ctx:spawnAs("level0")
    end

    if is_human(team) then
        if wants_builder_spawn(team, number, team_snapshot, level) then
            return ctx:spawnAs("ckit")
        end

        return ctx:spawnAs("rifle")
    end

    return STATUS_FAILURE
end

local function become_builder(team, number, client, builder_elapsed, ctx)
    if is_alien(team) then
        if client.class == "builder" or client.class == "builderupg" then
            return STATUS_FAILURE
        end

        if builder_elapsed >= 20000 then
            clear_alien_builder_owner(number)
            ctx:resetMyTimer()
            return STATUS_FAILURE
        end

        local status = ctx:evolveTo("builderupg")
        if status ~= STATUS_FAILURE then
            return status
        end

        return ctx:evolveTo("builder")
    end

    if is_human(team) then
        if client.weapon == "ckit" then
            return STATUS_FAILURE
        end

        if builder_elapsed >= 20000 then
            ctx:resetMyTimer()
            return STATUS_FAILURE
        end

        return ctx:buyPrimary("ckit")
    end

    return STATUS_FAILURE
end

local function can_become_builder(team)
    if is_alien(team) then
        return cache.cvar("g_bot_builderupg") ~= "0"
            or cache.cvar("g_bot_builder") ~= "0"
    end

    return cache.cvar("g_bot_ckit") ~= "0"
end

local function maybe_build(builder, ctx)
    if not builder then
        return STATUS_FAILURE
    end

    return ctx:buildNowChosenBuildable()
end

local function maybe_use_medkit(team, client, ctx)
    return use_medkit_if_low(team, client, ctx, 50)
end

local function maybe_extinguish_fire(team, client, ctx)
    if not is_alien(team) or client.class ~= "builderupg" or cache.cvar("g_bot_extinguishFire") == "0" then
        return STATUS_FAILURE
    end

    return ctx:extinguishFire()
end

local function maybe_evolve(self, team, client, builder, health_frac, level, mind, enemy_visible, ctx)
    if not is_alien(team) or builder or enemy_visible then
        return STATUS_FAILURE
    end

    local spawn_elapsed = elapsed_since(level.time, mind.spawnTime)
    if spawn_elapsed <= 1500 or health_frac < 0.6 then
        return STATUS_FAILURE
    end

    return try_evolve_targets(self, ctx, client, ALIEN_EVOLVE_TARGETS)
end

local function maybe_fight(team, weapon, enemy, enemy_target, hostile_goal, enemy_visible, ctx)
    if not enemy and not hostile_goal then
        return STATUS_FAILURE
    end

    if is_human(team) and weapon == "ckit" then
        return STATUS_FAILURE
    end

    if enemy_visible then
        return ctx:fight()
    end

    if is_alien(team) and hostile_goal then
        return ctx:fight()
    end

    if is_human(team) and (enemy or hostile_goal) then
        return ctx:fight()
    end

    if enemy and enemy_target.distance and enemy_target.distance < 500 and ctx:directPathToEntity(enemy) then
        return ctx:fight()
    end

    return STATUS_FAILURE
end

local function maybe_fight_or_heal_alien(health_frac, enemy, enemy_target, hostile_goal,
    enemy_visible, base_rush_score, level, ctx, mind, team, client)
    local overmind_distance = building_distance(mind, "overmind")
    local booster_distance = building_distance(mind, "booster")
    local in_safe_heal_area = (overmind_distance and overmind_distance <= 200)
        or (booster_distance and booster_distance <= 200)
    local recently_in_combat = elapsed_since(level.time, mind.enemyLastSeen) < 2000
    local recently_attacked = common.recently_attacked(level, mind, 2000)
    local alerted = enemy ~= nil or hostile_goal or recently_in_combat
    local low_tier_alien = is_alien(team) and (client.class == "level0" or client.class == "level1")
    local needs_healing = health_frac < 1.0

    if needs_healing and not recently_attacked then
        local status = ctx:heal()
        if status ~= STATUS_FAILURE then
            return status
        end

        if overmind_distance == nil or overmind_distance > 200 then
            status = ctx:roamInRadius("overmind", 200)
            if status ~= STATUS_FAILURE then
                return status
            end
        end

        if booster_distance == nil or booster_distance > 200 then
            status = ctx:roamInRadius("booster", 200)
            if status ~= STATUS_FAILURE then
                return status
            end
        end

        return STATUS_RUNNING
    end

    if alerted then
        if not low_tier_alien and health_frac < 0.4
            and not in_safe_heal_area
            and not recently_attacked
            and base_rush_score < 1.0 then
            local status = ctx:heal()
            if status ~= STATUS_FAILURE then
                return status
            end
        end

        return maybe_fight(team, nil, enemy, enemy_target, hostile_goal, enemy_visible, ctx)
    end

    if not low_tier_alien and health_frac < 0.4 and not in_safe_heal_area and not recently_attacked then
        return ctx:heal()
    end

    return STATUS_FAILURE
end

local function repair_target(mind)
    local info = human_repair_target_info(mind)
    return info and info.target or nil
end

local function maybe_retire_builder(team, number, client, builder, wants_build, ctx, mind)
    if not builder or wants_build then
        return STATUS_FAILURE
    end

    if is_human(team) then
        if repair_target(mind) then
            return STATUS_FAILURE
        end

        return ctx:equip()
    end

    if is_alien(team) then
        local target = common.best_alien_combat_target(number, client, ALIEN_COMBAT_TARGETS)
        if target then
            local status = ctx:evolveTo(target)
            if status ~= STATUS_FAILURE then
                clear_alien_builder_owner()
                return status
            end
        end
    end

    return STATUS_FAILURE
end

local function maybe_builder_behavior(team, number, client, builder, wants_build, level, builder_elapsed,
    enemy, enemy_target, enemy_visible, hostile_goal, base_rush_score, ctx, mind)
    if not wants_build then
        return STATUS_FAILURE
    end

    if not builder and not can_become_builder(team) then
        return STATUS_FAILURE
    end

    local anchor = main_building_name(team)
    local anchor_target = anchor and mind:closestBuilding(anchor) or nil
    local anchor_distance = anchor_target and anchor_target.distance or nil

    local status = become_builder(team, number, client, builder_elapsed, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = maybe_build(builder, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    if anchor then
        status = ctx:roamInRadius(anchor, BUILDER_ANCHOR_RADIUS)
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    status = maybe_fight(team, client.weapon, enemy, enemy_target, hostile_goal, enemy_visible, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    if is_alien(team) then
        status = maybe_fight_or_heal_alien(health_fraction(client), enemy, enemy_target, hostile_goal,
            enemy_visible, base_rush_score, sgame.level, ctx, mind, team, client)
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    status = ctx:rush()
    if status ~= STATUS_FAILURE then
        return status
    end

    if anchor_distance == nil or anchor_distance > BUILDER_ANCHOR_RADIUS then
        if anchor then
            status = ctx:roamInRadius(anchor, BUILDER_ANCHOR_RADIUS)
            if status ~= STATUS_FAILURE then
                return status
            end
        end
    end

    return ctx:roam()
end

local function maybe_repair(team, number, client, ctx, mind)
    if not is_human(team) then
        return STATUS_FAILURE
    end

    local target = repair_target(mind)
    if not target then
        return STATUS_FAILURE
    end

    if cache.has_teammate_weapon(team, "ckit", number) and client.weapon ~= "ckit" then
        return STATUS_FAILURE
    end

    if client.weapon ~= "ckit" then
        return ctx:buyPrimary("ckit")
    end

    return ctx:repair()
end

local function maybe_reload(team, client, weapon_attr, level, mind, enemy_visible, ctx)
    if not is_human(team)
        or enemy_visible
        or elapsed_since(level.time, mind.enemyLastSeen) <= 3000
        or not weapon_attr
        or weapon_attr.ammo <= 0 then
        return STATUS_FAILURE
    end

    if (client.ammo or 0) / weapon_attr.ammo >= 0.4 then
        return STATUS_FAILURE
    end

    return ctx:reload()
end

local function maybe_equip(team, level, mind, enemy_visible, ctx)
    if not is_human(team)
        or enemy_visible
        or elapsed_since(level.time, mind.enemyLastSeen) <= 1000 then
        return STATUS_FAILURE
    end

    return ctx:equip()
end

local function maybe_empty_ammo_resupply(team, client, weapon_attr, ctx)
    if not is_human(team) or not weapon_attr or weapon_attr.ammo <= 0 then
        return STATUS_FAILURE
    end

    if (client.ammo or 0) > 0 or (client.clips or 0) > 0 then
        return STATUS_FAILURE
    end

    return ctx:moveTo("arm")
end

local function maybe_rush(builder, base_rush_score, ctx)
    if builder or base_rush_score <= 0.5 then
        return STATUS_FAILURE
    end

    return ctx:rush()
end

local function human_should_reload(team, client, weapon_attr, level, mind, enemy_visible)
    return is_human(team)
        and not enemy_visible
        and elapsed_since(level.time, mind.enemyLastSeen) > 3000
        and weapon_attr ~= nil
        and weapon_attr.ammo > 0
        and (client.ammo or 0) / weapon_attr.ammo < 0.4
end

local function human_should_equip(team, level, mind, enemy_visible)
    return is_human(team)
        and not enemy_visible
        and elapsed_since(level.time, mind.enemyLastSeen) > 1000
end

local function human_should_resupply_empty_ammo(team, client, weapon_attr)
    return is_human(team)
        and weapon_attr ~= nil
        and weapon_attr.ammo > 0
        and (client.ammo or 0) <= 0
        and (client.clips or 0) <= 0
end

local function alien_support_needed(state)
    if state.client.class == "builderupg" and cache.cvar("g_bot_extinguishFire") ~= "0" then
        return true
    end

    if common.best_alien_evolve_target(state.number, state.client, state.level, ALIEN_EVOLVE_TARGETS) ~= nil then
        return true
    end

    return state.health_frac < 1.0 and not common.recently_attacked(state.level, state.mind, 2000)
end

local function has_meaningful_combat_pressure(state, ctx)
    if state.enemy_visible then
        return true
    end

    if state.hostile_goal then
        return true
    end

    return state.enemy ~= nil
        and state.enemy_target.distance ~= nil
        and state.enemy_target.distance < 500
        and ctx:directPathToEntity(state.enemy)
end

local BUILDER_TASK = {
    should_preempt = function(_, state, ctx)
        return has_meaningful_combat_pressure(state, ctx)
    end,
    preempt_cooldown_ms = 1000,
    run = function(_, state, ctx)
        if not state.wants_build then
            return STATUS_FAILURE
        end

        if not state.builder and not can_become_builder(state.team) then
            return STATUS_FAILURE
        end

        local status = become_builder(state.team, state.number, state.client, state.builder_elapsed, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_build(state.builder, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        local anchor = main_building_name(state.team)
        if anchor then
            status = ctx:roamInRadius(anchor, BUILDER_ANCHOR_RADIUS)
            if status ~= STATUS_FAILURE then
                return status
            end
        end

        return STATUS_RUNNING
    end,
}

local COMBAT_TASK = {
    run = function(_, state, ctx)
        if not has_meaningful_combat_pressure(state, ctx) then
            return STATUS_FAILURE
        end

        local status = maybe_fight(state.team, state.client.weapon, state.enemy, state.enemy_target,
            state.hostile_goal, state.enemy_visible, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        if is_alien(state.team) then
            return maybe_fight_or_heal_alien(state.health_frac, state.enemy, state.enemy_target, state.hostile_goal,
                state.enemy_visible, state.base_rush_score, state.level, ctx, state.mind, state.team, state.client)
        end

        return STATUS_FAILURE
    end,
}

local SUPPORT_TASK = {
    should_preempt = function(_, state, ctx)
        return not state.wants_build and has_meaningful_combat_pressure(state, ctx)
    end,
    run = function(_, state, ctx)
        local status = nil

        if is_human(state.team) then
            status = maybe_repair(state.team, state.number, state.client, ctx, state.mind)
            if status ~= STATUS_FAILURE then
                return status
            end

            status = maybe_reload(state.team, state.client, state.weapon_attr, state.level, state.mind,
                state.enemy_visible, ctx)
            if status ~= STATUS_FAILURE then
                return status
            end

            status = maybe_equip(state.team, state.level, state.mind, state.enemy_visible, ctx)
            if status ~= STATUS_FAILURE then
                return status
            end

            return maybe_empty_ammo_resupply(state.team, state.client, state.weapon_attr, ctx)
        end

        status = maybe_extinguish_fire(state.team, state.client, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_evolve(state.self, state.team, state.client, state.builder, state.health_frac,
            state.level, state.mind, state.enemy_visible, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        return maybe_fight_or_heal_alien(state.health_frac, state.enemy, state.enemy_target, state.hostile_goal,
            state.enemy_visible, state.base_rush_score, state.level, ctx, state.mind, state.team, state.client)
    end,
}

local IDLE_TASK = {
    should_preempt = function(_, state, ctx)
        return has_meaningful_combat_pressure(state, ctx)
    end,
    run = function(_, state, ctx)
        local status = maybe_rush(state.builder, state.base_rush_score, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        return ctx:roam()
    end,
}

local function should_support(state)
    if is_human(state.team) then
        if repair_target(state.mind) then
            return true
        end

        if human_should_reload(state.team, state.client, state.weapon_attr, state.level, state.mind,
            state.enemy_visible) then
            return true
        end

        if human_should_equip(state.team, state.level, state.mind, state.enemy_visible) then
            return true
        end

        return human_should_resupply_empty_ammo(state.team, state.client, state.weapon_attr)
    end

    return alien_support_needed(state)
end

local function select_task(state)
    if state.wants_build then
        if has_meaningful_combat_pressure(state, state.ctx) then
            return COMBAT_TASK
        end

        return BUILDER_TASK
    end

    if has_meaningful_combat_pressure(state, state.ctx) then
        return COMBAT_TASK
    end

    if should_support(state) then
        return SUPPORT_TASK
    end

    return IDLE_TASK
end

return function(self, ctx)
    local client = self.client
    local team = self.team
    local number = self.number
    local level = sgame.level
    local team_level_data = team_level(team)

    cache.refresh()
    local team_snapshot = cache.team(team)

    if should_spawn(self, PMF_QUEUED) then
        return choose_spawn(team, number, team_snapshot, team_level_data, ctx)
    end

    local bot = self.bot
    local mind = bot and bot.mind or nil
    if not client or not mind then
        return ctx:roam()
    end

    local weapon_attr = weapons[client.weapon]
    local builder = is_builder(team, client)
    local wants_build = wants_builder(team, number, team_snapshot, team_level_data, builder, client)
    local hostile_goal = is_hostile_target(team, mind.goal)
    local enemy_target = mind.bestEnemy
    local enemy = target_entity(enemy_target)
    local enemy_visible = enemy and ctx:isVisibleEntity(enemy) or false
    local health_frac = health_fraction(client)
    local builder_elapsed = elapsed_since(level.time, mind.stuckTimer)
    local base_rush_score = ctx.baseRushScore or 0
    local state = {
        self = self,
        ctx = ctx,
        team = team,
        number = number,
        level = level,
        client = client,
        mind = mind,
        weapon_attr = weapon_attr,
        builder = builder,
        wants_build = wants_build,
        hostile_goal = hostile_goal,
        enemy_target = enemy_target,
        enemy = enemy,
        enemy_visible = enemy_visible,
        health_frac = health_frac,
        builder_elapsed = builder_elapsed,
        base_rush_score = base_rush_score,
    }

    if not wants_build then
        mind.stuckTimer = level.time
    end

    local status = unstick(level.time, ctx, mind, enemy, enemy_visible, team, client, builder)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = maybe_retire_builder(team, number, client, builder, wants_build, ctx, mind)
    if status ~= STATUS_FAILURE then
        return status
    end

    if is_human(team) then
        status = maybe_use_medkit(team, client, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    local task = TASKS.current(number)
    if task then
        status = TASKS.maybe_preempt(state, ctx, select_task)
        if status ~= STATUS_FAILURE then
            return status
        end

        task = TASKS.current(number)
        if task then
            status = TASKS.run(task, state, ctx)
            if status ~= STATUS_FAILURE then
                return status
            end
        end
    end

    task = TASKS.start(number, select_task(state))
    return TASKS.run(task, state, ctx)
end
