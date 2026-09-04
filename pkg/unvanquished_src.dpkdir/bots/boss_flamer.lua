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
local RANDOM_LOOK_INTERVAL_MS = 200
local TAUNT_DURATION_MS = 2500
local RECOVERY_MOVE_DURATION_MS = 500
local RECOVERY_MIN_PROGRESS_DISTANCE = 24
local RECOVERY_JUMP_DELAY_MS = 200
local DETOUR_DURATION_MS = 1000
local RECOVERY_ABORT_ATTEMPTS = 3
local TARGET_ABANDON_DURATION_MS = 4000
local YAW_SWEEP_DEGREES = 45
local YAW_SWEEP_INTERVAL_MS = 90
local PITCH_SWEEP_DEGREES = 18
local PITCH_SWEEP_INTERVAL_MS = 180
local LOOK_STATE = {}
local RECOVERY_DIRS = { "left", "right", "backward", "forward" }
local TAUNTS = {
    "Catch your breath. I am.",
    "Too slow.",
    "Still standing?",
}

local function clamp(value, min_value, max_value)
    if value < min_value then
        return min_value
    end

    if value > max_value then
        return max_value
    end

    return value
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

local function spin_aim(state, ctx)
    local goal = state.mind.goal
    if state.enemy_visible and state.enemy then
        ctx:aimAtGoal()
    elseif goal and goal.isValid and goal:isValid() then
        ctx:aimAtGoal()
    else
        local look = LOOK_STATE[state.number]
        local now = state.level.time
        if not look then
            look = {}
            LOOK_STATE[state.number] = look
        end

        if not look.next_turn_at or now >= look.next_turn_at then
            local pitch, _, roll = table.unpack(state.client.viewangles)
            state.client.viewangles = {
                clamp((pitch or 0) + random(-20, 20), -70, 70),
                random() * 360,
                roll,
            }
            look.next_turn_at = now + RANDOM_LOOK_INTERVAL_MS
        end
    end

    local pitch, yaw, roll = table.unpack(state.client.viewangles)
    local yaw_dir = math.floor(state.level.time / YAW_SWEEP_INTERVAL_MS) % 2 == 0 and 1 or -1
    local pitch_dir = math.floor(state.level.time / PITCH_SWEEP_INTERVAL_MS) % 2 == 0 and 1 or -1
    state.client.viewangles = {
        clamp((pitch or 0) + (PITCH_SWEEP_DEGREES * pitch_dir), -75, 75),
        (yaw or 0) + (YAW_SWEEP_DEGREES * yaw_dir),
        roll,
    }
end

local function always_fire(state, ctx)
    spin_aim(state, ctx)
    ctx:fire()
end

local function abandon_enemy(task, state, ctx)
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
    task.mode_dir = nil
    task.mode_jumped = nil
end

local function start_mode(task, mode, now, origin, extra)
    extra = extra or {}
    task.mode = mode
    task.mode_started_at = now
    task.mode_until = now + extra.duration
    task.mode_origin = origin
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
    if task.mode == "recover" then
        local moved = distance_between(state.self.origin, task.mode_origin)
        local elapsed = state.level.time - (task.mode_started_at or state.level.time)
        local dir = task.mode_dir

        if task.mode_until <= state.level.time then
            task.last_recovery_dir = dir
            if moved < RECOVERY_MIN_PROGRESS_DISTANCE then
                task.stuck_cycles = (task.stuck_cycles or 0) + 1
                if task.stuck_cycles >= RECOVERY_ABORT_ATTEMPTS then
                    abandon_enemy(task, state, ctx)
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

