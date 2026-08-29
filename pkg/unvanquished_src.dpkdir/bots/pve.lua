STATUS_FAILURE = 0
STATUS_SUCCESS = 1
STATUS_RUNNING = 2

local PMF_QUEUED = 1 << 12
package.loaded["bots/common.lua"] = nil

local cache = require("bots/cache.lua")
local common = require("bots/common.lua")
local weapons = Unv.weapons
local random = math.random
local elapsed_since = common.elapsed_since
local is_alien = common.is_alien
local is_human = common.is_human
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
    builder_assignment = {},
    build_failures = {},
    build_retry_after = {},
    build_reposition_until = {},
    tasks = {},
}

local PVE_MAX_BUILDERS = 3
local PVE_BUILD_JITTER_MS = 5000
local PVE_BUILD_FAILURE_BASE_DELAY_MS = 750
local PVE_BUILD_FAILURE_MAX_DELAY_MS = 5000
local PVE_BUILD_REPOSITION_THRESHOLD = 3
local PVE_BUILD_REPOSITION_MS = 2000
local PVE_HUMAN_BUILDER_SEARCH_MS = 4000
local ALIEN_EVOLVE_TARGETS = common.ALIEN_EVOLVE_TARGETS
local ALIEN_COMBAT_TARGETS = common.ALIEN_COMBAT_TARGETS
local function random_choice(options)
    if not options or #options == 0 then
        return nil
    end

    return options[random(#options)]
end

local function jitter_offset(number, spawn_time)
    local seed = (number or 0) * 1103 + (spawn_time or 0)
    return seed % (PVE_BUILD_JITTER_MS + 1)
end

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

local function clear_build_failure_state(number)
    STATE.build_failures[number] = nil
    STATE.build_retry_after[number] = nil
    STATE.build_reposition_until[number] = nil
end

local function clear_builder_assignment(number)
    STATE.builder_assignment[number] = nil
end

local function builder_assignment_team(number)
    return STATE.builder_assignment[number]
end

local function has_builder_assignment(number, team_name)
    local assigned_team = builder_assignment_team(number)
    if not assigned_team then
        return false
    end

    if team_name ~= nil then
        return assigned_team == team_name
    end

    return true
end

local function assign_builder(number, team_name)
    STATE.builder_assignment[number] = team_name
end

local function clear_task(number)
    STATE.tasks[number] = nil
end

local function current_task(number)
    return STATE.tasks[number]
end

local function start_task(number, task)
    STATE.tasks[number] = task
    return task
end

local select_task

local function run_task(task, state, ctx)
    local status = task.run(task, state, ctx)
    if status ~= STATUS_RUNNING then
        clear_task(state.number)
    end

    return status
end

local function maybe_preempt_task(state, ctx)
    local task = current_task(state.number)
    if not task or not task.should_preempt then
        return STATUS_FAILURE
    end

    local retry_at = task.preempt_retry_at
    if retry_at and retry_at > state.level.time then
        return STATUS_FAILURE
    end

    if not task.should_preempt(task, state, ctx) then
        return STATUS_FAILURE
    end

    local previous_task = task
    local replacement = select_task(state)
    if not replacement then
        return STATUS_FAILURE
    end

    clear_task(state.number)
    start_task(state.number, replacement)

    local status = run_task(replacement, state, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    previous_task.preempt_retry_at = state.level.time + (previous_task.preempt_cooldown_ms or 1000)
    start_task(state.number, previous_task)
    return STATUS_FAILURE
end

local function builder_role(task)
    return task
end

local function attack_role(task)
    return task
end

local function roam_role(task)
    return task
end

local function build_retry_active(number, now)
    local retry_at = STATE.build_retry_after[number]
    return retry_at ~= nil and now ~= nil and retry_at > now or false
end

local function build_reposition_active(number, now)
    local reposition_until = STATE.build_reposition_until[number]
    return reposition_until ~= nil and now ~= nil and reposition_until > now or false
end

local function note_build_failure(number, now)
    local failures = (STATE.build_failures[number] or 0) + 1
    STATE.build_failures[number] = failures

    local delay = PVE_BUILD_FAILURE_BASE_DELAY_MS * failures
    if delay > PVE_BUILD_FAILURE_MAX_DELAY_MS then
        delay = PVE_BUILD_FAILURE_MAX_DELAY_MS
    end

    STATE.build_retry_after[number] = now + delay
    if failures >= PVE_BUILD_REPOSITION_THRESHOLD then
        STATE.build_reposition_until[number] = now + PVE_BUILD_REPOSITION_MS
    end
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

local function best_alien_combat_target(number, client, level)
    return common.best_alien_combat_target(number, client, level, ALIEN_COMBAT_TARGETS)
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

local function builder_count(team_name, team_snapshot, number)
    local count = 0

    for entity_num, assigned_team in pairs(STATE.builder_assignment) do
        if entity_num ~= number and assigned_team == team_name then
            count = count + 1
        end
    end

    if count < 0 then
        return 0
    end

    return count
end

local function choose_pve_buildable(team_name, team_snapshot)
    if not team_snapshot then
        return nil
    end

    if is_human(team_name) then
        local telenodes = team_snapshot.buildables.telenode or 0
        local arms = team_snapshot.buildables.arm or 0
        local medis = team_snapshot.buildables.medistat or 0
        local can_build_telenode = telenodes < cache.cvar_number("g_bot_buildNumTelenodes")
        local support = nil

        if (team_snapshot.buildables.reactor or 0) == 0 then
            return "reactor"
        end

        if cache.cvar_number("g_maxMiners") ~= 0 and (team_snapshot.buildables.drill or 0) == 0 then
            return "drill"
        end

        if arms == 0 and medis == 0 then
            support = random(2) == 1 and "arm" or "medistat"
        elseif arms == 0 then
            support = "arm"
        elseif medis == 0 then
            support = "medistat"
        end

        local roll = random(10)

        if roll <= 6 then
            if cache.buildable_unlocked("humans", "rocketpod")
                and random() < cache.cvar_number("g_bot_buildProbRocketPod") then
                return "rocketpod"
            end

            return "mgturret"
        end

        if roll <= 9 and can_build_telenode then
            return "telenode"
        end

        if support then
            return support
        end

        if can_build_telenode then
            return "telenode"
        end

        if cache.buildable_unlocked("humans", "rocketpod")
            and random() < cache.cvar_number("g_bot_buildProbRocketPod") then
            return "rocketpod"
        end

        return "mgturret"
    end

    if is_alien(team_name) then
        if (team_snapshot.buildables.overmind or 0) == 0 then
            return "overmind"
        end

        if cache.cvar_number("g_maxMiners") ~= 0 and (team_snapshot.buildables.leech or 0) == 0 then
            return "leech"
        end

        if (team_snapshot.buildables.eggpod or 0) == 0 then
            return "eggpod"
        end

        if cache.buildable_unlocked("aliens", "booster") and (team_snapshot.buildables.booster or 0) == 0 then
            return "booster"
        end

        return random_choice({
            "eggpod",
            "acid_tube",
            "hive",
            "booster",
        })
    end

    return nil
end

local function builder_needed(team_name, number, team_snapshot, level, selected_buildable)
    if not team_snapshot or not level or not selected_buildable then
        return false
    end

    local chosen_cost = cache.buildable_cost(selected_buildable)
    local usable_build_points = cache.usable_build_points(team_name)
    local build_enabled = is_alien(team_name) and cache.cvar("g_bot_buildAliens") ~= "0"
        or is_human(team_name) and cache.cvar("g_bot_buildHumans") ~= "0"

    return build_enabled
        and usable_build_points >= chosen_cost
        and builder_count(team_name, team_snapshot, number) < PVE_MAX_BUILDERS
end

local function can_convert_to_builder_now(team, client, level)
    if not is_alien(team) then
        return true
    end

    return common.can_evolve_to_class(client, level, "builderupg")
        or common.can_evolve_to_class(client, level, "builder")
end

local function wants_builder(team_name, number, team_snapshot, level, builder, selected_buildable)
    if has_builder_assignment(number) then
        if builder_needed(team_name, number, team_snapshot, level, selected_buildable) then
            return true
        end

        clear_builder_assignment(number)
        return false
    end

    if not builder_needed(team_name, number, team_snapshot, level, selected_buildable) then
        return false
    end

    if builder or can_convert_to_builder_now(team_name, sgame.entity[number].client, level) then
        assign_builder(number, team_name)
        return true
    end

    return false
end

local function wants_builder_spawn(team_name, number, team_snapshot, level, selected_buildable)
    if has_builder_assignment(number) then
        if builder_needed(team_name, number, team_snapshot, level, selected_buildable) then
            return true
        end

        clear_builder_assignment(number)
        return false
    end

    if not builder_needed(team_name, number, team_snapshot, level, selected_buildable) then
        return false
    end

    assign_builder(number, team_name)
    return true
end

local function choose_spawn(team, number, team_snapshot, level, selected_buildable, ctx)
    if is_alien(team) then
        if wants_builder_spawn(team, number, team_snapshot, level, selected_buildable) then
            local status = ctx:spawnAs("builderupg")
            if status ~= STATUS_FAILURE then
                return status
            end

            return ctx:spawnAs("builder")
        end
        return ctx:spawnAs("level0")
    end

    if is_human(team) then
        if wants_builder_spawn(team, number, team_snapshot, level, selected_buildable) then
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

local function maybe_build(builder, selected_buildable, level, mind, number, ctx)
    if not builder or not selected_buildable then
        return STATUS_FAILURE
    end

    if build_retry_active(number, level.time) then
        return STATUS_FAILURE
    end

    if (level.time % (PVE_BUILD_JITTER_MS + 1)) < jitter_offset(number, mind.spawnTime) then
        return STATUS_FAILURE
    end

    if not ctx:canBuild(selected_buildable) then
        note_build_failure(number, level.time)
        return STATUS_FAILURE
    end

    local status = ctx:buildNow(selected_buildable)
    if status ~= STATUS_FAILURE then
        clear_build_failure_state(number)
    else
        note_build_failure(number, level.time)
    end

    return status
end

local function human_repair_target(mind)
    local info = human_repair_target_info(mind)
    return info and info.target or nil
end

local maybe_fight

local function maybe_retire_builder(team, number, client, builder, wants_build, level, ctx, mind)
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
        local target = best_alien_combat_target(number, client, level)
        if target then
            local status = ctx:evolveTo(target)
            if status ~= STATUS_FAILURE then
                return status
            end
        end
    end

    return STATUS_FAILURE
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

    return try_evolve_targets(self, ctx, client, level, ALIEN_EVOLVE_TARGETS)
end

maybe_fight = function(team, weapon, enemy, enemy_target, hostile_goal, enemy_visible, ctx)
    if not enemy and not hostile_goal then
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
    local stuck_for = elapsed_since(level.time, mind.stuckTime)

    if is_alien(team) and stuck_for > 45000 then
        return ctx:suicide()
    end

    if alerted then
        local status = maybe_fight("aliens", nil, enemy, enemy_target, hostile_goal, enemy_visible, ctx)
        if status ~= STATUS_FAILURE then
            note_alien_combat(number, level.time, 1500)
        end
        return status
    end

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

        return STATUS_FAILURE
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

    if not has_builder_assignment(number) and builder_count(team, team_snapshot, number) > 0 then
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

local function maybe_equip(team, number, level, ctx, mind, enemy_visible)
    if not is_human(team) or enemy_visible or elapsed_since(level.time, mind.enemyLastSeen) <= 1000 then
        return STATUS_FAILURE
    end

    if has_builder_assignment(number) then
        return STATUS_FAILURE
    end

    return ctx:equip()
end

local function maybe_rush(builder, base_rush_score, ctx)
    if builder or base_rush_score <= 0.5 then
        return STATUS_FAILURE
    end

    return ctx:rush()
end

local function timed_rush_score(level)
    local timelimit_minutes = cache.cvar_number("g_timelimit")
    if timelimit_minutes <= 0 or not level or not level.time then
        return 0
    end

    local timelimit_ms = timelimit_minutes * 60 * 1000
    if timelimit_ms <= 0 then
        return 0
    end

    local progress = level.time / timelimit_ms
    if progress <= 0.5 then
        return 0
    end

    local rush_score = (progress - 0.5) * 2
    if rush_score > 1 then
        return 1
    end

    return rush_score
end

local function combat_active(state)
    if state.enemy or state.hostile_goal then
        return true
    end

    if is_alien(state.team) then
        local recently_in_combat = elapsed_since(state.level.time, state.mind.enemyLastSeen) < 2000
        return recently_in_combat or alien_combat_latched(state.number, state.level.time)
    end

    return false
end

local function urgent_combat(state)
    return state.enemy_visible
end

local function run_builder_task(task, state, ctx)
    if not state.wants_build then
        return STATUS_FAILURE
    end

    if not state.builder
        and not (is_human(state.team) and task.phase == "search")
        and not can_become_builder(state.team, state.client, state.team_level_data) then
        return STATUS_FAILURE
    end

    if task.phase == "equip" then
        if state.client.weapon ~= "ckit"
            or state.enemy_visible then
            task.phase = "search"
            task.search_until = state.level.time + PVE_HUMAN_BUILDER_SEARCH_MS
        else
            return ctx:equip()
        end
    end

    if task.phase == "search" and is_human(state.team) then
        if state.level.time >= (task.search_until or 0)
            or human_repair_target(state.mind) then
            task.phase = "acquire"
        else
            local status = maybe_fight(state.team, state.client.weapon, state.enemy, state.enemy_target,
                state.hostile_goal, state.enemy_visible, ctx)
            if status ~= STATUS_FAILURE then
                return status
            end

            status = maybe_reload(state.team, state.client, state.weapon_attr, state.level, ctx, state.mind, state.enemy_visible)
            if status ~= STATUS_FAILURE then
                return status
            end

            status = ctx:roam()
            if status ~= STATUS_FAILURE then
                return status
            end

            return STATUS_FAILURE
        end
    end

    if not state.builder and task.phase == nil then
        task.phase = "acquire"
    end

    local status = become_builder(state.team, state.number, state.client, state.builder_elapsed, ctx)
    if status ~= STATUS_FAILURE then
        clear_build_failure_state(state.number)
        task.phase = "acquire"
        task.search_until = nil
        return STATUS_RUNNING
    end

    if is_human(state.team) and not state.builder then
        status = ctx:roam()
        if status ~= STATUS_FAILURE then
            return status
        end

        return STATUS_RUNNING
    end

    task.phase = nil

    if build_reposition_active(state.number, state.level.time) then
        status = ctx:roam()
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    if state.builder then
        status = maybe_build(state.builder, state.selected_buildable, state.level, state.mind, state.number, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        if is_human(state.team)
            and state.client.weapon == "ckit"
            and not human_repair_target(state.mind)
            and not ctx:canBuild(state.selected_buildable) then
            task.phase = "search"
            task.search_until = state.level.time + PVE_HUMAN_BUILDER_SEARCH_MS

            status = ctx:roam()
            if status ~= STATUS_FAILURE then
                return status
            end

            return STATUS_RUNNING
        end
    end

    if is_human(state.team) and state.builder and state.client.weapon == "ckit" and state.enemy_visible then
        status = ctx:flee()
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    if is_alien(state.team) and state.health_frac < 0.8 then
        status = ctx:heal()
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    status = ctx:roam()
    if status ~= STATUS_FAILURE then
        return status
    end

    return STATUS_FAILURE
end

local function should_preempt_builder_task(task, state, ctx)
    return not state.wants_build or urgent_combat(state)
end

local function new_builder_task()
    return {
        role = builder_role,
        phase = nil,
        search_until = nil,
        preempt_cooldown_ms = 750,
        run = run_builder_task,
        should_preempt = should_preempt_builder_task,
    }
end

local function run_attack_task(task, state, ctx)
    if is_alien(state.team) then
        local status = maybe_extinguish_fire(state.team, state.client, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_heal_or_fight_alien(state.number, state.health_frac, state.enemy, state.enemy_target,
            state.hostile_goal, state.enemy_visible, state.base_rush_score, state.level, ctx, state.mind,
            state.team, sgame.entity[state.number].client)
        if status ~= STATUS_FAILURE then
            return status
        end

        return STATUS_FAILURE
    end

    local status = maybe_fight(state.team, state.weapon, state.enemy, state.enemy_target,
        state.hostile_goal, state.enemy_visible, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    if state.builder and state.client.weapon == "ckit" and state.enemy_visible then
        status = ctx:flee()
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    return STATUS_FAILURE
end

local function should_preempt_attack_task(task, state, ctx)
    return not combat_active(state)
end

local function new_attack_task()
    return {
        role = attack_role,
        preempt_cooldown_ms = 750,
        run = run_attack_task,
        should_preempt = should_preempt_attack_task,
    }
end

local function run_roam_task(task, state, ctx)
    if is_alien(state.team) then
        local status = maybe_extinguish_fire(state.team, state.client, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_evolve(state.self, state.team, state.client, state.builder, state.health_frac,
            ctx, state.level, state.mind, state.enemy_visible)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_heal_or_fight_alien(state.number, state.health_frac, state.enemy, state.enemy_target,
            state.hostile_goal, state.enemy_visible, state.base_rush_score, state.level, ctx, state.mind,
            state.team, sgame.entity[state.number].client)
        if status ~= STATUS_FAILURE then
            return status
        end

        if state.builder then
            status = maybe_build(state.builder, state.selected_buildable, state.level, state.mind, state.number, ctx)
            if status ~= STATUS_FAILURE then
                return status
            end
        end

        status = maybe_rush(state.builder, state.base_rush_score, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        return ctx:roam()
    end

    local status = maybe_fight(state.team, state.weapon, state.enemy, state.enemy_target,
        state.hostile_goal, state.enemy_visible, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = maybe_repair(state.team, state.number, state.weapon, state.team_snapshot, ctx, state.mind)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = maybe_reload(state.team, state.client, state.weapon_attr, state.level, ctx, state.mind, state.enemy_visible)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = maybe_equip(state.team, state.number, state.level, ctx, state.mind, state.enemy_visible)
    if status ~= STATUS_FAILURE then
        return status
    end

    if state.builder then
        status = maybe_build(state.builder, state.selected_buildable, state.level, state.mind, state.number, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    status = maybe_rush(state.builder, state.base_rush_score, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    return ctx:roam()
end

local function should_preempt_roam_task(task, state, ctx)
    return state.wants_build or combat_active(state)
end

local function new_roam_task()
    return {
        role = roam_role,
        preempt_cooldown_ms = 1500,
        run = run_roam_task,
        should_preempt = should_preempt_roam_task,
    }
end

select_task = function(state)
    if state.wants_build and not urgent_combat(state) then
        return new_builder_task()
    end

    if combat_active(state) then
        return new_attack_task()
    end

    return new_roam_task()
end

return function(self, ctx)
    local client = self.client
    local team = self.team
    local number = self.number
    local level = sgame.level
    local team_level_data = team_level(team)
    cache.refresh()
    local team_snapshot = cache.team(team)
    local selected_buildable = choose_pve_buildable(team, team_snapshot)

    if should_spawn(self, PMF_QUEUED) then
        clear_alien_target_lock(number)
        clear_alien_combat_latch(number)
        clear_task(number)
        clear_build_failure_state(number)
        return choose_spawn(team, number, team_snapshot, team_level_data, selected_buildable, ctx)
    end

    local bot = self.bot
    local mind = bot.mind

    if not client or not mind then
        print("no client or bot mind")
        return ctx:roam()
    end

    local weapon = client.weapon
    local weapon_attr = weapon and weapons[weapon] or nil
    local builder = is_builder(team, client)
    local wants_build = wants_builder(team, number, team_snapshot, team_level_data, builder, selected_buildable)
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
    local base_rush_score = math.max(ctx.baseRushScore or 0, timed_rush_score(level))

    if not wants_build then
        mind.stuckTimer = level.time
        clear_build_failure_state(number)
        clear_builder_assignment(number)
    end

    local status = unstick(level.time, ctx, mind, enemy, enemy_visible, team, client, builder)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = maybe_retire_builder(team, number, client, builder, wants_build, level, ctx, mind)
    if status ~= STATUS_FAILURE then
        clear_build_failure_state(number)
        return status
    end

    status = maybe_use_medkit(team, client, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    local state = {
        self = self,
        client = client,
        team = team,
        number = number,
        level = level,
        team_level_data = team_level_data,
        team_snapshot = team_snapshot,
        selected_buildable = selected_buildable,
        weapon = weapon,
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
        mind = mind,
    }

    local task = current_task(number)
    if task then
        status = maybe_preempt_task(state, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        task = current_task(number)
        if task then
            status = run_task(task, state, ctx)
            if status ~= STATUS_FAILURE then
                return status
            end
        end
    end

    task = start_task(number, select_task(state))
    status = run_task(task, state, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    clear_task(number)
    return ctx:roam()
end
