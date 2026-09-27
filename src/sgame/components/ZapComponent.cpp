#include "common/Common.h"
#include "ZapComponent.h"

namespace {

bool IsZapTargetAimedAt( gentity_t* target, const glm::vec3& muzzle,
	const glm::vec3& forward )
{
	if ( !target )
	{
		return false;
	}

	// A wide trace against the target's bounding box is equivalent to a ray
	// against the box expanded by the trace width and height.
	glm::vec3 mins = VEC2GLM( target->r.absmin ) -
		glm::vec3( LEVEL2_AREAZAP_WIDTH, LEVEL2_AREAZAP_WIDTH,
			LEVEL2_AREAZAP_WIDTH );
	glm::vec3 maxs = VEC2GLM( target->r.absmax ) +
		glm::vec3( LEVEL2_AREAZAP_WIDTH, LEVEL2_AREAZAP_WIDTH,
			LEVEL2_AREAZAP_WIDTH );
	float enter = 0.0f;
	float exit = LEVEL2_AREAZAP_RANGE;

	for ( int axis = 0; axis < 3; ++axis )
	{
		if ( std::abs( forward[ axis ] ) < 1.0e-6f )
		{
			if ( muzzle[ axis ] < mins[ axis ] || muzzle[ axis ] > maxs[ axis ] )
			{
				return false;
			}
			continue;
		}

		float near = ( mins[ axis ] - muzzle[ axis ] ) / forward[ axis ];
		float far = ( maxs[ axis ] - muzzle[ axis ] ) / forward[ axis ];
		if ( near > far ) std::swap( near, far );
		enter = std::max( enter, near );
		exit = std::min( exit, far );

		if ( enter > exit )
		{
			return false;
		}
	}

	return enter <= exit;
}

void FreeZapEffectEntity(zap_t& zap)
{
	if (!zap.effectChannel)
	{
		return;
	}

	if (zap.effectChannel->inuse)
	{
		G_FreeEntity(zap.effectChannel);
	}

	zap.effectChannel = nullptr;
}

void FindZapChainTargets(zap_t *zap)
{
	gentity_t *ent = zap->targets[0].get(); // the source
	int entityList[MAX_GENTITIES];
	vec3_t range;
	vec3_t mins, maxs;
	int i, num;
	gentity_t *enemy;
	trace_t tr;
	float distance;

	VectorSet(range, LEVEL2_AREAZAP_CHAIN_RANGE, LEVEL2_AREAZAP_CHAIN_RANGE, LEVEL2_AREAZAP_CHAIN_RANGE);

	VectorAdd(ent->s.origin, range, maxs);
	VectorSubtract(ent->s.origin, range, mins);

	// Always reset number of targets to 1 so we can rediscover new chains.
	zap->numTargets = 1;

	num = trap_EntitiesInBox(mins, maxs, entityList, MAX_GENTITIES);

	for (i = 0; i < num; i++)
	{
		enemy = &g_entities[entityList[i]];

		// don't chain to self; noclippers can be listed, don't chain to them either
		if (enemy == ent || (enemy->client && enemy->client->noclip))
		{
			continue;
		}

		distance = Distance(ent->s.origin, enemy->s.origin);

		if (((enemy->client &&
				enemy->client->pers.team == TEAM_HUMANS) ||
				(enemy->s.eType == entityType_t::ET_BUILDABLE &&
				BG_Buildable(enemy->s.modelindex)->team == TEAM_HUMANS)) &&
			enemy->entity->Get<HealthComponent>()->Alive() &&
			distance <= LEVEL2_AREAZAP_CHAIN_RANGE)
		{
			// world-LOS check: trace against the world, ignoring other BODY entities
			trap_Trace(&tr, ent->s.origin, nullptr, nullptr,
						enemy->s.origin, ent->s.number, CONTENTS_SOLID, 0);

			if (tr.entityNum == ENTITYNUM_NONE)
			{
				zap->targets[zap->numTargets] = enemy;
				zap->distances[zap->numTargets] = distance;

				if (++zap->numTargets >= LEVEL2_AREAZAP_MAX_TARGETS)
				{
					return;
				}
			}
		}
	}
}  // namespace

void UpdateZapEffect(const glm::vec3& muzzle, zap_t *zap)
{
	int i;
	int entityNums[LEVEL2_AREAZAP_MAX_TARGETS + 1];

	entityNums[0] = zap->creator->s.number;

	ASSERT_LE(zap->numTargets, LEVEL2_AREAZAP_MAX_TARGETS);

	for (i = 0; i < zap->numTargets; i++)
	{
		entityNums[i + 1] = zap->targets[i]->s.number;
	}

	BG_PackEntityNumbers(&zap->effectChannel->s,
							entityNums, zap->numTargets + 1);

	G_SetOrigin(zap->effectChannel, muzzle);
	trap_LinkEntity(zap->effectChannel);
}

} // namespace

