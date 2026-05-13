/*
===========================================================================

Unvanquished GPL Source Code
Copyright (C) 2014 Unvanquished Developers

This file is part of the Unvanquished GPL Source Code (Unvanquished Source Code).

Unvanquished is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Unvanquished is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Unvanquished. If not, see <http://www.gnu.org/licenses/>.

===========================================================================
*/

#include "common/Common.h"
#include "sg_local.h"
#include "CBSE.h"
#include "Entities.h"

#include <cmath>
#include <limits>

static bool MinerReadyForPayout( team_t team, Entity& entity, MiningComponent& miningComponent )
{
	return G_Team( entity.oldEnt ) == team
		&& Entities::IsAlive( entity )
		&& miningComponent.Active()
		&& level.matchTime - miningComponent.TimeBuilt() >= MINER_INTERVAL;
}

void G_UpdateMinerIncome()
{
	if ( MINER_INTERVAL <= 0 )
	{
		return;
	}

	for ( team_t team = TEAM_NONE; ( team = G_IterateTeams( team ) ); )
	{
		int earliestActiveMinerTime = std::numeric_limits<int>::max();
		bool hasActiveMiner = false;

		ForEntities<MiningComponent>([&]( Entity& entity, MiningComponent& miningComponent ) {
			if ( G_Team( entity.oldEnt ) != team || !Entities::IsAlive( entity ) || !miningComponent.Active() )
			{
				return;
			}

			hasActiveMiner = true;
			earliestActiveMinerTime = std::min( earliestActiveMinerTime, miningComponent.TimeBuilt() );
		});

		int& nextPayoutTime = level.team[ team ].nextMinerPayoutTime;
		if ( !hasActiveMiner )
		{
			nextPayoutTime = 0;
			continue;
		}

		if ( nextPayoutTime <= 0 )
		{
			nextPayoutTime = earliestActiveMinerTime + MINER_INTERVAL;
		}

		while ( level.matchTime >= nextPayoutTime )
		{
			float sumEfficiency = 0.0f;

			ForEntities<MiningComponent>([&]( Entity& entity, MiningComponent& miningComponent ) {
				if ( MinerReadyForPayout( team, entity, miningComponent ) )
				{
					sumEfficiency += miningComponent.Efficiency();
				}
			});

			if ( sumEfficiency > 0.0f )
			{
				short payout = static_cast<short>(
					std::lround( MINER_CREDITS_PER_INTERVAL * std::pow( sumEfficiency, MINER_MULTIPLIER ) ) );

				for ( int i = 0; i < level.maxclients; i++ )
				{
					gclient_t* client = &level.clients[ i ];
					if ( client->pers.connected != CON_CONNECTED || client->pers.team != team )
					{
						continue;
					}

					G_AddCreditToClient( client, payout, true );
				}
			}

			nextPayoutTime += MINER_INTERVAL;
		}
	}
}

void G_SetTeamBuildPoints( team_t team, int amount )
{
	if ( !G_IsPlayableTeam( team ) )
	{
		return;
	}

	level.team[ team ].totalBudget = amount;
	G_PublishOverloadState( team );
}

/**
 * @brief Get the potentially negative number of free build points for a team.
 */
int G_GetFreeBudget(team_t team)
{
	return level.team[ team ].totalBudget;
}

/**
 * @brief Get the number of marked build points for a team.
 */
int G_GetMarkedBudget(team_t team)
{
	int sum = 0;

	ForEntities<BuildableComponent>(
	[&](Entity& entity, BuildableComponent& buildableComponent) {
		if (G_Team(entity.oldEnt) == team && buildableComponent.MarkedForDeconstruction()) {
			sum += G_BuildableDeconValue(entity.oldEnt);
		}
	});

	return sum;
}

/**
 * @brief Get the potentially negative number of build points a team can spend, including those from
 *        marked buildables.
 */
int G_GetSpendableBudget(team_t team)
{
	return G_GetFreeBudget(team) + G_GetMarkedBudget(team);
}

int G_GetEffectiveBudget( team_t team, int replacementCount, gentity_t *const *replacementList )
{
	int budget = G_GetFreeBudget( team );

	for ( int i = 0; i < replacementCount; i++ )
	{
		if ( replacementList[ i ] )
		{
			budget += G_BuildableDeconValue( replacementList[ i ] );
		}
	}

	return budget;
}

void G_FreeBudget( team_t team, int immediateAmount, int queuedAmount )
{
	if ( G_IsPlayableTeam( team ) )
	{
		level.team[ team ].spentBudget -= ( immediateAmount + queuedAmount );
		level.team[ team ].totalBudget += ( immediateAmount + queuedAmount );
		level.team[ team ].queuedBudget = 0;

		if ( level.team[ team ].spentBudget < 0 ) {
			level.team[ team ].spentBudget = 0;
		}

		G_PublishOverloadState( team );
	}
}

void G_SpendBudget( team_t team, int amount )
{
	if ( G_IsPlayableTeam( team ) )
	{
		level.team[ team ].totalBudget -= amount;
		level.team[ team ].spentBudget += amount;
		G_PublishOverloadState( team );
	}
}

void G_RemoveBudget( team_t team, int amount )
{
	if ( !G_IsPlayableTeam( team ) )
	{
		return;
	}

	level.team[ team ].spentBudget -= amount;
	if ( level.team[ team ].spentBudget < 0 )
	{
		level.team[ team ].spentBudget = 0;
	}

	G_PublishOverloadState( team );
}

int G_BuildableDeconValue(gentity_t *ent)
{
	HealthComponent* healthComponent = ent->entity->Get<HealthComponent>();

	if (!healthComponent->Alive()) {
		return 0;
	}

	return (int)ceilf((float)BG_Buildable(ent->s.modelindex)->buildPoints
	                  * healthComponent->HealthFraction());
}

/**
 * @brief Calculates the current value of buildables (in build points) for both teams.
 */
void G_GetTotalBuildableValues(int *buildableValuesByTeam)
{
	for (team_t team = TEAM_NONE; (team = G_IterateTeams(team)); ) {
		buildableValuesByTeam[team] = 0;
	}

	ForEntities<BuildableComponent>([&](Entity& entity, BuildableComponent&) {
		buildableValuesByTeam[G_Team(entity.oldEnt)] += G_BuildableDeconValue(entity.oldEnt);
	});
}
