-- Team behavior used while a boss is active. Prepare builders, then run the
-- regular attack behavior without changing that behavior's implementation.

local constants = require("bots/consts.lua")
local STATUS_FAILURE = constants.STATUS_FAILURE
local STATUS_SUCCESS = constants.STATUS_SUCCESS
local STATUS_RUNNING = constants.STATUS_RUNNING

local common = require("bots/common.lua")
local behavior_state = require("bots/state.lua")
local boss = require("bots/boss.lua")
local attack = require("bots/attack.lua")
local STATE = behavior_state.new_behavior("boss_rush", { equip_retry_at = {} }, { "equip_retry_at" })

local function same_boss(ref, entity)
    return ref
        and ref.number == entity.number
        and ref.generation == entity.generation
end

local function prepare_builder(self, ctx, boss_entity)
    local client = self.client
    local number = self.number

    if common.is_alien(self.team)
        and (client.class == "builder" or client.class == "builderupg") then
        return common.try_evolve_targets(
            self,
            ctx,
            client,
            sgame.level,
            common.ALIEN_EVOLVE_TARGETS
        )
    end

    if common.is_human(self.team) and client.weapon == "ckit" then
        if same_boss(STATE[number], boss_entity) then
            return STATUS_FAILURE
        end

        local status = common.equip_and_buy_overload(
            ctx, STATE.equip_retry_at, number, sgame.level.time)
        if status == STATUS_FAILURE or status == STATUS_SUCCESS then
            STATE[number] = {
                number = boss_entity.number,
                generation = boss_entity.generation,
            }
        end
        return status
    end

    return STATUS_FAILURE
end

return function(self, ctx)
    if not self.client or not self.bot then
        return ctx:roam()
    end

    local boss_entity = boss.enemy(self.team)
    if not boss_entity then
        return attack(self, ctx)
    end

    local status = prepare_builder(self, ctx, boss_entity)
    if status ~= STATUS_FAILURE then
        return status
    end

    return attack(self, ctx)
end
