-- Spawn random buildables for a team on valid world surfaces.
-- Usage from the server Lua console:
--   lua require("scripts/random_buildables.lua").spawn("aliens")
--   lua require("scripts/random_buildables.lua").spawn("humans", 40, 8)
-- Parameters: team, successful build count (default 100), candidate attempts per batch (default 16).
-- Searches connected, grounded space starting at live players, or existing map/layout
-- spawn structures on an empty server. Stops if no safe seed exists or attempts run out.
-- Requires the sgame.world collision queries and surface/content constants.

local M = {}

local DEFAULT_BUILD_COUNT = 100
local DEFAULT_BATCH_SIZE = 16
local MAX_CANDIDATE_TRIES_PER_BUILDABLE = 128
local FLOOR_NORMAL_Z = 0.5
local WALL_NORMAL_Z = 0.3
-- Search through connected, floor-supported space with a player-sized hull.
-- The engine separately checks the actual buildable and owning builder hulls.
local BUILDER_MINS = { -18, -18, -24 }
local BUILDER_MAXS = { 18, 18, 32 }
local STEP_HEIGHT = 18
local MAX_DROP = 128
local SEARCH_STEP = 128
local SURFACE_RANGE = 512
local MAX_SEARCH_POINTS = 2048

local BUILDABLES = {
    aliens = {
        "eggpod",
        "overmind",
        "barricade",
        "acid_tube",
        "trapper",
        "booster",
        "hive",
        "spiker",
    },
    humans = {
        "telenode",
        "mgturret",
        "rocketpod",
        "arm",
        "medistat",
        "reactor",
    },
}

local function clear_trace(trace)
    return trace and not trace.startsolid and not trace.allsolid and trace.fraction == 1
end

local function valid_surface(world, trace)
    return trace and not trace.startsolid and not trace.allsolid
        and trace.fraction < 1 and trace.entityNum == world.ENTITYNUM_WORLD
        and trace.normal
        and (trace.surfaceFlags & (world.SURF_SKY | world.SURF_NODRAW)) == 0
        and (trace.contents & world.CONTENTS_SOLID) ~= 0
        and (trace.contents & world.MASK_HAZARD) == 0
end

local function search_trace(world, start, finish)
    -- Ignore players/buildables while exploring; retain world and mover collision,
    -- player clips, and hazard volumes. Spawn validation includes occupied hulls.
    return world.trace(start, BUILDER_MINS, BUILDER_MAXS, finish,
        world.ENTITYNUM_NONE, world.MASK_DEADSOLID | world.MASK_HAZARD, 0)
end

local function supported_point(world, start)
    local finish = { start[1], start[2], start[3] - MAX_DROP }
    local floor = search_trace(world, start, finish)
    if not valid_surface(world, floor) or floor.normal[3] < 0.7 then
        return nil
    end
    -- Keep the search hull just off the supporting surface, avoiding boundary
    -- roundoff in subsequent stationary and travel traces.
    local point = { floor.endpos[1], floor.endpos[2], floor.endpos[3] + 1 }
    if not clear_trace(search_trace(world, point, point))
        or (world.point_contents(point, world.ENTITYNUM_NONE) & world.MASK_HAZARD) ~= 0 then
        return nil
    end
    return point
end

