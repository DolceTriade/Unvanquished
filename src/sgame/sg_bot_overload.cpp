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
#include "sg_overload.h"
#include "sg_local.h"
#include "sg_bot_local.h"

#include <limits>

static constexpr int OVERLOAD_BOT_PURCHASE_COOLDOWN = 3000;
static constexpr int OVERLOAD_BOT_FAILURE_COOLDOWN = 1000;
static constexpr int OVERLOAD_BOT_SPEND_CHUNK = 200;
static constexpr int OVERLOAD_BOT_HUMAN_RESERVE = 200;
static constexpr int OVERLOAD_BOT_ALIEN_RESERVE = 200;
static constexpr int OVERLOAD_BOT_BP_PRESSURE_THRESHOLD = 20;
static constexpr int OVERLOAD_BOT_BP_AFTER_DESTRUCTION_WINDOW = 30000;

static int BotReserveCredits( team_t team )
{
	return team == TEAM_ALIENS ? OVERLOAD_BOT_ALIEN_RESERVE : OVERLOAD_BOT_HUMAN_RESERVE;
}

static bool OverloadEntryIsPartial( team_t team, int purchaseIndex )
{
	if ( !G_IsPlayableTeam( team ) )
	{
		return false;
	}

	if ( purchaseIndex < 0 || purchaseIndex >= static_cast<int>( overloadPurchases.size() ) )
	{
		return false;
	}

	const overloadPurchaseDef_t& entry = overloadPurchases[ purchaseIndex ];
	const TeamEconomyState& economy = TeamEconomy( team );

	return ( entry.team == TEAM_NONE || entry.team == team ) &&
	       economy.investedCredits[ purchaseIndex ] > 0 &&
	       EntryIsAvailable( team, entry ) &&
	       RemainingSpendCapacity( entry, purchaseIndex, team ) > 0;
}

static bool OverloadEntryCanBotStart( team_t team, int purchaseIndex )
{
	if ( !G_IsPlayableTeam( team ) )
	{
		return false;
	}

	if ( purchaseIndex < 0 || purchaseIndex >= static_cast<int>( overloadPurchases.size() ) )
	{
		return false;
	}

	const overloadPurchaseDef_t& entry = overloadPurchases[ purchaseIndex ];
	return ( entry.team == TEAM_NONE || entry.team == team ) &&
	       EntryIsAvailable( team, entry ) &&
	       RemainingSpendCapacity( entry, purchaseIndex, team ) > 0;
}

static bool OverloadEntryIsBPBundle( team_t team, int purchaseIndex )
{
	if ( !G_IsPlayableTeam( team ) )
	{
		return false;
	}

	if ( purchaseIndex < 0 || purchaseIndex >= static_cast<int>( overloadPurchases.size() ) )
	{
		return false;
	}

	const overloadPurchaseDef_t& entry = overloadPurchases[ purchaseIndex ];
	return ( entry.team == TEAM_NONE || entry.team == team ) && entry.kind == overloadPurchaseKind_t::BP_BUNDLE;
}

static bool OverloadEntryIsUnlock( team_t team, int purchaseIndex )
{
	if ( !G_IsPlayableTeam( team ) )
	{
		return false;
	}

	if ( purchaseIndex < 0 || purchaseIndex >= static_cast<int>( overloadPurchases.size() ) )
	{
		return false;
	}

	const overloadPurchaseDef_t& entry = overloadPurchases[ purchaseIndex ];
	return ( entry.team == TEAM_NONE || entry.team == team ) && entry.kind == overloadPurchaseKind_t::UNLOCK;
}

static bool OverloadEntryIsUpgrade( team_t team, int purchaseIndex )
{
	if ( !G_IsPlayableTeam( team ) )
	{
		return false;
	}

	if ( purchaseIndex < 0 || purchaseIndex >= static_cast<int>( overloadPurchases.size() ) )
	{
		return false;
	}

	const overloadPurchaseDef_t& entry = overloadPurchases[ purchaseIndex ];
	return ( entry.team == TEAM_NONE || entry.team == team ) && entry.kind == overloadPurchaseKind_t::UPGRADE;
}

static int OverloadEntryRemainingSpendCapacity( team_t team, int purchaseIndex )
{
	if ( !G_IsPlayableTeam( team ) )
	{
		return 0;
	}

	if ( purchaseIndex < 0 || purchaseIndex >= static_cast<int>( overloadPurchases.size() ) )
	{
		return 0;
	}

	const overloadPurchaseDef_t& entry = overloadPurchases[ purchaseIndex ];
	if ( entry.team != TEAM_NONE && entry.team != team )
	{
		return 0;
	}

	return RemainingSpendCapacity( entry, purchaseIndex, team );
}

static int FindBotPartialPurchase( team_t team )
{
	int bestPurchaseIndex = -1;
	int bestRemaining = std::numeric_limits<int>::max();

	for ( int i = 0; i < G_OverloadPurchaseCount(); ++i )
	{
		if ( OverloadEntryIsPartial( team, i ) )
		{
			const int remaining = OverloadEntryRemainingSpendCapacity( team, i );
			if ( remaining > 0 && remaining < bestRemaining )
			{
				bestPurchaseIndex = i;
				bestRemaining = remaining;
			}
		}
	}

	return bestPurchaseIndex;
}

static int FindBotBPPurchase( team_t team )
{
	for ( int i = 0; i < G_OverloadPurchaseCount(); ++i )
	{
		if ( OverloadEntryCanBotStart( team, i ) && OverloadEntryIsBPBundle( team, i ) )
		{
			return i;
		}
	}

	return -1;
}

