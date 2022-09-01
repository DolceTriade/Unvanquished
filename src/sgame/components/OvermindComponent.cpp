#include "OvermindComponent.h"
#include "../Entities.h"

const float OvermindComponent::ATTACK_RANGE  = 300.0f;
const float OvermindComponent::ATTACK_DAMAGE = 10.0f;
const float DAMAGE_INC_PER_MINER = 30.0f;
const float RANGE_INC_PER_MINER = 400.0f;

OvermindComponent::OvermindComponent(Entity& entity, AlienBuildableComponent& r_AlienBuildableComponent,
                                     MainBuildableComponent& r_MainBuildableComponent)
	: OvermindComponentBase(entity, r_AlienBuildableComponent, r_MainBuildableComponent)
	, storedTarget(nullptr)
{
	GetMainBuildableComponent().GetBuildableComponent().REGISTER_THINKER(
		Think, ThinkingComponent::SCHEDULER_AVERAGE, 1000
	);
}

void OvermindComponent::HandlePrepareNetCode() {
	entity.oldEnt->s.otherEntityNum = storedTarget ? storedTarget->s.number : ENTITYNUM_NONE;
}

void OvermindComponent::HandleFinishConstruction() {
	// TODO: Make an event and move to MainBuildableComponent.
	G_TeamCommand(TEAM_ALIENS, "cp \"The Overmind has awakened!\"");
}

void OvermindComponent::Think(int timeDelta) {
	if (!GetAlienBuildableComponent().GetBuildableComponent().Active()) return;

	// Find and a target to look at.
	Entity* target = FindTarget();

	// Save the target for network transmission.
	storedTarget = target ? target->oldEnt : nullptr;

	// If target is an enemy in reach, attack it.
	// TODO: Add LocationComponent and Utility::Distance.
	int miners = level.team[GetTeamComponent().Team()].numMiners;
	float range = (ATTACK_RANGE + (RANGE_INC_PER_MINER * miners));
	if (target && Entities::OnOpposingTeams(entity, *target) &&
	    G_Distance(entity.oldEnt, target->oldEnt) < range) {

		float damage = (ATTACK_DAMAGE + (miners * DAMAGE_INC_PER_MINER)) * ((float)timeDelta / 1000.0f);

		// Psychic damage penetrates armor and goes right to your soul.
		target->Damage(damage, entity.oldEnt, {}, {}, DAMAGE_NO_PROTECTION, MOD_OVERMIND);

		G_SetBuildableAnim(entity.oldEnt, BANIM_ATTACK1, false);
		GetBuildableComponent().ProtectAnimation(1000);
	}
}

Entity* OvermindComponent::FindTarget() {
	Entity* target = nullptr;

	ForEntities<ClientComponent>([&](Entity& candidate, ClientComponent&) {
		// Do not target spectators.
		if (candidate.Get<SpectatorComponent>()) return;

		// Do not target dead entities.
		if (Entities::IsDead(candidate)) return;

		// Respect the no-target flag.
		if ((candidate.oldEnt->flags & FL_NOTARGET)) return;

		// Check for line of sight.
		if (!G_LineOfSight(entity.oldEnt, candidate.oldEnt, MASK_SOLID, false)) return;

		// Check if better target.
		if (!target || CompareTargets(candidate, *target)) {
			target = &candidate;
		}
	});

	return target;
}

bool OvermindComponent::CompareTargets(Entity& a, Entity& b) const {
	int miners = level.team[GetTeamComponent().Team()].numMiners;
	float range = (ATTACK_RANGE + (RANGE_INC_PER_MINER * miners));
	team_t aTeam = G_Team(a.oldEnt);
	team_t bTeam = G_Team(b.oldEnt);

	// Prefer humans.
	if (aTeam == TEAM_HUMANS && bTeam != TEAM_HUMANS) return true;
	if (aTeam != TEAM_HUMANS && bTeam == TEAM_HUMANS) return false;

	float aDistance = G_Distance(entity.oldEnt, a.oldEnt);
	float bDistance = G_Distance(entity.oldEnt, b.oldEnt);

	// Rules for humans.
	if (aTeam == TEAM_HUMANS && bTeam == TEAM_HUMANS) {
		// Prefer the one in attack range.
		if (aDistance < range && bDistance > range) return true;
		if (aDistance > range && bDistance < range) return false;

		// The overmind senses weakness!
		if (Entities::HealthFraction(a) < Entities::HealthFraction(b)) return true;
		if (Entities::HealthFraction(a) > Entities::HealthFraction(b)) return false;
	}

	// Break tie by preferring smaller distance.
	return aDistance < bDistance;
}

// TODO: Add OvermindComponent::CanAttack that also checks for the vision cone.
