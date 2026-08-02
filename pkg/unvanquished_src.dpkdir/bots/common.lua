local M = {}
local RUNNING_ACTIONS = {}
local EVOLVE_STATE = {}
local SIDEGRADE_HYSTERESIS_MS = 15000

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

local function credits_per_evo()
    return Gameplay.CREDITS_PER_EVO or 100
end

local function level_time()
    return sgame and sgame.level and sgame.level.time or 0
end

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

function M.alive(self)
    local client = self.client
    return client and client.health > 0
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

function M.evolve_cost_evos(current_name, target_name)
    local current = M.class_attr(current_name)
    local target = M.class_attr(target_name)
    if not current or not target then
        return nil
    end

    return (target.price - current.price) / credits_per_evo()
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

    local evolve_cost = M.evolve_cost_evos(client.class, class_name)
    local available_evos = client.evos or 0
    local unlock_threshold = target.unlock_threshold or 0
    local overload_progress = level and level.overload_progress or 0

    if not evolve_cost or evolve_cost <= 0 or available_evos < evolve_cost then
        return false
    end

    return overload_progress >= unlock_threshold
end

function M.best_alien_evolve_target(self, client, level, targets)
    local now = level_time()
    local number = bot_number(self)
    local current = M.class_attr(client.class)
    local state = nil

    if number then
        state = EVOLVE_STATE[number]
        if not state then
            state = {
                class_name = client.class,
                sidegrade_until = 0,
            }
            EVOLVE_STATE[number] = state
        elseif state.class_name ~= client.class then
            local previous = M.class_attr(state.class_name)
            if previous and current and previous.price == current.price then
                state.sidegrade_until = now + SIDEGRADE_HYSTERESIS_MS
            else
                state.sidegrade_until = 0
            end

            state.class_name = client.class
        end
    end

    for _, class_name in ipairs(targets or M.ALIEN_EVOLVE_TARGETS) do
        local target = M.class_attr(class_name)
        local sidegrade_blocked = state
            and current
            and target
            and client.class ~= class_name
            and current.price == target.price
            and state.sidegrade_until > now

        if not sidegrade_blocked and M.can_evolve_to_class(self, client, level, class_name) then
            return class_name
        end
    end

    return nil
end

function M.try_evolve_targets(self, ctx, client, targets)
    local state = nil
    local current = M.class_attr(client.class)
    local now = level_time()
    local number = nil

    if type(self) == "number" then
        number = self
    elseif self then
        number = self.number
    end

    if number then
        state = EVOLVE_STATE[number]
        if not state then
            state = {
                class_name = client.class,
                sidegrade_until = 0,
            }
            EVOLVE_STATE[number] = state
        elseif state.class_name ~= client.class then
            local previous = M.class_attr(state.class_name)
            if previous and current and previous.price == current.price then
                state.sidegrade_until = now + SIDEGRADE_HYSTERESIS_MS
            else
                state.sidegrade_until = 0
            end

            state.class_name = client.class
        end
    end

    for _, class_name in ipairs(targets or M.ALIEN_EVOLVE_TARGETS) do
        local target = M.class_attr(class_name)
        local sidegrade_blocked = state
            and current
            and target
            and client.class ~= class_name
            and current.price == target.price
            and state.sidegrade_until > now

        if client.class ~= class_name and not sidegrade_blocked and ctx:canEvolveTo(class_name) then
            return ctx:evolveTo(class_name)
        end
    end

    return STATUS_FAILURE
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

local function buildable_health_fraction(entity)
    local buildable = entity and entity.buildable or nil
    local attr = buildable and Unv.buildables[buildable.name] or nil
    if not buildable or not attr or attr.health <= 0 then
        return 1
    end

    return buildable.health / attr.health
end

local function goal_reached(ctx, goal, radius)
    if not goal then
        return false
    end

    local entity = M.target_entity(goal)
    local distance = nil
    if entity then
        distance = ctx:distanceToEntity(entity)
    elseif goal.kind == "coordinates"
        and goal.positionX ~= nil
        and goal.positionY ~= nil
        and goal.positionZ ~= nil then
        distance = ctx:distanceToPosition(goal.positionX, goal.positionY, goal.positionZ)
    elseif goal.kind == "coordinates" and goal.position ~= nil then
        distance = ctx:distanceToPosition(goal.position)
    end

    return distance and distance <= radius or false
