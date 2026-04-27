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

#include "cg_local.h"
#include "../shared/bg_teamprogress.h"

static bool Disabled( unlockable_t *unlockable )
{
	switch ( unlockable->type )
	{
		case UNLT_WEAPON:    return BG_WeaponDisabled( unlockable->num );
		case UNLT_UPGRADE:   return BG_UpgradeDisabled( unlockable->num );
		case UNLT_BUILDABLE: return BG_BuildableDisabled( unlockable->num );
		case UNLT_CLASS:     return BG_ClassDisabled( unlockable->num );
	}

	Sys::Error( "Disabled: Unlockable has unknown type" );
}

void BG_TeamProgressNotifyStatusChanges( const int *statusChanges, int count )
{
	std::string text;
	bool        firstPass = true, unlocked = true;

	for ( int unlockableNum = 0; unlockableNum < NUM_UNLOCKABLES; unlockableNum++ )
	{
		unlockable_t *unlockable = &unlockables[ unlockableNum ];

		if ( !statusChanges[ unlockableNum ] || Disabled( unlockable ) )
		{
			continue;
		}

		if ( firstPass )
		{
			if ( statusChanges[ unlockableNum ] > 0 )
			{
				text += Str::Format( "^2ITEM%s UNLOCKED: ^*", ( count > 1 ) ? "S" : "" );
			}
			else
			{
				unlocked = false;
				text += Str::Format( "^1ITEM%s LOCKED: ^*", ( count > 1 ) ? "S" : "" );
			}
			firstPass = false;
		}
		else
		{
			text += ", ";
		}

		text += BG_UnlockableHumanName( unlockable );
	}

	switch ( cg.snap->ps.persistant[ PERS_TEAM ] )
	{
		case TEAM_ALIENS:
			if ( unlocked )
			{
				trap_S_StartLocalSound( cgs.media.weHaveEvolved, soundChannel_t::CHAN_ANNOUNCER );
			}
			break;

		case TEAM_HUMANS:
		default:
			if ( unlocked )
			{
				trap_S_StartLocalSound( cgs.media.reinforcement, soundChannel_t::CHAN_ANNOUNCER );
			}
			break;
	}

	CG_CenterPrint( text.c_str(), 1.0f );
}

void CG_UpdateUnlockables( playerState_t *ps )
{
	BG_ImportUnlockablesFromMask( ps->persistant[ PERS_TEAM ], ps->persistant[ PERS_UNLOCKABLES ] );
}
