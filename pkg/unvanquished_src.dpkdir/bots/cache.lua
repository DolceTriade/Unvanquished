local wf = sgame.workflow

local M = {
    state = {
        updated_at = -1,
        cvars = {},
        teams = {
            aliens = {},
            humans = {},
        },
    },
}

local REFRESH_INTERVAL = 3000

local OBSERVED_CVARS = {
    "g_bot_buildAliens",
    "g_bot_buildHumans",
    "g_bot_buildNumEggs",
    "g_bot_buildNumTelenodes",
    "g_bot_builder",
    "g_bot_builderupg",
    "g_bot_ckit",
    "g_bot_extinguishFire",
    "g_bot_chasetime",
    "g_bot_aliensenseRange",
    "g_maxMiners",
}

local function entity_ref(ent)
    if not ent then
        return nil
    end

    return {
        number = ent.number,
        generation = ent.generation,
    }
end

local function resolve_entity(ref)
    if not ref then
        return nil
    end

    local ent = sgame.entity[ref.number]
    if not ent or ent.generation ~= ref.generation then
        return nil
    end

    return ent
end

local function new_team_snapshot(name)
    return {
        name = name,
        entities = {},
        weapons = {},
        classes = {},
        buildables = {},
    }
end

local function normalize_team_name(team)
    if team == "alien" or team == "aliens" then
        return "aliens"
    end

    if team == "human" or team == "humans" then
        return "humans"
    end

    return nil
end

local function team_level(team_name)
    if team_name == "aliens" then
        return sgame.level.aliens
    end

    if team_name == "humans" then
        return sgame.level.humans
    end

    return nil
end

local function increment(map, key)
    if not key then
        return
    end

    map[key] = (map[key] or 0) + 1
end

local function buildable_attr(name)
    return Unv.buildables[name]
end

local function buildable_cost(name)
    local buildable = name and buildable_attr(name) or nil
    return buildable and buildable.build_points or 32767
end

local function buildable_unlocked(team_name, name)
    local buildable = name and buildable_attr(name) or nil
    local team = team_level(team_name)
    if not buildable or not team then
        return false
    end

    return team.overload_progress >= buildable.unlock_threshold
end

local function count_buildable(team_name, name)
    local team = M.state.teams[team_name]
    if not team then
        return 0
    end

    return team.buildables[name] or 0
end

local function count_weapon(team_name, weapon_name, exclude_entity_num)
    local team = M.state.teams[team_name]
    if not team then
        return 0
    end

    local count = 0
    for entity_num, info in pairs(team.entities) do
        if entity_num ~= exclude_entity_num and info.weapon == weapon_name then
            count = count + 1
        end
    end
    return count
end

local function snapshot_counts()
    local aliens = new_team_snapshot("aliens")
    local humans = new_team_snapshot("humans")

    for _, ent in pairs(sgame.entity) do
        local team_name = normalize_team_name(ent.team)
        if ent.client and team_name then
            local team = team_name == "aliens" and aliens or humans
            local entity_info = {
                ref = entity_ref(ent),
                weapon = ent.client.weapon,
                class = ent.client.class,
                is_bot = ent.bot ~= nil,
            }
            team.entities[ent.number] = entity_info
            increment(team.weapons, entity_info.weapon)
            increment(team.classes, entity_info.class)
        end

        if ent.buildable then
            local buildable_team = normalize_team_name(ent.buildable.team)
            if buildable_team then
                local team = buildable_team == "aliens" and aliens or humans
                increment(team.buildables, ent.buildable.name)
            end
        end
    end

    -- TODO: This only counts active buildables visible through sgame.entity.
    -- It does not include ghost buildables like the BT helper does.
    M.state.teams.aliens = aliens
    M.state.teams.humans = humans
end

local function refresh_cvars()
    local cvars = {}
    for _, name in ipairs(OBSERVED_CVARS) do
        cvars[name] = Cvar.get(name)
    end
    M.state.cvars = cvars
end

function M.refresh()
    if M.state.updated_at < 0
        or sgame.level.time - M.state.updated_at >= REFRESH_INTERVAL then
        snapshot_counts()
        refresh_cvars()
        M.state.updated_at = sgame.level.time
    end

    return M.state
end

function M.team(team)
    local team_name = normalize_team_name(team)
    if not team_name then
        return nil
    end

    return M.state.teams[team_name]
end

function M.has_teammate_weapon(team, weapon_name, exclude_entity_num)
    return count_weapon(normalize_team_name(team), weapon_name, exclude_entity_num) > 0
end

function M.free_budget(team)
    local team_name = normalize_team_name(team)
    local level = team_name and team_level(team_name) or nil
    if not level then
        return 0
    end

    local free_budget = level.total_budget - level.spent_budget
    if free_budget < 0 then
        free_budget = 0
    end

    return free_budget
end

function M.usable_build_points(team)
    local team_name = normalize_team_name(team)
    local build_points = M.free_budget(team_name)

    if team_name == "aliens" and not buildable_unlocked("aliens", "booster") then
        build_points = build_points - buildable_cost("booster")
    end

    if build_points < 0 then
        return 0
    end

    return build_points
end

function M.cvar(name)
    return M.state.cvars[name] or ""
end

function M.cvar_number(name)
    return tonumber(M.cvar(name)) or 0
end

function M.chosen_buildable(team)
    local team_name = normalize_team_name(team)
    if team_name == "humans" then
        if count_buildable("humans", "reactor") == 0 then
            return "reactor"
        end
        if M.cvar_number("g_maxMiners") ~= 0 and count_buildable("humans", "drill") == 0 then
            return "drill"
        end
        if count_buildable("humans", "telenode") < M.cvar_number("g_bot_buildNumTelenodes") then
            return "telenode"
        end
        if count_buildable("humans", "arm") == 0 then
            return "arm"
        end
        if count_buildable("humans", "medistat") == 0 then
            return "medistat"
        end
        return "mgturret"
    end

    if team_name == "aliens" then
        if count_buildable("aliens", "overmind") == 0 then
            return "overmind"
        end
        if M.cvar_number("g_maxMiners") ~= 0 and count_buildable("aliens", "leech") == 0 then
            return "leech"
        end
        if count_buildable("aliens", "eggpod") < math.floor(M.cvar_number("g_bot_buildNumEggs") / 2) then
            return "eggpod"
        end
        if buildable_unlocked("aliens", "booster") and count_buildable("aliens", "booster") == 0 then
            return "booster"
        end
        if count_buildable("aliens", "eggpod") < M.cvar_number("g_bot_buildNumEggs") then
            return "eggpod"
        end
        return "acid_tube"
    end

    return nil
end

function M.chosen_buildable_cost(team)
    return buildable_cost(M.chosen_buildable(team))
end

function M.entity_ref(ent)
    return entity_ref(ent)
end

function M.resolve_entity(ref)
    return resolve_entity(ref)
end

snapshot_counts()

wf.run(function()
    while true do
        snapshot_counts()
        wf.wait_ms(2000)
    end
end)

return M
