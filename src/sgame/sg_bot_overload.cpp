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
#include "sg_bot_ai.h"

#include <algorithm>

AINodeStatus_t BotActionBuyOverloadUpgrades( gentity_t *ent, AIGenericNode_t *node )
{
	if ( !ent || !ent->client || !ent->botMind || !ent->client->pers.isBot )
	{
		return STATUS_FAILURE;
	}

	team_t team = static_cast<team_t>( ent->client->pers.team );
	if ( !G_IsPlayableTeam( team ) )
	{
		return STATUS_FAILURE;
	}

	if ( !G_ActiveMainBuildable( team ) )
	{
		return STATUS_SUCCESS;
	}

	AIActionNode_t *action = reinterpret_cast<AIActionNode_t *>( node );
	const int availableCredits = std::max( 0, static_cast<int>( ent->client->pers.credit ) );
	const int requestedCredits = action->nparams > 0 ? AIUnBoxInt( action->params[ 0 ] ) : availableCredits;
	const int spend = std::min( std::max( 0, requestedCredits ), availableCredits );
	if ( spend <= 0 )
	{
		return STATUS_SUCCESS;
	}

	std::string error;
	if ( !G_OverloadAutoDonate( ent, spend, &error ) )
	{
		if ( error == "no eligible overload purchase found" )
		{
			return STATUS_SUCCESS;
		}

		Log::Warn( "%s: Bot failed autodonate: %s", ent->client->pers.netname, error.c_str() );
		return STATUS_FAILURE;
	}

	return STATUS_SUCCESS;
}