ZapComponent::ZapComponent(Entity &entity, ThinkingComponent &r_ThinkingComponent)
	: ZapComponentBase(entity, r_ThinkingComponent)
{
	REGISTER_THINKER(UpdateZap, ThinkingComponent::SCHEDULER_AVERAGE, 100);
	zap = zap_t{};
	zap.creator = entity.oldEnt;
}

void ZapComponent::HandleZapTarget(Entity &target)
{
	if (zap.used)
		return;
	zap.timeAlive = 0;
	zap.timeWrongTarget = 0;
	zap.used = true;
	glm::vec3 forward;
	gentity_t* self = this->entity.oldEnt;
	AngleVectors( VEC2GLM( self->client->ps.viewangles ), &forward, nullptr, nullptr);
	glm::vec3 muzzle = G_CalcMuzzlePoint( self, forward );
	zap.targets[0] = target.oldEnt;
	zap.numTargets = 1;
	if (target.Damage(static_cast<float>(LEVEL2_AREAZAP_DMG), zap.creator.get(), glm::make_vec3(target.oldEnt->s.origin),
					  glm::make_vec3(forward), DAMAGE_NO_LOCDAMAGE, MOD_LEVEL2_ZAP))
	{
		FindZapChainTargets(&zap);
		for (int i = 1; i < zap.numTargets; ++i)
		{
			float damage = LEVEL2_AREAZAP_DMG * (1 - powf((zap.distances[i] /
														   LEVEL2_AREAZAP_CHAIN_RANGE),
														  LEVEL2_AREAZAP_CHAIN_FALLOFF)) +
						   1;
			target.Damage(damage, zap.creator.get(), glm::make_vec3(target.oldEnt->s.origin),
						  glm::make_vec3(forward), DAMAGE_NO_LOCDAMAGE, MOD_LEVEL2_ZAP);
		}
	}
	zap.effectChannel = G_NewEntity(NO_CBSE);
	zap.effectChannel->s.eType = entityType_t::ET_LEV2_ZAP_CHAIN;
	BG_Free(zap.effectChannel->classname);
	zap.effectChannel->classname = BG_strdup("lev2zapchain");
	UpdateZapEffect(muzzle, &zap);
}

void ZapComponent::HandleClearZap(Entity &player)
{
	if (!zap.used)
		return;
	glm::vec3 muzzle, forward;
	muzzle = G_CalcMuzzlePoint(entity.oldEnt, forward);
	// the disappearance of the creator or the first target destroys the whole zap effect
	if (zap.creator.get() == player.oldEnt || zap.targets[0].get() == player.oldEnt)
	{
		FreeZapEffectEntity(zap);
		zap.used = false;
	}

	// the disappearance of chained players destroy the appropriate beams
	for (int j = 1; j < zap.numTargets; j++)
	{
		if (zap.targets[j].get() == player.oldEnt)
		{
			zap.targets[j--] = zap.targets[--zap.numTargets];
		}
	}
}

