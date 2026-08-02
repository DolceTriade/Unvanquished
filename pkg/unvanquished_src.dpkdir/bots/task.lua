local M = {}

local function copy_task(task)
    local instance = {}
    for key, value in pairs(task) do
        instance[key] = value
    end
    return instance
end

function M.new_runtime()
    local tasks = {}

    local runtime = {}

    function runtime.current(number)
        return tasks[number]
    end

    function runtime.start(number, task)
        local instance = copy_task(task)
        tasks[number] = instance
        return instance
    end

    function runtime.clear(number)
        tasks[number] = nil
    end

    function runtime.run(task, state, ctx)
        local status = task.run(task, state, ctx)
        if status ~= STATUS_RUNNING then
            runtime.clear(state.number)
        end

        return status
    end

    function runtime.maybe_preempt(state, ctx, select_task)
        local task = runtime.current(state.number)
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

        runtime.clear(state.number)
        local next_task = runtime.start(state.number, replacement)
        local status = runtime.run(next_task, state, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        previous_task.preempt_retry_at = state.level.time + (previous_task.preempt_cooldown_ms or 1000)
        tasks[state.number] = previous_task
        return STATUS_FAILURE
    end

    return runtime
end

return M
