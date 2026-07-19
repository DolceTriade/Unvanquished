STATUS_FAILURE = 0
STATUS_SUCCESS = 1
STATUS_RUNNING = 2

local PMF_QUEUED = 1 << 12
package.loaded["bots/common.lua"] = nil

local cache = require("bots/cache.lua")
local common = require("bots/common.lua")
local weapons = Unv.weapons
local elapsed_since = common.elapsed_since
local is_alien = common.is_alien
local is_human = common.is_human
local alive = common.alive
local should_spawn = common.should_spawn
local target_entity = common.target_entity
local health_fraction = common.health_fraction
local building_distance = common.building_distance
local recently_attacked = common.recently_attacked
local use_medkit_if_low = common.use_medkit_if_low
local try_evolve_targets = common.try_evolve_targets
local human_repair_target_info = common.human_repair_target_info
local unstick = common.unstick
local STATE = {
    alien_target_lock = {},
    alien_combat_until = {},
    alien_builder_owner = nil,
}

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

local function entity_is_alive(entity)
    if not entity then
        return false
    end

    local client = entity.client
    if client then
        return client.health and client.health > 0 or false
    end

    local buildable = entity.buildable
    if buildable then
        return buildable.health and buildable.health > 0 or false
    end

    return true
end

local function clear_alien_target_lock(number)
    STATE.alien_target_lock[number] = nil
end

local function clear_alien_combat_latch(number)
    STATE.alien_combat_until[number] = nil
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

local function is_alien_builder_owner(number)
    local owner = alien_builder_owner()
    return owner and owner.number == number or false
end

local function lock_alien_target(number, entity, now)
    if not entity or not entity_is_alive(entity) then
        return
    end

    STATE.alien_target_lock[number] = {
        ref = entity_ref(entity),
        seen_at = now,
    }
end

local function alien_combat_latched(number, now)
    local until_time = STATE.alien_combat_until[number]
    return until_time and until_time > now or false
end

local function note_alien_combat(number, now, duration)
    STATE.alien_combat_until[number] = now + duration
end

local function alien_target_lock(number)
    return STATE.alien_target_lock[number]
end

local function is_hostile_target(team, target)
    if not target or target.kind ~= "entity" then
        return false
    end

    if not entity_is_alive(target.entity) then
        return false
    end

    local target_team = target.team
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

local function distance_to_target(ctx, target)
    local entity = target_entity(target)
    if entity then
        return ctx:distanceToEntity(entity)
    end

    if target and target.kind == "coordinates"
        and target.positionX ~= nil
        and target.positionY ~= nil
        and target.positionZ ~= nil then
        return ctx:distanceToPosition(target.positionX, target.positionY, target.positionZ)
    end

    if target and target.kind == "coordinates" then
        return ctx:distanceToPosition(target.position)
    end

    return nil
end

local function goal_reached(ctx, goal, radius)
    local distance = distance_to_target(ctx, goal)
    return distance and distance <= radius or false
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

local function class_attr(class_name)
    return common.class_attr(class_name)
end

local function class_health(class_name)
    return common.class_health(class_name)
end

local function can_evolve_to_class(client, level, class_name)
    return common.can_evolve_to_class(client, level, class_name)
end

local function best_alien_evolve_target(client, level)
    return common.best_alien_evolve_target(nil, client, level, ALIEN_EVOLVE_TARGETS)
end

local function best_alien_combat_target(number, client)
    return common.best_alien_combat_target(number, client, ALIEN_COMBAT_TARGETS)
end

local function choose_alien_enemy(number, now, enemy_target, enemy_visible, hostile_goal, level, ctx, mind)
    local sensed_entity = target_entity(enemy_target)
    local lock = alien_target_lock(number)
    local locked_entity = lock and resolve_entity_ref(lock.ref) or nil
    local aliensense_range = cache.cvar_number("g_bot_aliensenseRange")
    local chase_time = cache.cvar_number("g_bot_chasetime")

    if sensed_entity and not entity_is_alive(sensed_entity) then
        sensed_entity = nil
    end

    if locked_entity and not entity_is_alive(locked_entity) then
        clear_alien_target_lock(number)
        lock = nil
        locked_entity = nil
    end

    if sensed_entity and enemy_visible then
        lock_alien_target(number, sensed_entity, now)
        return sensed_entity
    end

    if hostile_goal then
        local goal_entity = target_entity(mind.goal)
        if goal_entity and entity_is_alive(goal_entity) then
            lock_alien_target(number, goal_entity, now)
            return goal_entity
        end
    end

    if not locked_entity then
        if sensed_entity then
            lock_alien_target(number, sensed_entity, now)
            note_alien_combat(number, now, 1500)
            return sensed_entity
        end

        clear_alien_target_lock(number)
        clear_alien_combat_latch(number)
        return nil
    end

    local locked_distance = ctx:distanceToEntity(locked_entity)
    if locked_distance and aliensense_range > 0 and locked_distance <= aliensense_range then
        lock.seen_at = now
        note_alien_combat(number, now, 1500)
        return locked_entity
    end

    if elapsed_since(now, lock.seen_at) <= chase_time then
        note_alien_combat(number, now, 1000)
        return locked_entity
    end

    if sensed_entity then
        lock_alien_target(number, sensed_entity, now)
        note_alien_combat(number, now, 1500)
        return sensed_entity
    end

    clear_alien_target_lock(number)
    clear_alien_combat_latch(number)
    return nil
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
            and (level.num_players == 0
                or ((team_snapshot.buildables.eggpod or 0) < 2
                    and (team_snapshot.buildables.eggpod or 0) < cache.cvar_number("g_bot_buildNumEggs")))
            and cache.cvar("g_bot_buildAliens") ~= "0"
            and usable_build_points >= chosen_cost
            and not cache.has_teammate_weapon(team_name, "abuild", number)
            and not cache.has_teammate_weapon(team_name, "abuildupg", number)
    end

    if is_human(team_name) then
        return level_time >= 60000
            and (level.num_players == 0
                or ((team_snapshot.buildables.telenode or 0) < 2
                    and (team_snapshot.buildables.telenode or 0) < cache.cvar_number("g_bot_buildNumTelenodes")))
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

    return common.can_evolve_to_class(client, level, "builderupg")
        or common.can_evolve_to_class(client, level, "builder")
