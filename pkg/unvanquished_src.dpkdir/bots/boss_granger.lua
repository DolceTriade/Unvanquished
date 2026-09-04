STATUS_FAILURE = 0
STATUS_SUCCESS = 1
STATUS_RUNNING = 2

package.loaded["bots/common.lua"] = nil
package.loaded["bots/boss.lua"] = nil

local common = require("bots/common.lua")
local boss = require("bots/boss.lua")
local task_runtime = require("bots/task.lua")

local random = math.random
local target_entity = common.target_entity
local unstick = common.unstick

local TASKS = task_runtime.new_runtime()
local BLOB_STANDOFF_DISTANCE = 180
local RANDOM_LOOK_INTERVAL_MS = 250
local RECOVERY_MOVE_DURATION_MS = 500
local RECOVERY_MIN_PROGRESS_DISTANCE = 24
local RECOVERY_JUMP_DELAY_MS = 200
local DETOUR_DURATION_MS = 1000
local STANDOFF_RETRY_MS = 400
local STANDOFF_MIN_SEPARATION_GAIN = 24
local RECOVERY_ABORT_ATTEMPTS = 3
local TARGET_ABANDON_DURATION_MS = 4000
local LOOK_STATE = {}
local RECOVERY_DIRS = { "left", "right", "backward", "forward" }


local function always_fire(state, ctx)
    if state.enemy_visible and state.enemy then
        ctx:aimAtGoal()
    elseif state.mind.goal and state.mind.goal.isValid and state.mind.goal:isValid() then
        ctx:aimAtGoal()
    else
        local look = LOOK_STATE[state.number]
        local now = state.level.time
        if not look then
            look = {}
            LOOK_STATE[state.number] = look
        end

        if not look.next_turn_at or now >= look.next_turn_at then
            x, y, z = table.unpack(state.client.viewangles)
            state.client.viewangles = {
                random() * -90,
                random() * 360,
                z,
            }
            look.next_turn_at = now + RANDOM_LOOK_INTERVAL_MS
        end
    end

    ctx:fireTertiary()
end

local function distance_between(a, b)
    if not a or not b then
        return 0
    end

    local dx = (a[1] or 0) - (b[1] or 0)
    local dy = (a[2] or 0) - (b[2] or 0)
    local dz = (a[3] or 0) - (b[3] or 0)
    return math.sqrt(dx * dx + dy * dy + dz * dz)
end

local function abandon_enemy(task, state, ctx, reason)
    if not state.enemy then
        return
    end

    ctx:abandonEnemy(state.enemy, TARGET_ABANDON_DURATION_MS)
end

local function clear_stuck_state(task)
    task.mode = nil
    task.mode_until = nil
    task.mode_started_at = nil
    task.mode_origin = nil
    task.mode_distance = nil
    task.mode_dir = nil
    task.mode_jumped = nil
end

local function start_mode(task, mode, now, origin, extra)
    extra = extra or {}
    task.mode = mode
    task.mode_started_at = now
    task.mode_until = now + extra.duration
    task.mode_origin = origin
    task.mode_distance = extra.distance
    task.mode_dir = extra.dir
    task.mode_jumped = false
end

local function note_progress(task, state)
    if not task.mode_origin then
        return
    end

    local moved = distance_between(state.self.origin, task.mode_origin)
    if moved >= RECOVERY_MIN_PROGRESS_DISTANCE then
        task.stuck_cycles = 0
    end
end

