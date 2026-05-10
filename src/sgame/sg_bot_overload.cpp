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

static constexpr int OVERLOAD_BOT_PURCHASE_COOLDOWN = 3000;
static constexpr int OVERLOAD_BOT_FAILURE_COOLDOWN = 1000;
static constexpr int OVERLOAD_BOT_SPEND_CHUNK = 200;
static constexpr int OVERLOAD_BOT_HUMAN_RESERVE = 400;
static constexpr int OVERLOAD_BOT_ALIEN_RESERVE = 400;
static constexpr int OVERLOAD_BOT_BP_PRESSURE_THRESHOLD = 20;

static int BotReserveCredits( team_t team )
{
	return team == TEAM_ALIENS ? OVERLOAD_BOT_ALIEN_RESERVE : OVERLOAD_BOT_HUMAN_RESERVE;
}

static int FindBotPartialPurchase( team_t team )
{
	for ( int i = 0; i < G_OverloadPurchaseCount(); ++i )
	{
		if ( G_OverloadEntryIsPartial( team, i ) )
		{
			return i;
		}
	}

	return -1;
}

static int FindBotFreshPurchase( team_t team )
{
	if ( G_GetFreeBudget( team ) < OVERLOAD_BOT_BP_PRESSURE_THRESHOLD )
	{
		return 0;
	}

	for ( int i = 0; i < G_OverloadPurchaseCount(); ++i )
	{
		if ( G_OverloadEntryCanBotStart( team, i ) )
		{
			return i;
		}
	}

	return -1;
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

	if ( level.time < level.team[ team ].overloadBotNextPurchaseTime )
	{
		return;
	}

	const int spendableCredits = ent->client->pers.credit - BotReserveCredits( team );
	if ( spendableCredits <= 0 )
	{
		return;
	}

	int purchaseIndex = FindBotPartialPurchase( team );
	if ( purchaseIndex >= 0 )
	{
		level.team[ team ].overloadBotTargetPurchase = purchaseIndex;
	}
	else
	{
		purchaseIndex = level.team[ team ].overloadBotTargetPurchase;
		if ( G_OverloadEntryRemainingSpendCapacity( team, purchaseIndex ) <= 0 )
		{
			purchaseIndex = FindBotFreshPurchase( team );
			level.team[ team ].overloadBotTargetPurchase = purchaseIndex;
		}
	}

	if ( purchaseIndex < 0 )
	{
		level.team[ team ].overloadBotNextPurchaseTime = level.time + OVERLOAD_BOT_PURCHASE_COOLDOWN;
		return;
	}

	const int remaining = G_OverloadEntryRemainingSpendCapacity( team, purchaseIndex );
	if ( remaining <= 0 )
	{
		level.team[ team ].overloadBotTargetPurchase = -1;
		level.team[ team ].overloadBotNextPurchaseTime = level.time + OVERLOAD_BOT_FAILURE_COOLDOWN;
		return;
	}

	int spend = std::min( spendableCredits, remaining );
	if ( remaining > OVERLOAD_BOT_SPEND_CHUNK )
	{
		spend = std::min( spend, OVERLOAD_BOT_SPEND_CHUNK );
	}

	if ( spend <= 0 )
	{
		level.team[ team ].overloadBotNextPurchaseTime = level.time + OVERLOAD_BOT_FAILURE_COOLDOWN;
		return;
	}

	if ( !G_OverloadPurchaseByIndex( ent, purchaseIndex, spend ) )
	{
		level.team[ team ].overloadBotTargetPurchase = -1;
		level.team[ team ].overloadBotNextPurchaseTime = level.time + OVERLOAD_BOT_FAILURE_COOLDOWN;
		return;
	}

	level.team[ team ].overloadBotNextPurchaseTime = level.time + OVERLOAD_BOT_PURCHASE_COOLDOWN;

	if ( G_OverloadEntryRemainingSpendCapacity( team, purchaseIndex ) <= 0 )
	{
		level.team[ team ].overloadBotTargetPurchase = -1;
	}
}
