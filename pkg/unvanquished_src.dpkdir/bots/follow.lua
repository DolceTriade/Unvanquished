STATUS_FAILURE = 0
STATUS_SUCCESS = 1
STATUS_RUNNING = 2

local PMF_QUEUED = 1 << 12

local common = require("bots/common.lua")
local task_runtime = require("bots/task.lua")
local weapons = Unv.weapons
local is_alien = common.is_alien
local is_human = common.is_human
local should_spawn = common.should_spawn
local target_entity = common.target_entity
local health_fraction = common.health_fraction
local use_medkit_if_low = common.use_medkit_if_low
local recently_attacked = common.recently_attacked
local try_evolve_targets = common.try_evolve_targets
local unstick = common.unstick

local ALIEN_EVOLVE_TARGETS = common.ALIEN_EVOLVE_TARGETS
local TASKS = task_runtime.new_runtime()
local STATE = {}
local FOLLOW_FAILURE_TIMEOUT = 10000
local COMBAT_RETRY_DELAY = 1000

local function default_behavior(team)
    local cvar = is_human(team) and "g_bot_defaultBehaviorHuman" or "g_bot_defaultBehaviorAlien"
    local behavior = Cvar.get(cvar)
    if behavior == "" or not behavior:match("^[%w_-]+$") then
        return "default"
    end
    return behavior
end

local function followed_name(state)
    local client_num = state.mind.userSpecifiedClient
    local followed = client_num and sgame.entity[client_num]
    return followed and followed.client and followed.client.name or "my teammate"
end

local function maybe_use_medkit(team, client, ctx)
    return use_medkit_if_low(team, client, ctx, 50)
end

local function maybe_equip(team, mind, ctx)
    if not is_human(team) then
        return STATUS_FAILURE
    end

    local armoury = mind:closestBuilding("arm")
    if not armoury or not armoury.distance or armoury.distance >= 500 then
        return STATUS_FAILURE
    end

    return ctx:equip()
end

local function maybe_evolve(self, team, client, enemy_visible, ctx)
    if not is_alien(team) or enemy_visible then
        return STATUS_FAILURE
    end

    return try_evolve_targets(self, ctx, client, ALIEN_EVOLVE_TARGETS)
end

local function maybe_fight(team, client, enemy_engaged, ctx)
    if not enemy_engaged then
        return STATUS_FAILURE
    end

    if is_human(team) and client.weapon == "ckit" then
        return STATUS_FAILURE
    end

    return ctx:fight()
end

local function enemy_engaged(state)
    return state.enemy_visible or state.enemy_in_range
end

local function combat_alerted(state)
    return enemy_engaged(state)
        or recently_attacked(state.level, state.mind, 1000)
        or (state.mind.enemyLastSeen > 0
            and state.level.time - state.mind.enemyLastSeen <= 3000)
end

local function maybe_heal(state, ctx)
    if not is_human(state.team) or health_fraction(state.client) >= 2 / 3
        or combat_alerted(state) then
        return STATUS_FAILURE
    end

    return ctx:heal()
end

local function can_fight(state)
    local bot_state = STATE[state.number]
    return not bot_state or not bot_state.retry_at or state.level.time >= bot_state.retry_at
end

local function run_combat(_, state, ctx)
    if not combat_alerted(state) or not can_fight(state) then
        return STATUS_FAILURE
    end

    if is_human(state.team) then
        local status = maybe_use_medkit(state.team, state.client, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    local status = maybe_fight(state.team, state.client, true, ctx)
    if status == STATUS_FAILURE then
        STATE[state.number] = STATE[state.number] or {}
        STATE[state.number].retry_at = state.level.time + COMBAT_RETRY_DELAY
    elseif STATE[state.number] then
        STATE[state.number].retry_at = nil
    end
    return status
end

local COMBAT_TASK = { run = run_combat }

local ESCORT_TASK = {
    should_preempt = function(_, state)
        return combat_alerted(state)
    end,
    run = function(_, state, ctx)
        local status

        if is_human(state.team) then
            status = maybe_use_medkit(state.team, state.client, ctx)
            if status ~= STATUS_FAILURE then
                return status
            end

            status = maybe_equip(state.team, state.mind, ctx)
            if status == STATUS_RUNNING then
                return status
            end

            status = maybe_heal(state, ctx)
            if status ~= STATUS_FAILURE then
                return status
            end
        else
            status = maybe_evolve(state.self, state.team, state.client, state.enemy_visible, ctx)
            if status ~= STATUS_FAILURE then
                return status
            end
        end

        if enemy_engaged(state) and can_fight(state) then
            return ctx:fight()
        end

        status = ctx:follow(250)
        if status ~= STATUS_FAILURE then
            STATE[state.number] = nil
            return status
        end

        local bot_state = STATE[state.number]
        if not bot_state then
            bot_state = {}
            STATE[state.number] = bot_state
        end
        bot_state.follow_failed_at = bot_state.follow_failed_at or state.level.time

        if state.level.time - bot_state.follow_failed_at < FOLLOW_FAILURE_TIMEOUT then
            return ctx:roam()
        end

        STATE[state.number] = nil
        ctx:say(("I failed to follow %s^*. Switching to default behavior."):format(followed_name(state)), 1) -- 1 == SAY_TEAM
        return ctx:changeBehavior(default_behavior(state.team))
    end,
}

local function select_task(state)
    if combat_alerted(state) and can_fight(state) then
        return COMBAT_TASK
    end

    return ESCORT_TASK
end

return function(self, ctx)
    if should_spawn(self, PMF_QUEUED) then
        return common.spawn_as_team_default(self.team, ctx, "level0", "rifle")
    end

    local client = self.client
    if not client or not self.bot or not self.bot.mind then
        return ctx:roam()
    end

    local mind = self.bot.mind
    local enemy = target_entity(mind.bestEnemy)
    local enemy_visible = enemy and ctx:isVisibleEntity(enemy) or false
    local enemy_in_range = enemy and mind.bestEnemy.distance
        and mind.bestEnemy.distance <= 500 or false
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
        enemy_in_range = enemy_in_range,
        weapon_attr = weapon_attr,
    }

    local status = unstick(sgame.level.time, ctx, mind, enemy, enemy_visible, self.team, client)
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
