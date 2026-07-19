STATUS_FAILURE = 0
STATUS_SUCCESS = 1
STATUS_RUNNING = 2

local PMF_QUEUED = 1 << 12

local cache = require("bots/cache.lua")
local common = require("bots/common.lua")
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

    return ctx:equip()
end

local function maybe_fight_human(team, client, enemy_visible, mind, ctx)
    if not is_human(team) or client.weapon == "ckit" or not enemy_visible then
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
    local enemy_visible = enemy and ctx:isVisibleEntity(enemy) or false
    local weapon_attr = weapons[client.weapon]
    local status = unstick(sgame.level.time, ctx, mind, enemy, enemy_visible, self.team, client)
    if status ~= STATUS_FAILURE then
        return status
    end

    if is_alien(self.team) then
        status = maybe_extinguish_fire(self.team, client, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_evolve(self, self.team, client, enemy_visible, ctx)
        if status ~= STATUS_FAILURE then
            return status
        end

        status = maybe_fight_or_heal_alien(client, enemy_visible, ctx, mind)
        if status ~= STATUS_FAILURE then
            return status
        end

        return roam_friendly_base(self.team, ctx)
    end

    status = maybe_use_medkit(self.team, client, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = maybe_retire_repair_kit(self.team, client, mind, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = maybe_fight_human(self.team, client, enemy_visible, mind, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = maybe_repair(self.team, self.number, client, ctx, mind)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = maybe_reload(self.team, client, weapon_attr, enemy_visible, sgame.level, mind, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    status = maybe_equip(self.team, enemy_visible, sgame.level, mind, ctx)
    if status ~= STATUS_FAILURE then
        return status
    end

    return roam_friendly_base(self.team, ctx)
end
