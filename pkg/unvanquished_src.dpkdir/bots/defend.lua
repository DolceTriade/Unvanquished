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
local elapsed_since = common.elapsed_since
local health_fraction = common.health_fraction
local use_medkit_if_low = common.use_medkit_if_low
local try_evolve_targets = common.try_evolve_targets
local heal_to_full_unless_attacked = common.heal_to_full_unless_attacked
local human_repair_target_info = common.human_repair_target_info
local roam_buildings = common.roam_buildings
local unstick = common.unstick
local TASKS = task_runtime.new_runtime()
local STATE = {
    human_equip_retry_at = {},
}

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

local function buildable_health_fraction(entity)
    local buildable = entity and entity.buildable or nil
    local attr = buildable and Unv.buildables[buildable.name] or nil
    if not buildable or not attr or attr.health <= 0 then
        return 1
    end

    return buildable.health / attr.health
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

local function maybe_heal_human(client, mind, enemy_visible, ctx)
    return heal_to_full_unless_attacked(sgame.level, mind, client, ctx, {
        full_health = 100,
        attacked_window = 2000,
        heal_anchor = "medistat",
        heal_radius = 250,
    })
end

local function repair_target_info(mind)
    return human_repair_target_info(mind)
end

local function maybe_retire_repair_kit(team, client, mind, ctx)
    if not is_human(team) or client.weapon ~= "ckit" then
        return STATUS_FAILURE
    end

    if repair_target_info(mind) then
        return STATUS_FAILURE
    end

    return ctx:equip()
end

local function maybe_repair(team, number, client, ctx, mind)
    if not is_human(team) then
        return STATUS_FAILURE
    end

    local repair_target = repair_target_info(mind)
    if not repair_target then
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

local function maybe_reload(team, client, weapon_attr, enemy_visible, level, mind, ctx)
    if not is_human(team)
        or enemy_visible
        or not weapon_attr
        or weapon_attr.ammo <= 0
        or elapsed_since(level.time, mind.enemyLastSeen) <= 3000 then
        return STATUS_FAILURE
    end

    if (client.ammo or 0) / weapon_attr.ammo >= 0.4 then
        return STATUS_FAILURE
    end

    return ctx:reload()
end

local function maybe_equip(team, enemy_visible, level, mind, ctx)
    if not is_human(team)
        or enemy_visible
        or elapsed_since(level.time, mind.enemyLastSeen) <= 1000 then
        return STATUS_FAILURE
    end

    local armoury = mind:closestBuilding("arm")
    if not armoury or not armoury.distance or armoury.distance >= 500 then
        return STATUS_FAILURE
    end

    if armoury.distance > 100 then
        return ctx:moveTo("arm")
    end

    return ctx:equip()
end

