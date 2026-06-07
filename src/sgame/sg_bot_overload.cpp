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

static constexpr int OVERLOAD_BOT_PURCHASE_COOLDOWN = 3000;
static constexpr int OVERLOAD_BOT_FAILURE_COOLDOWN = 1000;
static constexpr int OVERLOAD_BOT_SPEND_CHUNK = 200;
static constexpr int OVERLOAD_BOT_HUMAN_RESERVE = 200;
static constexpr int OVERLOAD_BOT_ALIEN_RESERVE = 200;

static int BotReserveCredits( team_t team )
{
	return team == TEAM_ALIENS ? OVERLOAD_BOT_ALIEN_RESERVE : OVERLOAD_BOT_HUMAN_RESERVE;
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

	if ( !G_ActiveMainBuildable( team ) )
	{
		return;
	}

	std::string error;
	if ( !G_OverloadAutoDonate( ent, std::min( spendableCredits, OVERLOAD_BOT_SPEND_CHUNK ), &error ) )
	{
		if ( error == "no eligible overload purchase found" )
		{
			ent->botMind->overloadNextPurchaseTime = level.time + OVERLOAD_BOT_PURCHASE_COOLDOWN;
			return;
		}

		Log::Warn( "%s: Bot failed autodonate: %s", ent->client->pers.netname, error.c_str() );
		ent->botMind->overloadNextPurchaseTime = level.time + OVERLOAD_BOT_FAILURE_COOLDOWN;
		return;
	}

	ent->botMind->overloadNextPurchaseTime = level.time + OVERLOAD_BOT_PURCHASE_COOLDOWN;
}