local function add_point(search, point)
    if not point or #search.points >= MAX_SEARCH_POINTS then
        return
    end
    local key = string.format("%d:%d:%d", math.floor(point[1] / 64),
        math.floor(point[2] / 64), math.floor(point[3] / 32))
    if not search.seen[key] then
        search.seen[key] = true
        search.points[#search.points + 1] = point
    end
end

local function seed_search(world)
    local search = { points = {}, seen = {} }
    -- Prefer live players to avoid using previously misplaced buildables as seeds.
    for _, entity in pairs(sgame.entity) do
        if entity.client and entity.client.health > 0
            and (entity.team == "aliens" or entity.team == "humans") then
            add_point(search, supported_point(world, entity.origin))
        end
    end
    if #search.points > 0 then
        return search
    end

    -- Empty servers start near existing map/layout spawn structures. Keep the
    -- search hull on the exposed side of each structure, then require ground.
    for _, entity in pairs(sgame.entity) do
        local buildable = entity.buildable
        if buildable and buildable.health > 0
            and (buildable.name == "eggpod" or buildable.name == "telenode") then
            local origin, normal = entity.origin, entity.origin2
            local mins, maxs = entity.mins, entity.maxs
            local offset = 2
            for axis = 1, 3 do
                offset = offset + math.max(mins[axis] * normal[axis], maxs[axis] * normal[axis])
                    - math.min(BUILDER_MINS[axis] * normal[axis], BUILDER_MAXS[axis] * normal[axis])
            end
            local start = { origin[1] + offset * normal[1], origin[2] + offset * normal[2],
                origin[3] + offset * normal[3] }
            add_point(search, supported_point(world, start))
        end
    end
    return search
end

local function expand_search(world, search, start)
    local raised = { start[1], start[2], start[3] + STEP_HEIGHT }
    if not clear_trace(search_trace(world, start, raised)) then
        return
    end
    local angle = math.random() * 2 * math.pi
    local finish = { raised[1] + SEARCH_STEP * math.cos(angle),
        raised[2] + SEARCH_STEP * math.sin(angle), raised[3] }
    local travel = search_trace(world, raised, finish)
    if not travel or travel.startsolid or travel.allsolid then
        return
    end
    add_point(search, supported_point(world, travel.endpos))
end

local function random_world_trace(world, search)
    local start = search.points[math.random(#search.points)]
    expand_search(world, search, start)
    start = search.points[math.random(#search.points)]
    local finish = { start[1], start[2], start[3] }
    local axis = math.random(1, 3)
    finish[axis] = finish[axis] + (math.random(0, 1) == 1 and SURFACE_RANGE or -SURFACE_RANGE)
    local trace = world.trace(start, BUILDER_MINS, BUILDER_MAXS, finish,
        world.ENTITYNUM_NONE, world.MASK_PLAYERSOLID | world.MASK_HAZARD, 0)
    return valid_surface(world, trace) and trace or nil
end

local function allowed_surface(team, buildable, normal)
    local normal_z = normal[3]

    if team == "humans" then
        return normal_z > FLOOR_NORMAL_Z
    end

    -- Trappers must attach to a wall or ceiling, never an upward-facing floor.
    if buildable == "trapper" then
        return normal_z < -FLOOR_NORMAL_Z or math.abs(normal_z) <= WALL_NORMAL_Z
    end

    -- Other alien buildables may use floors, walls, or ceilings.
    return true
end

local function try_candidate(team, choices, world, search)
    local buildable = choices[math.random(#choices)]
    local trace = random_world_trace(world, search)
    if not trace or not allowed_surface(team, buildable, trace.normal) then
        return nil
    end

    -- Supplying the hit normal keeps placement on the sampled surface even near corners.
    return sgame.TrySpawnBuildableAt(buildable, trace.endpos, trace.normal)
end

function M.spawn(team, number, batch_size)
    local choices = BUILDABLES[team]
    if not choices then
        return nil, "team must be 'aliens' or 'humans'"
    end

    number = number or DEFAULT_BUILD_COUNT
    batch_size = batch_size or DEFAULT_BATCH_SIZE
    if type(number) ~= "number" or number < 1 or number ~= math.floor(number)
        or number ~= number or number == math.huge then
        return nil, "number must be a positive integer"
    end
    if type(batch_size) ~= "number" or batch_size < 1 or batch_size ~= math.floor(batch_size)
        or batch_size ~= batch_size or batch_size == math.huge then
        return nil, "batch_size must be a positive integer"
    end

    return sgame.workflow.run(function()
        local world = sgame.world
        local search = seed_search(world)
        if #search.points == 0 then
            print("Random buildable spawn stopped: no clear, grounded player or spawn seed found.")
            return
        end
        local spawned = 0
        local attempts = 0
        local max_attempts = number * MAX_CANDIDATE_TRIES_PER_BUILDABLE

        while spawned < number and attempts < max_attempts do
            local batch_attempts = 0
            while batch_attempts < batch_size and attempts < max_attempts and spawned < number do
                attempts = attempts + 1
                batch_attempts = batch_attempts + 1
                if try_candidate(team, choices, world, search) then
                    spawned = spawned + 1
                end
            end

            if spawned < number and attempts < max_attempts then
                sgame.workflow.wait_ms(1)
            end
        end

        print(string.format(
            "Random buildable spawn for %s finished: %d of %d placed in %d attempts.",
            team,
            spawned,
            number,
            attempts
        ))
    end)
end

return M
