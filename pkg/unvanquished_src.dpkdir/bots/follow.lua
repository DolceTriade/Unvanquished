STATUS_FAILURE = 0
STATUS_SUCCESS = 1
STATUS_RUNNING = 2

local PMF_QUEUED = 1 << 12

local common = require("bots/common.lua")
local weapons = Unv.weapons
local is_alien = common.is_alien
local is_human = common.is_human
local should_spawn = common.should_spawn
local target_entity = common.target_entity
local use_medkit_if_low = common.use_medkit_if_low
local try_evolve_targets = common.try_evolve_targets
local unstick = common.unstick

local ALIEN_EVOLVE_TARGETS = common.ALIEN_EVOLVE_TARGETS

local function maybe_use_medkit(team, client, ctx)
    return use_medkit_if_low(team, client, ctx, 50)
end

local function maybe_reload(team, client, weapon_attr, enemy_visible, ctx)
    if not is_human(team) or enemy_visible or not weapon_attr or weapon_attr.ammo <= 0 then
        return STATUS_FAILURE
    end

    if (client.ammo or 0) / weapon_attr.ammo >= 0.4 then
        return STATUS_FAILURE
    end

    return ctx:reload()
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

local function maybe_fight(team, client, enemy_visible, ctx)
    if not enemy_visible then
        return STATUS_FAILURE
    end

    if is_human(team) and client.weapon == "ckit" then
        return STATUS_FAILURE
    end

    return ctx:fight()
end

return function(self, ctx)
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
    local enemy_visible = enemy and ctx:isVisibleEntity(enemy) or false
    local weapon_attr = weapons[client.weapon]

    local status = unstick(sgame.level.time, ctx, mind, enemy, enemy_visible, self.team, client)
    if status ~= STATUS_FAILURE then
        return status
    end

    if is_human(self.team) then
        status = maybe_use_medkit(self.team, client, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_fight(self.team, client, enemy_visible, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_reload(self.team, client, weapon_attr, enemy_visible, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_equip(self.team, mind, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = ctx:follow(250)
        if status ~= STATUS_FAILURE then
            return status
        end

        return ctx:roam()
    end

    if is_alien(self.team) then
        status = maybe_evolve(self, self.team, client, enemy_visible, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_fight(self.team, client, enemy_visible, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = ctx:follow(250)
        if status ~= STATUS_FAILURE then
            return status
        end
    end

    return ctx:roam()
end
