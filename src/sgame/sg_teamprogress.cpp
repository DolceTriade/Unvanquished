/*
===========================================================================

Unvanquished GPL Source Code
Copyright (C) 2013 Unvanquished Developers

This file is part of the Unvanquished GPL Source Code (Unvanquished Source Code).

Daemon is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Unvanquished is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Foobar.  If not, see <http://www.gnu.org/licenses/>.

===========================================================================
*/

#include "sg_local.h"
#include "../shared/bg_teamprogress.h"

static void UpdateUnlockablesMask()
{
	int unlockable, unlockableNum[ NUM_TEAMS ];
	int team;

	for ( team = TEAM_NONE + 1; team < NUM_TEAMS; team++ )
	{
		unlockableNum[ team ] = 0;
		BG_ResetUnlockablesMask( static_cast<team_t>( team ) );
	}

	for ( unlockable = 0; unlockable < NUM_UNLOCKABLES; unlockable++ )
	{
		if ( unlockables[ unlockable ].unlockThreshold )
		{
			team = unlockables[ unlockable ].team;

			if ( unlockableNum[ team ] > 15 )
			{
				Sys::Error( "UpdateUnlockablesMask: Number of unlockable items for a team exceeded" );
			}

			if ( !unlockables[ unlockable ].statusKnown )
			{
				Sys::Error( "UpdateUnlockablesMask: Called before G_UpdateUnlockables" );
			}

			if ( unlockables[ unlockable ].unlocked )
			{
				BG_SetUnlockablesMaskBit( static_cast<team_t>( team ), unlockableNum[ team ] );
			}

			unlockableNum[ team ]++;
		}
	}
}

void BG_TeamProgressNotifyStatusChanges( const int *, int )
{
}

void G_UpdateUnlockables()
{
	int          itemNum = 0, unlockableNum, unlockThreshold;
	unlockable_t *unlockable;
	int          unlockableType = 0;
	team_t       team;

	for ( unlockableNum = 0; unlockableNum < NUM_UNLOCKABLES; unlockableNum++ )
	{
		unlockable = &unlockables[ unlockableNum ];

		while ( unlockableType < UNLT_NUM_UNLOCKABLETYPES - 1 &&
		        unlockableNum == unlockablesTypeOffset[ unlockableType + 1 ] )
		{
			unlockableType++;
			itemNum = 0;
		}

		switch ( unlockableType )
		{
			case UNLT_WEAPON:
				team            = BG_Weapon( itemNum )->team;
				unlockThreshold = BG_Weapon( itemNum )->unlockThreshold;
				break;

			case UNLT_UPGRADE:
				team            = BG_Upgrade( itemNum )->team;
				unlockThreshold = BG_Upgrade( itemNum )->unlockThreshold;
				break;

			case UNLT_BUILDABLE:
				team            = BG_Buildable( itemNum )->team;
				unlockThreshold = BG_Buildable( itemNum )->unlockThreshold;
				break;

			case UNLT_CLASS:
				team            = BG_Class( itemNum )->team;
				unlockThreshold = BG_Class( itemNum )->unlockThreshold;
				break;

			default:
				Sys::Error( "G_UpdateUnlockables: Unknown unlockable type" );
		}

		int rawThreshold = std::max( unlockThreshold, 0 );
		unlockThreshold = NormalizeUnlockThreshold( rawThreshold );

		unlockable->type            = unlockableType;
		unlockable->num             = itemNum;
		unlockable->team            = team;
		unlockable->statusKnown     = true;
		unlockable->unlockThreshold = unlockThreshold;
		unlockable->lockThreshold   = UnlockToLockThreshold( unlockThreshold );

		if ( rawThreshold == 0 )
		{
			unlockable->unlocked = true;
		}
		else
		{
			unlockable->unlocked = G_OverloadUnlockPurchased( team, static_cast<unlockableType_t>( unlockableType ), itemNum );
		}

		itemNum++;
	}

	unlockablesDataAvailable = true;
	unlockablesTeamKnowledge = ~0;
	UpdateUnlockablesMask();
}