end

function M.unstick(now, ctx, mind, enemy, enemy_visible, team, client, builder)
    builder = builder or false
    local stuck_for = M.elapsed_since(now, mind and mind.stuckTime or 0)

    if stuck_for <= 10000 then
        return STATUS_FAILURE
    end

    if mind.goal.kind == "empty" then
        ctx:resetStuckTime()
        return STATUS_FAILURE
    end

    if goal_reached(ctx, mind.goal, 96) then
        ctx:resetStuckTime()
        return STATUS_FAILURE
    end

    if enemy and enemy_visible and ctx:inAttackRangeEntity(enemy) then
        ctx:resetStuckTime()
        return STATUS_FAILURE
    end

    if M.is_human(team) and client and client.weapon == "ckit"
        and mind.goal.distance and mind.goal.distance <= 100 then
        local goal_entity = M.target_entity(mind.goal)
        if goal_entity and buildable_health_fraction(goal_entity) < 1 then
            ctx:resetStuckTime()
            return STATUS_FAILURE
        end
    end

    if M.is_alien(team) and client and client.health < M.class_health(client.class) then
        local overmind_distance = M.building_distance(mind, "overmind")
        local booster_distance = M.building_distance(mind, "booster")
        if (overmind_distance and overmind_distance <= 128)
            or (booster_distance and booster_distance <= 200) then
            ctx:resetStuckTime()
            return STATUS_FAILURE
        end
    end

    if builder then
        local anchor_name = M.is_alien(team) and "overmind" or M.is_human(team) and "reactor" or nil
        local anchor_target = anchor_name and mind:closestBuilding(anchor_name) or nil
        if anchor_target and anchor_target.distance and anchor_target.distance <= 700 then
            ctx:resetStuckTime()
            return STATUS_FAILURE
        end

        if mind.closestDamagedBuilding.kind == "entity"
            and mind.closestDamagedBuilding.distance
            and mind.closestDamagedBuilding.distance <= 192 then
            ctx:resetStuckTime()
            return STATUS_FAILURE
        end
    end

    if stuck_for > 45000 then
        local status = ctx:moveTo("self")
        if status ~= STATUS_FAILURE then
            return status
        end

        return ctx:moveInDir("forward")
    end

    if stuck_for > 17500 then
        return ctx:moveInDir("forward")
    end

    if stuck_for > 15000 then
        return ctx:moveInDir("right")
    end

    if stuck_for > 12500 then
        return ctx:moveInDir("backward")
    end

    local jump_window = math.floor((stuck_for - 10000) / 3000)
    if jump_window >= 1 and (stuck_for % 3000) < 250 then
        local status = ctx:jump()
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    local status = ctx:moveInDir("left")
    if status ~= STATUS_FAILURE then
        return status
    end

    return ctx:moveTo("self")
end

function M.best_alien_combat_target(self, client, targets)
    local current = M.class_attr(client.class)
    if not current then
        return nil
    end

    local now = level_time()
    local number = bot_number(self)
    local state = nil
    if number then
        state = EVOLVE_STATE[number]
        if not state then
            state = {
                class_name = client.class,
                sidegrade_until = 0,
            }
            EVOLVE_STATE[number] = state
        elseif state.class_name ~= client.class then
            local previous = M.class_attr(state.class_name)
            if previous and previous.price == current.price then
                state.sidegrade_until = now + SIDEGRADE_HYSTERESIS_MS
            else
                state.sidegrade_until = 0
            end

            state.class_name = client.class
        end
    end

    local available_evos = client.evos + (current.price / credits_per_evo())
    for _, class_name in ipairs(targets or M.ALIEN_COMBAT_TARGETS) do
        local target = M.class_attr(class_name)
        local target_cost = target and (target.price / credits_per_evo()) or nil
        local sidegrade_blocked = state
            and target
            and class_name ~= client.class
            and current.price == target.price
            and state.sidegrade_until > now

        if target
            and class_name ~= client.class
            and not sidegrade_blocked
            and available_evos >= target_cost then
            return class_name
        end
    end

    return nil
end

return M