local function maybe_fight_human(team, client, enemy_visible, attack_active, mind, ctx)
    if not is_human(team) or client.weapon == "ckit" or not attack_active then
        return STATUS_FAILURE
    end

    local reactor_distance = building_distance(mind, "reactor")
    local medistat_distance = building_distance(mind, "medistat")
    if (reactor_distance and reactor_distance <= 300) or (medistat_distance and medistat_distance <= 250) then
        return ctx:fight()
    end

    if client.health < 60 then
        local status = maybe_heal_human(client, mind, enemy_visible, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    return ctx:fight()
end

local function maybe_fight_or_heal_alien(client, enemy_visible, ctx, mind)
    local overmind_distance = building_distance(mind, "overmind")
    local booster_distance = building_distance(mind, "booster")
    local safe_heal_zone = (overmind_distance and overmind_distance <= 200)
        or (booster_distance and booster_distance <= 200)
    local recently_attacked = common.recently_attacked(sgame.level, mind, 2000)
    local needs_healing = health_fraction(client) < 1.0

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

    if enemy_visible then
        if health_fraction(client) < 0.4
            and not safe_heal_zone
            and not recently_attacked
            and (ctx.baseRushScore or 0) < 1.0 then
            local status = ctx:heal()
            if status ~= STATUS_FAILURE then
                return status
            end
        end

        return ctx:fight()
    end

    if health_fraction(client) < 0.4 and not safe_heal_zone and not recently_attacked then
        return ctx:heal()
    end

    return STATUS_FAILURE
end

local function maybe_defend_as_alien(client, attack_active, enemy_visible, ctx, mind)
    local status = maybe_fight_or_heal_alien(client, enemy_visible, ctx, mind)
    if status ~= STATUS_FAILURE then
        return status
    end

    if attack_active then
        return ctx:fight()
    end

    return STATUS_FAILURE
end

local function roam_friendly_base(team, ctx)
    if is_alien(team) then
        local status = roam_buildings(ctx, { "overmind", "eggpod", "booster" }, 500)
        return status ~= STATUS_FAILURE and status or ctx:roam()
    end

    if is_human(team) then
        local status = roam_buildings(ctx, { "reactor", "telenode", "arm", "medistat" }, 500)
        return status ~= STATUS_FAILURE and status or ctx:roam()
    end

    return STATUS_FAILURE
end

local function near_friendly_base(state)
    if is_human(state.team) then
        local reactor_distance = building_distance(state.mind, "reactor")
        local sense_range = cache.cvar_number("g_bot_humansenseRange")
        return reactor_distance and sense_range > 0 and reactor_distance <= sense_range or false
    end

    if is_alien(state.team) then
        local overmind_distance = building_distance(state.mind, "overmind")
        local sense_range = cache.cvar_number("g_bot_aliensenseRange")
        return overmind_distance and sense_range > 0 and overmind_distance <= sense_range or false
    end

    return false
end

local function defend_attack_active(state)
    if not near_friendly_base(state) then
        return state.enemy_visible
    end

    if state.enemy_visible then
        return true
    end

    if state.enemy and state.enemy_target and state.enemy_target.distance
        and state.enemy_target.distance <= 700 then
        return true
    end

    return state.enemy ~= nil
        and state.mind.enemyLastSeen ~= nil
        and state.mind.enemyLastSeen > 0
        and elapsed_since(state.level.time, state.mind.enemyLastSeen) <= 1500
end

local function alien_support_needed(client, mind)
    if client.class == "builderupg" and cache.cvar("g_bot_extinguishFire") ~= "0" then
        return true
    end

    if health_fraction(client) < 1.0 and not common.recently_attacked(sgame.level, mind, 2000) then
        return true
    end

    return common.best_alien_evolve_target(nil, client, sgame.level, ALIEN_EVOLVE_TARGETS) ~= nil
end

local function human_should_retire_repair_kit(team, client, mind)
    return is_human(team) and client.weapon == "ckit" and repair_target_info(mind) == nil
end

local function human_should_heal(team, client)
    return is_human(team) and client.health < 100
end

local function human_should_reload(team, client, weapon_attr, enemy_visible, level, mind)
    return is_human(team)
        and not enemy_visible
        and weapon_attr ~= nil
        and weapon_attr.ammo > 0
        and elapsed_since(level.time, mind.enemyLastSeen) > 3000
        and (client.ammo or 0) / weapon_attr.ammo < 0.4
end

local function human_should_equip(number, team, enemy_visible, level, mind)
    if not is_human(team)
        or enemy_visible
        or elapsed_since(level.time, mind.enemyLastSeen) <= 1000 then
        return false
    end

    local retry_at = STATE.human_equip_retry_at[number]
    if retry_at and retry_at > level.time then
        return false
    end

    local armoury = mind:closestBuilding("arm")
    return armoury ~= nil and armoury.distance ~= nil and armoury.distance < 500
end

local function human_support_needed(number, team, client, weapon_attr, enemy_visible, level, mind)
    if human_should_heal(team, client) then
        return true
    end

    if human_should_retire_repair_kit(team, client, mind) then
        return true
    end

    if repair_target_info(mind) then
        return true
    end

    if human_should_reload(team, client, weapon_attr, enemy_visible, level, mind) then
        return true
    end

    return human_should_equip(number, team, enemy_visible, level, mind)
end

local COMBAT_TASK = {
    run = function(_, state, ctx)
        if not defend_attack_active(state) then
            return STATUS_FAILURE
        end

        if is_alien(state.team) then
            return maybe_defend_as_alien(state.client, defend_attack_active(state),
                state.enemy_visible, ctx, state.mind)
        end

        return maybe_fight_human(state.team, state.client, state.enemy_visible,
            defend_attack_active(state), state.mind, ctx)
    end,
}

local REPAIR_HEAL_TASK = {
    should_preempt = function(_, state)
        return defend_attack_active(state)
    end,
    run = function(_, state, ctx)
        local status = nil

        if is_alien(state.team) then
            status = maybe_extinguish_fire(state.team, state.client, ctx)
            if status ~= STATUS_FAILURE then
                return status
            end

            status = maybe_evolve(state.self, state.team, state.client, state.enemy_visible, ctx)
            if status ~= STATUS_FAILURE then
                return status
            end

            return maybe_defend_as_alien(state.client, defend_attack_active(state),
                state.enemy_visible, ctx, state.mind)
        end

        status = maybe_heal_human(state.client, state.mind, state.enemy_visible, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_repair(state.team, state.number, state.client, ctx, state.mind)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_retire_repair_kit(state.team, state.client, state.mind, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_reload(state.team, state.client, state.weapon_attr, state.enemy_visible,
            state.level, state.mind, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_equip(state.team, state.enemy_visible, state.level, state.mind, ctx)
        if status == STATUS_FAILURE then
            STATE.human_equip_retry_at[state.number] = state.level.time + 2000
            return STATUS_FAILURE
        end

        STATE.human_equip_retry_at[state.number] = nil
        return status
    end,
}

local HOLD_TASK = {
    should_preempt = function(_, state)
        return defend_attack_active(state)
    end,
    run = function(_, state, ctx)
        return roam_friendly_base(state.team, ctx)
    end,
}

local function select_task(state)
    if defend_attack_active(state) then
        return COMBAT_TASK
    end

    if is_alien(state.team) then
        if alien_support_needed(state.client, state.mind) then
            return REPAIR_HEAL_TASK
        end
    elseif human_support_needed(state.number, state.team, state.client, state.weapon_attr,
        state.enemy_visible, state.level, state.mind) then
        return REPAIR_HEAL_TASK
    end

    return HOLD_TASK
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

    local enemy = target_entity(mind.bestEnemy)
    if enemy and not entity_is_alive(enemy) then
        enemy = nil
    end
    local enemy_visible = enemy and ctx:isVisibleEntity(enemy) or false
    local weapon_attr = weapons[client.weapon]
    local state = {
        self = self,
        team = self.team,
        number = self.number,
        level = sgame.level,
        client = client,
        mind = mind,
        enemy_target = mind.bestEnemy,
        enemy = enemy,
        enemy_visible = enemy_visible,
        weapon_attr = weapon_attr,
    }
    local status = unstick(sgame.level.time, ctx, mind, enemy, enemy_visible, self.team, client)
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
