/*
===========================================================================

Unvanquished GPL Source Code
Copyright (C) 2026 Unvanquished Developers

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

namespace {

enum class overloadPurchaseKind_t
{
	MULTIPLIER,
	BP_BUNDLE,
	UNLOCK_TIER,
};

struct overloadPurchaseDef_t
{
	const char*            name;
	const char*            description;
	overloadPurchaseKind_t kind;
	int                    bankCost;
	int                    progressRequirement;
	int                    amount;
	bool                   repeatable;
};

constexpr int OVERLOAD_LEGACY_STAGE_VALUE = 100;

const overloadPurchaseDef_t overloadPurchases[] =
{
	{ "bp_25",       "Add 25 team BP",                overloadPurchaseKind_t::BP_BUNDLE,   350,   0,  25, true  },
	{ "multiplier",  "Increase future combat income", overloadPurchaseKind_t::MULTIPLIER, 500,   0,   1, true  },
	{ "tier_1",      "Raise the team's unlock tier",  overloadPurchaseKind_t::UNLOCK_TIER, 900, 100,   1, false },
};

static_assert( ARRAY_LEN( overloadPurchases ) <= MAX_OVERLOAD_PURCHASES, "purchase catalog exceeds storage" );

static int InitialBudgetForTeam( team_t team )
{
	if ( team == TEAM_ALIENS && g_BPInitialBudgetAliens.Get() >= 0 )
	{
		return g_BPInitialBudgetAliens.Get();
	}

	if ( team == TEAM_HUMANS && g_BPInitialBudgetHumans.Get() >= 0 )
	{
		return g_BPInitialBudgetHumans.Get();
	}

	return g_buildPointInitialBudget.Get();
}

static TeamEconomyState& TeamEconomy( team_t team )
{
	return level.team[ team ].economy;
}

static void NotifyLegacyStageSensors( team_t team, int oldProgress, int newProgress )
{
	for ( int stage = 1; stage < 3; stage++ )
	{
		int threshold = stage * OVERLOAD_LEGACY_STAGE_VALUE;
		bool wasPast = oldProgress >= threshold;
		bool isPast  = newProgress >= threshold;

		if ( wasPast == isPast )
		{
			continue;
		}

		if ( isPast )
		{
			G_notify_sensor_stage( team, stage - 1, stage );
		}
		else
		{
			G_notify_sensor_stage( team, stage, stage - 1 );
		}
	}
}

static void SyncOverloadProgress( team_t team )
{
	level.team[ team ].momentum = G_OverloadProgressValue( team );
	G_UpdateUnlockables();
}

static const overloadPurchaseDef_t* FindPurchase( Str::StringRef name, int* purchaseIndex = nullptr )
{
	for ( size_t i = 0; i < ARRAY_LEN( overloadPurchases ); i++ )
	{
		if ( name == overloadPurchases[ i ].name )
		{
			if ( purchaseIndex )
			{
				*purchaseIndex = i;
			}
			return &overloadPurchases[ i ];
		}
	}

	return nullptr;
}

static void ApplyPurchase( team_t team, int purchaseIndex, const overloadPurchaseDef_t& purchase )
{
	TeamEconomyState& economy = TeamEconomy( team );

	economy.repeatCounts[ purchaseIndex ]++;
	economy.ownedPurchases[ purchaseIndex ] = true;

	switch ( purchase.kind )
	{
		case overloadPurchaseKind_t::MULTIPLIER:
			economy.multiplierLevel += purchase.amount;
			economy.rewardMultiplier = 1.0f + 0.25f * economy.multiplierLevel;
			break;

		case overloadPurchaseKind_t::BP_BUNDLE:
			economy.bpPurchased += purchase.amount;
			level.team[ team ].totalBudget += purchase.amount;
			break;

		case overloadPurchaseKind_t::UNLOCK_TIER:
			economy.tier += purchase.amount;
			break;
	}

	SyncOverloadProgress( team );
}

} // namespace

void G_UpdateBuildPointBudgets()
{
	for ( team_t team = TEAM_NONE; ( team = G_IterateTeams( team ) ); )
	{
		if ( level.team[ team ].totalBudget == 0 && level.team[ team ].spentBudget == 0 )
		{
			level.team[ team ].totalBudget = InitialBudgetForTeam( team );
		}
	}
}

void G_InitOverloadEconomy()
{
	for ( team_t team = TEAM_NONE; ( team = G_IterateTeams( team ) ); )
	{
		level.team[ team ].totalBudget = InitialBudgetForTeam( team );
		level.team[ team ].spentBudget = 0;
		level.team[ team ].queuedBudget = 0;

		TeamEconomyState& economy = TeamEconomy( team );
		economy.bankBalance = 0;
		economy.progress = 0;
		economy.milestone = 0;
		economy.tier = 0;
		economy.bpPurchased = 0;
		economy.multiplierLevel = 0;
		economy.rewardMultiplier = 1.0f;
		memset( economy.repeatCounts, 0, sizeof( economy.repeatCounts ) );
		memset( economy.ownedPurchases, 0, sizeof( economy.ownedPurchases ) );

		SyncOverloadProgress( team );
	}
}

float G_OverloadRewardMultiplier( team_t team )
{
	return G_IsPlayableTeam( team ) ? TeamEconomy( team ).rewardMultiplier : 1.0f;
}

int G_OverloadProgressValue( team_t team )
{
	if ( !G_IsPlayableTeam( team ) )
	{
		return 0;
	}

	const TeamEconomyState& economy = TeamEconomy( team );
	return economy.progress + economy.tier * OVERLOAD_LEGACY_STAGE_VALUE;
}

bool G_OverloadDonate( gentity_t *ent, int amount )
{
	if ( !ent || !ent->client || amount <= 0 )
	{
		return false;
	}

	team_t team = (team_t) ent->client->pers.team;
	if ( !G_IsPlayableTeam( team ) || ent->client->pers.credit < amount )
	{
		return false;
	}

	G_AddCreditToClient( ent->client, -amount, false );
	TeamEconomy( team ).bankBalance += amount;
	return true;
}

bool G_OverloadPurchase( gentity_t *ent, Str::StringRef purchaseName )
{
	if ( !ent || !ent->client )
	{
		return false;
	}

	team_t team = (team_t) ent->client->pers.team;
	if ( !G_IsPlayableTeam( team ) )
	{
		return false;
	}

	int purchaseIndex = -1;
	const overloadPurchaseDef_t* purchase = FindPurchase( purchaseName, &purchaseIndex );
	if ( !purchase )
	{
		return false;
	}

	TeamEconomyState& economy = TeamEconomy( team );
	if ( !purchase->repeatable && economy.ownedPurchases[ purchaseIndex ] )
	{
		return false;
	}

	if ( G_OverloadProgressValue( team ) < purchase->progressRequirement ||
	     economy.bankBalance < purchase->bankCost )
	{
		return false;
	}

	economy.bankBalance -= purchase->bankCost;
	int oldProgress = G_OverloadProgressValue( team );
	ApplyPurchase( team, purchaseIndex, *purchase );
	int newProgress = G_OverloadProgressValue( team );
	NotifyLegacyStageSensors( team, oldProgress, newProgress );
	return true;
}