local function maybe_switch_phase(task, now, ctx)
    if task.phase == "taunt" then
        if now - (task.phase_started_at or now) < TAUNT_DURATION_MS then
            return false
        end

        task.phase_refilled = nil
        task.phase = "spin"
        task.phase_started_at = now
        return false
    end

    if (task.last_known_ammo or 0) > 0 then
        return false
    end

    task.phase = "taunt"
    task.phase_started_at = now
    task.taunted_at = now
    ctx:say(TAUNTS[random(#TAUNTS)])
    return true
end

local function run_spin_phase(task, state, ctx)
    local status = STATUS_FAILURE
    if state.enemy_visible then
        status = ctx:alternateStrafe()
        if status == STATUS_FAILURE then
            status = ctx:fight()
        end
    end

    if status == STATUS_FAILURE then
        status = ctx:moveTo("enemy")
    end

    if status == STATUS_FAILURE then
        local dir = (math.floor(state.level.time / 350) % 2 == 0) and "left" or "right"
        status = ctx:moveInDir(dir, 1)
        if status == STATUS_FAILURE then
            return run_recovery(task, state, ctx)
        end
    end

    clear_stuck_state(task)
    always_fire(state, ctx)
    return STATUS_RUNNING
end

local function run_patrol_spin(task, state, ctx)
    -- No sensed enemy yet: push toward the enemy base instead of patrolling
    -- friendly buildings. The rush action creates the engine's preferred
    -- aggressive target and keeps the boss moving into enemy territory.
    local status = ctx:rush()
    if status == STATUS_FAILURE then
        status = try_roam(task, state, ctx)
    end
    if status == STATUS_FAILURE then
        local dir = (math.floor(state.level.time / 350) % 2 == 0) and "left" or "right"
        status = ctx:moveInDir(dir, 1)
        if status == STATUS_FAILURE then
            return run_recovery(task, state, ctx)
        end
    end

    clear_stuck_state(task)
    always_fire(state, ctx)
    return STATUS_RUNNING
end

local function run_taunt_phase(task, state, ctx)
    if not task.taunted_at or task.taunted_at ~= task.phase_started_at then
        task.taunted_at = task.phase_started_at
        ctx:say(TAUNTS[random(#TAUNTS)])
    end

    if not task.phase_refilled then
        state.client:forceweapon("flamer")
        task.phase_refilled = true
    end

    if state.enemy_visible and state.enemy then
        ctx:aimAtGoal()
    end

    return STATUS_RUNNING
end

local function sustain_spin(state, ctx, move_status)
    if state.enemy_visible then
        ctx:alternateStrafe()
    else
        local dir = (math.floor(state.level.time / 350) % 2 == 0) and "left" or "right"
        ctx:moveInDir(dir, 1)
    end

    always_fire(state, ctx)
    return move_status
end

local function boss_attack_task_run(task, state, ctx)
    local enemy = state.enemy
    if not enemy then
        return STATUS_FAILURE
    end

    local status = unstick(state.level.time, ctx, state.mind, state.enemy,
        state.enemy_visible, state.team, state.client)
    if status ~= STATUS_FAILURE then
        return sustain_spin(state, ctx, status)
    end

    note_progress(task, state)
    if task.mode then
        status = run_stuck_mode(task, state, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    local now = state.level.time
    task.last_known_ammo = state.client.ammo or 0
    if not task.phase then
        task.phase = "spin"
        task.phase_started_at = now
    end

    local switched = maybe_switch_phase(task, now, ctx)
    if task.phase == "taunt" then
        return run_taunt_phase(task, state, ctx)
    end

    if switched then
        return STATUS_RUNNING
    end

    return run_spin_phase(task, state, ctx)
end

local function boss_attack_should_preempt(_, state)
    return not state.enemy
end

local BOSS_ATTACK_TASK = {
    run = boss_attack_task_run,
    should_preempt = boss_attack_should_preempt,
}

local function boss_patrol_task_run(task, state, ctx)
    local status = unstick(state.level.time, ctx, state.mind, state.enemy,
        state.enemy_visible, state.team, state.client)
    if status ~= STATUS_FAILURE then
        return sustain_spin(state, ctx, status)
    end

    note_progress(task, state)
    if task.mode then
        status = run_stuck_mode(task, state, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    local now = state.level.time
    task.last_known_ammo = state.client.ammo or 0
    if not task.phase then
        task.phase = "spin"
        task.phase_started_at = now
    end

    local switched = maybe_switch_phase(task, now, ctx)
    if task.phase == "taunt" then
        return run_taunt_phase(task, state, ctx)
    end

    if switched then
        return STATUS_RUNNING
    end

    return run_patrol_spin(task, state, ctx)
end

local function boss_patrol_should_preempt(_, state)
    return state.enemy ~= nil
end

local BOSS_PATROL_TASK = {
    run = boss_patrol_task_run,
    should_preempt = boss_patrol_should_preempt,
}

local function select_task(state)
    if state.enemy then
        return BOSS_ATTACK_TASK
    end

    return BOSS_PATROL_TASK
end

return function(self, ctx)
    local client = self.client
    local team = self.team
    local number = self.number
    local level = sgame.level

    local status = boss.prepare("flamer", self, ctx)
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
        local preempt_status = TASKS.maybe_preempt(state, ctx, select_task)
        if preempt_status ~= STATUS_FAILURE then
            return preempt_status
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