local function pick_recovery_dir(task)
    local last_dir = task.last_recovery_dir
    if not last_dir then
        return RECOVERY_DIRS[random(#RECOVERY_DIRS)]
    end

    local options = {}
    for _, dir in ipairs(RECOVERY_DIRS) do
        if dir ~= last_dir then
            options[#options + 1] = dir
        end
    end

    if #options == 0 then
        return last_dir
    end

    return options[random(#options)]
end

local function begin_recovery(task, state)
    start_mode(task, "recover", state.level.time, state.self.origin, {
        duration = RECOVERY_MOVE_DURATION_MS,
        dir = pick_recovery_dir(task),
    })
end

local function run_stuck_mode(task, state, ctx)
    if task.mode == "standoff" then
        local enemy_distance = ctx:distanceToEntity(state.enemy)
        if task.mode_until <= state.level.time then
            local gained = enemy_distance - (task.mode_distance or enemy_distance)
            clear_stuck_state(task)
            if gained < STANDOFF_MIN_SEPARATION_GAIN then
                begin_recovery(task, state)
                return run_stuck_mode(task, state, ctx)
            end
            return STATUS_FAILURE
        end

        ctx:moveInDir("backward", 1)
        always_fire(state, ctx)
        return STATUS_RUNNING
    end

    if task.mode == "recover" then
        local moved = distance_between(state.self.origin, task.mode_origin)
        local elapsed = state.level.time - (task.mode_started_at or state.level.time)
        local dir = task.mode_dir

        if task.mode_until <= state.level.time then
            task.last_recovery_dir = dir
            if moved < RECOVERY_MIN_PROGRESS_DISTANCE then
                task.stuck_cycles = (task.stuck_cycles or 0) + 1
                if task.stuck_cycles >= RECOVERY_ABORT_ATTEMPTS then
                    abandon_enemy(task, state, ctx, "stuck_recovery")
                    clear_stuck_state(task)
                    task.stuck_cycles = 0
                    return STATUS_FAILURE
                end
            else
                task.stuck_cycles = 0
            end

            start_mode(task, "detour", state.level.time, state.self.origin, {
                duration = DETOUR_DURATION_MS,
            })
            return run_stuck_mode(task, state, ctx)
        end

        if not task.mode_jumped and elapsed >= RECOVERY_JUMP_DELAY_MS and moved < RECOVERY_MIN_PROGRESS_DISTANCE then
            task.mode_jumped = true
            ctx:jumpInDir(dir, 1)
            always_fire(state, ctx)
            return STATUS_RUNNING
        end

        ctx:moveInDir(dir, 1)
        always_fire(state, ctx)
        return STATUS_RUNNING
    end

    if task.mode == "detour" then
        if task.mode_until <= state.level.time then
            clear_stuck_state(task)
            return STATUS_FAILURE
        end

        ctx:roam()
        always_fire(state, ctx)
        return STATUS_RUNNING
    end

    return STATUS_FAILURE
end

local function run_standoff(task, state, ctx)
    if task.mode ~= "standoff" then
        start_mode(task, "standoff", state.level.time, state.self.origin, {
            duration = STANDOFF_RETRY_MS,
            distance = ctx:distanceToEntity(state.enemy),
        })
    end

    return run_stuck_mode(task, state, ctx)
end

local function run_recovery(task, state, ctx)
    if not task.mode then
        begin_recovery(task, state)
    end

    return run_stuck_mode(task, state, ctx)
end

local function try_roam(task, state, ctx, anchor, radius)
    local status = anchor and ctx:roamInRadius(anchor, radius) or ctx:roam()
    if status ~= STATUS_FAILURE then
        clear_stuck_state(task)
        return status
    end

    if task.mode == "detour" then
        return STATUS_FAILURE
    end

    return run_recovery(task, state, ctx)
end

local function boss_attack_task_run(task, state, ctx)
    local enemy = state.enemy
    if not enemy then
        return STATUS_FAILURE
    end

    note_progress(task, state)
    if task.mode and task.mode ~= "standoff" then
        local status = run_stuck_mode(task, state, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    -- Keep a standoff distance so the boss can continuously spit slowblobs.
    if state.enemy_visible then
        local enemy_distance = ctx:distanceToEntity(enemy)
        if enemy_distance < BLOB_STANDOFF_DISTANCE then
            return run_standoff(task, state, ctx)
        end

        if task.mode == "standoff" then
            clear_stuck_state(task)
        end

        always_fire(state, ctx)
        local status = ctx:moveTo("enemy")
        if status ~= STATUS_FAILURE then
            clear_stuck_state(task)
            return status
        end

        return run_recovery(task, state, ctx)
    end

    -- Not visible: advance toward it.
    local status = ctx:moveTo("enemy")
    if status ~= STATUS_FAILURE then
        clear_stuck_state(task)
        return status
    end
    return run_recovery(task, state, ctx)
end

local function boss_attack_should_preempt(_, state)
    return not state.enemy
end

local BOSS_ATTACK_TASK = {
    run = boss_attack_task_run,
    should_preempt = boss_attack_should_preempt,
}

-- ---------------------------------------------------------------------------
-- Task: patrol the home territory, regen when hurt, spit at distant enemies
-- ---------------------------------------------------------------------------

local function boss_patrol_task_run(task, state, ctx)
    local status = unstick(state.level.time, ctx, state.mind, state.enemy,
        state.enemy_visible, state.team, state.client)
    if status ~= STATUS_FAILURE then
        return status
    end

    note_progress(task, state)
    if task.mode and task.mode ~= "standoff" then
        local status = run_stuck_mode(task, state, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    -- Regen near overmind when hurt (but don't camp there permanently).
    if state.client.health < common.class_health(state.client.class) then
        status = ctx:heal()
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    -- Keep firing while moving; only fire when aiming at a live entity goal.
    always_fire(state, ctx)

    -- Roam the home area: cycle between key alien buildings, wandering the
    -- map in between so the boss is not glued to a single spot.
    status = try_roam(task, state, ctx, "overmind", 5000)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = try_roam(task, state, ctx, "eggpod", 5000)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = try_roam(task, state, ctx, "booster", 5000)
    if status ~= STATUS_FAILURE then
        return status
    end

    return try_roam(task, state, ctx)
end

local function boss_patrol_should_preempt(_, state)
    -- Go hunt if we sense a best enemy (aliens sense at range, not just LOS).
    return state.enemy ~= nil
end

local BOSS_PATROL_TASK = {
    run = boss_patrol_task_run,
    should_preempt = boss_patrol_should_preempt,
}

local function select_task(state)
    -- Hunt best enemy whether or not we can currently see it.
    if state.enemy then
        return BOSS_ATTACK_TASK
    end

    return BOSS_PATROL_TASK
end

-- ---------------------------------------------------------------------------
-- Behavior entry point
-- ---------------------------------------------------------------------------

return function(self, ctx)
    local client = self.client
    local team = self.team
    local number = self.number
    local level = sgame.level

    local status = boss.prepare("granger", self, ctx)
    if status ~= nil then
        return status
    end

    if not client or not self.bot or not self.bot.mind then
        return ctx:roam()
    end

    local mind = self.bot.mind

    local enemy = target_entity(mind.bestEnemy)
    local enemy_visible = enemy and ctx:isVisibleEntity(enemy) or false

    local state = {
        self = self,
        team = team,
        number = number,
        level = level,
        client = client,
        mind = mind,
        enemy = enemy,
        enemy_visible = enemy_visible,
    }

    local task = TASKS.current(number)
    if task then
        local status = TASKS.maybe_preempt(state, ctx, select_task)
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