void ZapComponent::UpdateZap(int timeDelta)
{
	if (!zap.used) return;
	if (!zap.targets[0])
	{
		HandleClearZap(*zap.creator->entity);
		return;
	}
	glm::vec3 forward;
	gentity_t* self = this->entity.oldEnt;
	AngleVectors( VEC2GLM( self->client->ps.viewangles ), &forward, nullptr, nullptr);
	glm::vec3 muzzle = G_CalcMuzzlePoint( self, forward );

	HealthComponent *targetHealth = zap.targets[0]->entity->Get<HealthComponent>();
	const glm::vec3 traceExtents{ LEVEL2_AREAZAP_WIDTH, LEVEL2_AREAZAP_WIDTH,
		LEVEL2_AREAZAP_WIDTH };
	G_UnlaggedOn( self, GLM4READ( muzzle ),
		LEVEL2_AREAZAP_RANGE + glm::length( traceExtents ) );

	bool aimedAtTarget = IsZapTargetAimedAt(zap.targets[0].get(), muzzle, forward);

	// Keep the existing CONTENTS_SOLID behavior: BODY entities are ignored,
	// while world geometry and solid movers remain blockers.
	trace_t lineOfSightTrace;
	trap_Trace( &lineOfSightTrace, muzzle, glm::vec3(), glm::vec3(),
		VEC2GLM( zap.targets[0]->r.currentOrigin ), self->s.number, CONTENTS_SOLID, 0 );

	G_UnlaggedOff();

	if (!aimedAtTarget || lineOfSightTrace.entityNum != ENTITYNUM_NONE ||
		!targetHealth || !targetHealth->Alive())
	{

		// If you're not aiming at them at this moment, you have some time to aim at them again, however
		// if you are out of range, then you stop zapping.
		if (zap.timeWrongTarget < LEVEL2_AREAZAP_TIME && G_Distance(zap.targets[0].get(), entity.oldEnt) < LEVEL2_AREAZAP_RANGE)
		{
			// Fall though and execute the rest of the attack code.
			zap.timeWrongTarget += timeDelta;
		}
		else
		{
			FreeZapEffectEntity(zap);
			zap.used = false;
			for (int j = 1; j < zap.numTargets; j++)
			{
				if (!zap.targets[j]->inuse)
				{
					zap.targets[j--] = zap.targets[--zap.numTargets];
				}
			}
			return;
		}
	}
	else
	{
		zap.timeWrongTarget = 0;
	}

	float baseDamage = LEVEL2_AREAZAP_DMG * std::min(static_cast<float>(zap.timeAlive) / 1000.0f, 3.0f);
	if (zap.targets[0]->entity->Damage(baseDamage, zap.creator.get(), glm::make_vec3(zap.targets[0]->s.origin),
									   glm::make_vec3(forward), DAMAGE_NO_LOCDAMAGE, MOD_LEVEL2_ZAP))
	{
		FindZapChainTargets(&zap);
		for (int i = 1; i < zap.numTargets; ++i)
		{
			gentity_t *target = zap.targets[i].get();
			float damage = baseDamage * (1 - powf((zap.distances[i] /
												   LEVEL2_AREAZAP_CHAIN_RANGE),
												  LEVEL2_AREAZAP_CHAIN_FALLOFF)) +
						   1;
			target->entity->Damage(damage, zap.creator.get(), glm::make_vec3(target->s.origin),
								   glm::make_vec3(forward), DAMAGE_NO_LOCDAMAGE, MOD_LEVEL2_ZAP);
		}
	}
	zap.timeAlive += timeDelta;
	UpdateZapEffect(muzzle, &zap);
}

void ZapComponent::HandleDie(gentity_t*, meansOfDeath_t) {
	HandleClearZap(*zap.creator->entity);
}
