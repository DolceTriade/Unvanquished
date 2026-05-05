#include "common/Common.h"
#include "MiningComponent.h"
#include "../Entities.h"

MiningComponent::MiningComponent(Entity& entity, ThinkingComponent& r_ThinkingComponent)
	: MiningComponentBase(entity, r_ThinkingComponent)
	, active(false)
	, timeBuilt(level.matchTime) {

	// Already calculate the predicted efficiency.
	CalculateEfficiency();

	// Inform neighbouring miners so they can adjust their own predictions.
	InformNeighbors();
}

void MiningComponent::HandlePrepareNetCode() {
	// Mining efficiency.
	entity.oldEnt->s.weaponAnim = (int)std::round(Efficiency() * (float)0xff);

	// TODO: Transmit budget grant.
}

void MiningComponent::HandleFinishConstruction() {
	active = true;
	timeBuilt = level.matchTime;

	// Now that we are active, calculate the current efficiency.
	CalculateEfficiency();

	// Inform neighbouring miners so they can react immediately.
	InformNeighbors();
}

void MiningComponent::HandleDie(gentity_t* /*killer*/, meansOfDeath_t /*meansOfDeath*/) {
	active = false;

	// Efficiency will be zero from now on.
	currentEfficiency   = 0.0f;
	predictedEfficiency = 0.0f;

	// Inform neighbouring miners so they can react immediately.
	InformNeighbors();
}

float MiningComponent::InterferenceMod(float distance) {
	if (RGS_RANGE <= 0.0f) return 1.0f;

	float t = Math::Clamp(distance / (2.0f * RGS_RANGE), 0.0f, 1.0f);
	return 0.1f + 0.9f * t * t;
}

MiningComponent::Efficiencies MiningComponent::FindEfficiencies(
	team_t team, const glm::vec3& location, MiningComponent* skip)
{
	MiningComponent::Efficiencies efficiencies{ 1.0f, 1.0f };

	ForEntities<MiningComponent>([&](Entity& other, MiningComponent& miningComponent) {
		if (&miningComponent == skip) return;

		// Do not consider dead neighbours, even when predicting, as they can never become active.
		if (!Entities::IsAlive(other)) return;

		float interferenceMod = InterferenceMod(glm::distance(location, VEC2GLM(other.oldEnt->s.origin)));

		// Enemy miners under construction are a secret
		if (!(G_Team(other.oldEnt) != team && !miningComponent.active)) {
			efficiencies.predicted *= interferenceMod;
		}

		if (miningComponent.active) {
			efficiencies.actual *= interferenceMod;
		}
	});
	return efficiencies;
}

void MiningComponent::CalculateEfficiency() {
	Efficiencies efficiencies = FindEfficiencies(
		G_Team(entity.oldEnt), VEC2GLM(entity.oldEnt->s.origin), this);
	currentEfficiency = active ? efficiencies.actual : 0.0f;
	predictedEfficiency = efficiencies.predicted;
}

void MiningComponent::InformNeighbors() {
	ForEntities<MiningComponent>([&] (Entity& other, MiningComponent& miningComponent) {
		if (&other == &entity) return;
		if (G_Distance(entity.oldEnt, other.oldEnt) > RGS_RANGE * 2.0f) return;

		miningComponent.CalculateEfficiency();
	});
}

float MiningComponent::Efficiency(bool predict) {
	return predict ? predictedEfficiency : currentEfficiency;
}

bool MiningComponent::Active() const {
	return active;
}

int MiningComponent::TimeBuilt() const {
	return timeBuilt;
}