end

local function wants_builder(team_name, number, team_snapshot, level, builder)
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

    if not can_convert_to_builder_now(team_name, sgame.entity[number].client, level) then
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

        status = ctx:evolveTo("builder")
        if status ~= STATUS_FAILURE then
            return status
        end

        return STATUS_FAILURE
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

local function can_become_builder(team, client, level)
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

local function human_repair_target(mind)
    local info = human_repair_target_info(mind)
    return info and info.target or nil
end

local maybe_fight

local function maybe_retire_builder(team, number, client, builder, wants_build, ctx, mind)
    if not builder or wants_build then
        return STATUS_FAILURE
    end

    if is_human(team) then
        if human_repair_target(mind) then
            return STATUS_FAILURE
        end
        return ctx:equip()
    end

    if is_alien(team) then
        local target = best_alien_combat_target(number, client)
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
    enemy, enemy_visible, hostile_goal, ctx, mind)
    if not wants_build then
        return STATUS_FAILURE
    end

    if not builder and not can_become_builder(team, client, level) then
        return STATUS_FAILURE
    end

    local anchor = main_building_name(team)
    local anchor_target = anchor and mind:closestBuilding(anchor) or nil
    local anchor_distance = anchor_target and anchor_target.distance or nil

    local status = become_builder(team, number, client, builder_elapsed, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    if anchor_distance == nil or anchor_distance > BUILDER_ANCHOR_RADIUS then
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

        return ctx:roam()
    end

    if builder then
        status = maybe_build(builder, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    if anchor then
        status = ctx:roamInRadius(anchor, BUILDER_ANCHOR_RADIUS)
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    status = maybe_fight(team, client.weapon, enemy, mind.bestEnemy, hostile_goal, enemy_visible, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    if can_become_builder(team, client, level) then
        status = ctx:rush()
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    return ctx:roam()
end

local function maybe_use_medkit(team, client, ctx)
    local status = use_medkit_if_low(team, client, ctx, 50)
    if status ~= STATUS_FAILURE then
        return STATUS_FAILURE
    end

    return STATUS_FAILURE
end

local function maybe_extinguish_fire(team, client, ctx)
    if not is_alien(team) or client.class ~= "builderupg" or cache.cvar("g_bot_extinguishFire") == "0" then
        return STATUS_FAILURE
    end

    return ctx:extinguishFire()
end

local function maybe_evolve(self, team, client, builder, health_frac, ctx, level, mind, enemy_visible)
    if not is_alien(team) then
        return STATUS_FAILURE
    end

    if builder then
        return STATUS_FAILURE
    end

    if enemy_visible then
        return STATUS_FAILURE
    end

    local spawn_elapsed = elapsed_since(level.time, mind.spawnTime)
    if spawn_elapsed <= 1500 then
        return STATUS_FAILURE
    end

    if health_frac < 0.6 then
        return STATUS_FAILURE
    end

    return try_evolve_targets(self, ctx, client, ALIEN_EVOLVE_TARGETS)
end

maybe_fight = function(team, weapon, enemy, enemy_target, hostile_goal, enemy_visible, ctx)
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

local function maybe_heal_or_fight_alien(number, health_frac, enemy, enemy_target, hostile_goal,
    enemy_visible, base_rush_score, level, ctx, mind, team, client)
    local overmind_distance = building_distance(mind, "overmind")
    local booster_distance = building_distance(mind, "booster")
    local in_safe_heal_area = (overmind_distance and overmind_distance <= 200)
        or (booster_distance and booster_distance <= 200)
    local recently_in_combat = elapsed_since(level.time, mind.enemyLastSeen) < 2000
    local recently_attacked = recently_attacked(level, mind, 2000)
    local latched = alien_combat_latched(number, level.time)
    local alerted = enemy ~= nil or hostile_goal or recently_in_combat or latched
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

        local status = maybe_fight("aliens", nil, enemy, enemy_target, hostile_goal, enemy_visible, ctx)
        if status ~= STATUS_FAILURE then
            note_alien_combat(number, level.time, 1500)
        end
        return status
    end

    if not low_tier_alien and health_frac < 0.4 and not in_safe_heal_area and not recently_attacked then
        local status = ctx:heal()
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    return STATUS_FAILURE
end

local function maybe_repair(team, number, weapon, team_snapshot, ctx, mind)
    if not is_human(team) or not team_snapshot then
        return STATUS_FAILURE
    end

    local target = human_repair_target(mind)
    if not target then
        return STATUS_FAILURE
    end

    if cache.has_teammate_weapon(team, "ckit", number) and weapon ~= "ckit" then
        return STATUS_FAILURE
    end

    local entity = target_entity(target)

    local buildable = entity.buildable
    local max_health = target.buildable and Unv.buildables[target.buildable] and Unv.buildables[target.buildable].health or nil
    if not buildable or not max_health or max_health <= 0 then
        return STATUS_FAILURE
    end

    if buildable.health / max_health >= 0.7 then
        return STATUS_FAILURE
    end

    if weapon ~= "ckit" then
        return ctx:buyPrimary("ckit")
    end

    return ctx:repair()
end

local function maybe_reload(team, client, weapon_attr, level, ctx, mind, enemy_visible)
    if not is_human(team)
        or enemy_visible
        or elapsed_since(level.time, mind.enemyLastSeen) <= 3000
        or not weapon_attr
        or weapon_attr.ammo <= 0
        or (client.ammo / weapon_attr.ammo) >= 0.4 then
        return STATUS_FAILURE
    end

    return ctx:reload()
end

local function maybe_equip(team, level, ctx, mind, enemy_visible)
    if not is_human(team) or enemy_visible or elapsed_since(level.time, mind.enemyLastSeen) <= 1000 then
        return STATUS_FAILURE
    end

    return ctx:equip()
end

local function maybe_flee_human(team, weapon, enemy_visible, ctx)
    if not is_human(team) or weapon ~= "ckit" or not enemy_visible then
        return STATUS_FAILURE
    end

    return ctx:flee()
end

local function maybe_rush(builder, base_rush_score, ctx)
    if builder or base_rush_score <= 0.5 then
        return STATUS_FAILURE
    end

    return ctx:rush()
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
        clear_alien_target_lock(number)
        clear_alien_combat_latch(number)
        return choose_spawn(team, number, team_snapshot, team_level_data, ctx)
    end

    local bot = self.bot
    local mind = bot.mind

    if not client or not mind then
        print("no client or bot mind")
        return ctx:roam()
    end

    local weapon = client.weapon
    local weapon_attr = weapons[weapon]
    local builder = is_builder(team, client)
    local wants_build = wants_builder(team, number, team_snapshot, team_level_data, builder)
    local hostile_goal = is_hostile_target(team, mind.goal)
    local enemy_target = mind.bestEnemy
    local enemy = target_entity(enemy_target)
    local enemy_visible = enemy and ctx:isVisibleEntity(enemy) or false

    if is_alien(team) then
        enemy = choose_alien_enemy(number, level.time, enemy_target, enemy_visible, hostile_goal, level, ctx, mind)
        enemy_visible = enemy and ctx:isVisibleEntity(enemy) or false
        hostile_goal = hostile_goal or enemy ~= nil
    else
        clear_alien_target_lock(number)
        clear_alien_combat_latch(number)
    end

    local health_frac = health_fraction(client)
    local builder_elapsed = elapsed_since(level.time, mind.stuckTimer)
    local base_rush_score = ctx.baseRushScore or 0

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

    status = maybe_use_medkit(team, client, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = maybe_builder_behavior(team, number, client, builder, wants_build, team_level_data, builder_elapsed,
        enemy, enemy_visible, hostile_goal, ctx, mind)
    if status ~= STATUS_FAILURE then
        return status
    end

    if is_alien(team) then
        status = maybe_extinguish_fire(team, client, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_evolve(self, team, client, builder, health_frac, ctx, level, mind, enemy_visible)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_heal_or_fight_alien(number, health_frac, enemy, enemy_target, hostile_goal,
            enemy_visible, base_rush_score, level, ctx, mind, team, sgame.entity[number].client)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_build(builder, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_rush(builder, base_rush_score, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        return ctx:roam()
    end

    status = maybe_fight(team, weapon, enemy, enemy_target, hostile_goal, enemy_visible, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = maybe_repair(team, number, weapon, team_snapshot, ctx, mind)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = maybe_reload(team, client, weapon_attr, level, ctx, mind, enemy_visible)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = maybe_equip(team, level, ctx, mind, enemy_visible)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = maybe_flee_human(team, weapon, enemy_visible, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = maybe_build(builder, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = maybe_rush(builder, base_rush_score, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    return ctx:roam()
end