static bool BotBuildablesRecentlyKilled( team_t team )
{
	for ( int i = 0; i < level.numBuildLogs; ++i )
	{
		const buildLog_t& log = level.buildLog[ ( level.buildId - i - 1 ) % MAX_BUILDLOG ];
		if ( level.time - log.time > OVERLOAD_BOT_BP_AFTER_DESTRUCTION_WINDOW )
		{
			return false;
		}

		if ( log.buildableTeam != team )
		{
			continue;
		}

		switch ( log.fate )
		{
			case BF_DESTROY:
			case BF_TEAMKILL:
				return true;

			case BF_AUTO:
			case BF_CONSTRUCT:
			case BF_DECONSTRUCT:
			case BF_REPLACE:
				break;
		}
	}

	return false;
}

static int FindBotFreshPurchase( team_t team )
{
	int bpPurchaseIndex = -1;
	std::vector<int> upgradePurchaseIndices;

	for ( int i = 0; i < G_OverloadPurchaseCount(); ++i )
	{
		if ( !OverloadEntryCanBotStart( team, i ) )
		{
			continue;
		}

		if ( OverloadEntryIsUnlock( team, i ) )
		{
			return i;
		}

		if ( bpPurchaseIndex < 0 && OverloadEntryIsBPBundle( team, i ) )
		{
			bpPurchaseIndex = i;
			continue;
		}

		if ( OverloadEntryIsUpgrade( team, i ) )
		{
			upgradePurchaseIndices.push_back( i );
		}
	}

	if ( bpPurchaseIndex >= 0 &&
	     G_GetFreeBudget( team ) < OVERLOAD_BOT_BP_PRESSURE_THRESHOLD &&
	     BotBuildablesRecentlyKilled( team ) )
	{
		return bpPurchaseIndex;
	}

	if ( upgradePurchaseIndices.empty() )
	{
		return -1;
	}

	const int randomIndex = static_cast<int>( BG_random() * upgradePurchaseIndices.size() );
	return upgradePurchaseIndices[ std::min( randomIndex, static_cast<int>( upgradePurchaseIndices.size() ) - 1 ) ];
}

void G_BotOverloadThink( gentity_t *ent )
{
	if ( !ent || !ent->client || !ent->client->pers.isBot )
	{
		return;
	}

	team_t team = static_cast<team_t>( ent->client->pers.team );
	if ( !G_IsPlayableTeam( team ) )
	{
		return;
	}

	if ( level.time < ent->botMind->overloadNextPurchaseTime )
	{
		return;
	}

	const int spendableCredits = ent->client->pers.credit - BotReserveCredits( team );
	if ( spendableCredits <= 0 )
	{
		return;
	}

	int purchaseIndex = -1;
	if ( G_GetFreeBudget( team ) < OVERLOAD_BOT_BP_PRESSURE_THRESHOLD &&
	     BotBuildablesRecentlyKilled( team ) )
	{
		purchaseIndex = FindBotBPPurchase( team );
	}

	if ( purchaseIndex < 0 )
	{
		purchaseIndex = FindBotPartialPurchase( team );
		if ( purchaseIndex >= 0 )
		{
			ent->botMind->overloadTargetPurchase = purchaseIndex;
		}
		else
		{
			purchaseIndex = ent->botMind->overloadTargetPurchase;
			if ( OverloadEntryRemainingSpendCapacity( team, purchaseIndex ) <= 0 )
			{
				purchaseIndex = FindBotFreshPurchase( team );
				ent->botMind->overloadTargetPurchase = purchaseIndex;
			}
		}
	}

	if ( purchaseIndex < 0 )
	{
		ent->botMind->overloadNextPurchaseTime = level.time + OVERLOAD_BOT_PURCHASE_COOLDOWN;
		return;
	}

	const int remaining = OverloadEntryRemainingSpendCapacity( team, purchaseIndex );
	if ( remaining <= 0 )
	{
		ent->botMind->overloadTargetPurchase = -1;
		ent->botMind->overloadNextPurchaseTime = level.time + OVERLOAD_BOT_FAILURE_COOLDOWN;
		return;
	}

	int spend = std::min( spendableCredits, remaining );
	if ( remaining > OVERLOAD_BOT_SPEND_CHUNK )
	{
		spend = std::min( spend, OVERLOAD_BOT_SPEND_CHUNK );
	}

	if ( spend <= 0 )
	{
		ent->botMind->overloadNextPurchaseTime = level.time + OVERLOAD_BOT_FAILURE_COOLDOWN;
		return;
	}

	std::string error;
	if ( !G_OverloadPurchaseByIndex( ent, purchaseIndex, spend, &error ) )
	{
		Log::Warn( "%s: Bot failed purchase: %d: %s", ent->client->pers.netname, purchaseIndex, error );
		ent->botMind->overloadTargetPurchase = -1;
		ent->botMind->overloadNextPurchaseTime = level.time + OVERLOAD_BOT_FAILURE_COOLDOWN;
		return;
	}

	ent->botMind->overloadNextPurchaseTime = level.time + OVERLOAD_BOT_PURCHASE_COOLDOWN;

	const int nextPartialPurchase = FindBotPartialPurchase( team );
	if ( nextPartialPurchase >= 0 )
	{
		ent->botMind->overloadTargetPurchase = nextPartialPurchase;
	}
	else if ( OverloadEntryRemainingSpendCapacity( team, purchaseIndex ) > 0 )
	{
		ent->botMind->overloadTargetPurchase = purchaseIndex;
	}
	else
	{
		ent->botMind->overloadTargetPurchase = FindBotFreshPurchase( team );
	}
}
