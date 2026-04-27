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

#include "common/Common.h"
#include "bg_teamprogress.h"

// ----
// data
// ----

bool         unlockablesDataAvailable;
int unlockablesTeamKnowledge; // bit mask of (1 << team)

unlockable_t     unlockables[ NUM_UNLOCKABLES ];
static int       unlockablesMask[ NUM_TEAMS ];

int              unlockablesTypeOffset[ UNLT_NUM_UNLOCKABLETYPES ];

// -------------
// local methods
// -------------

const char *BG_UnlockableHumanName( unlockable_t *unlockable )
{
	switch ( unlockable->type )
	{
		case UNLT_WEAPON:    return BG_Weapon( unlockable->num )->humanName;
		case UNLT_UPGRADE:   return BG_Upgrade( unlockable->num )->humanName;
		case UNLT_BUILDABLE: return BG_Buildable( unlockable->num )->humanName;
		case UNLT_CLASS:     return BG_ClassModelConfig( unlockable->num )->humanName;
	}

	Sys::Error( "BG_UnlockableHumanName: Unlockable has unknown type" );
}

static bool Unlocked( unlockableType_t type, int itemNum )
{
	return unlockables[ unlockablesTypeOffset[ type ] + itemNum ].unlocked;
}

static void CheckStatusKnowledge( unlockableType_t type, int itemNum )
{
	unlockable_t dummy;

	if ( !unlockables[ unlockablesTypeOffset[ type ] + itemNum ].statusKnown )
	{
		dummy.type = type;
		dummy.num  = itemNum;

		Log::Warn( "Asked for the status of unlockable item %s but the status is unknown.",
		            BG_UnlockableHumanName( &dummy ) );
	}
}

float BG_UnlockToLockThreshold( float unlockThreshold )
{
	return unlockThreshold;
}

int BG_NormalizeUnlockThreshold( int unlockThreshold )
{
	if ( unlockThreshold <= 0 )
	{
		return 0;
	}

	return unlockThreshold >= 200 ? 6 : 3;
}

void BG_ClearUnlockablesMasks()
{
	memset( unlockablesMask, 0, sizeof( unlockablesMask ) );
}

void BG_ResetUnlockablesMask( team_t team )
{
	unlockablesMask[ team ] = 0;
}

void BG_SetUnlockablesMaskBit( team_t team, int bit )
{
	unlockablesMask[ team ] |= ( 1 << bit );
}

void BG_SetUnlockablesMask( team_t team, int mask )
{
	unlockablesMask[ team ] = mask;
}

// ----------
// BG methods
// ----------

void BG_InitUnlockackables()
{
	unlockablesDataAvailable = false;
	unlockablesTeamKnowledge = 0;

	memset( unlockables, 0, sizeof( unlockables ) );
	BG_ClearUnlockablesMasks();

	unlockablesTypeOffset[ UNLT_WEAPON ]    = 0;
	unlockablesTypeOffset[ UNLT_UPGRADE ]   = WP_NUM_WEAPONS;
	unlockablesTypeOffset[ UNLT_BUILDABLE ] = unlockablesTypeOffset[ UNLT_UPGRADE ]   + UP_NUM_UPGRADES;
	unlockablesTypeOffset[ UNLT_CLASS ]     = unlockablesTypeOffset[ UNLT_BUILDABLE ] + BA_NUM_BUILDABLES;
}

void BG_ImportUnlockablesFromMask( int team, int mask )
{
	int              unlockableNum, teamUnlockableNum = 0, itemNum = 0, unlockThreshold;
	unlockable_t     *unlockable;
	int unlockableType = 0;
	team_t           currentTeam;
	bool         newStatus;

	// maintain a cache to prevent redundant imports
	static int    lastMask = 0;
	static team_t lastTeam = TEAM_NONE;

	// just import if data is unavailable, cached mask is outdated or team has changed
	if ( unlockablesDataAvailable && team == lastTeam && mask == lastMask )
	{
		return;
	}

	// cache input
	lastMask = mask;
	lastTeam = static_cast<team_t>( team );

	int statusChanges[ NUM_UNLOCKABLES ]{};
	int statusChangeCount = 0;

	for ( unlockableNum = 0; unlockableNum < NUM_UNLOCKABLES; unlockableNum++ )
	{
		unlockable = &unlockables[ unlockableNum ];

		// also iterate over item types, itemNum is a per-type counter
		if ( unlockableType < UNLT_NUM_UNLOCKABLETYPES - 1 &&
		     unlockableNum == unlockablesTypeOffset[ unlockableType + 1 ] )
		{
			unlockableType++;
			itemNum = 0;
		}

		switch ( unlockableType )
		{
			case UNLT_WEAPON:
				currentTeam     = BG_Weapon( itemNum )->team;
				unlockThreshold = BG_Weapon( itemNum )->unlockThreshold;
				break;

			case UNLT_UPGRADE:
				currentTeam     = BG_Upgrade( itemNum )->team;
				unlockThreshold = BG_Upgrade( itemNum )->unlockThreshold;
				break;

			case UNLT_BUILDABLE:
				currentTeam     = BG_Buildable( itemNum )->team;
				unlockThreshold = BG_Buildable( itemNum )->unlockThreshold;
				break;

			case UNLT_CLASS:
				currentTeam     = BG_Class( itemNum )->team;
				unlockThreshold = BG_Class( itemNum )->unlockThreshold;
				break;

			default:
				Sys::Error( "BG_ImportUnlockablesFromMask: Unknown unlockable type" );
		}

		unlockThreshold = BG_NormalizeUnlockThreshold( std::max( unlockThreshold, 0 ) );

		unlockable->type            = unlockableType;
		unlockable->num             = itemNum;
		unlockable->team            = currentTeam;
		unlockable->unlockThreshold = unlockThreshold;
		unlockable->lockThreshold   = BG_UnlockToLockThreshold( unlockThreshold );

		// retrieve the item's locking state
		if ( !unlockThreshold )
		{
			unlockable->statusKnown = true;
			unlockable->unlocked    = true;
		}
		else if ( currentTeam == team )
		{
			newStatus = mask & ( 1 << teamUnlockableNum );

			if ( unlockablesTeamKnowledge == (1 << team) && unlockable->statusKnown &&
			     unlockable->unlocked != newStatus )
			{
				statusChanges[ unlockableNum ] = newStatus ? 1 : -1;
				statusChangeCount++;
			}

			unlockable->statusKnown = true;
			unlockable->unlocked    = newStatus;

			teamUnlockableNum++;
		}
		else
		{
			unlockable->statusKnown = false;
			unlockable->unlocked    = false;
		}

		itemNum++;
	}

	if ( statusChangeCount )
	{
		BG_TeamProgressNotifyStatusChanges( statusChanges, statusChangeCount );
	}

	// we only know the state for one team
	unlockablesDataAvailable = true;
	unlockablesTeamKnowledge = 1 << team;

	// save mask for later use
	BG_SetUnlockablesMask( static_cast<team_t>( team ), mask );
}

int BG_UnlockablesMask( int team )
{
	if ( !( unlockablesTeamKnowledge & ( 1 << team ) ) )
	{
		Sys::Error( "BG_UnlockablesMask: Requested mask for a team with unknown unlockable status" );
	}

	return unlockablesMask[ team ];
}

unlockableType_t BG_UnlockableType( int num )
{
	return (unlockableType_t) ( ( (unsigned) num < NUM_UNLOCKABLES ) ? unlockables[ num ].type : UNLT_NUM_UNLOCKABLETYPES );
}

int BG_UnlockableTypeIndex( int num )
{
	return ( (unsigned) num < NUM_UNLOCKABLES ) ? unlockables[ num ].num : 0;
}

bool BG_WeaponUnlocked( int weapon )
{
	CheckStatusKnowledge( UNLT_WEAPON, weapon);

	return Unlocked( UNLT_WEAPON, weapon);
}

bool BG_UpgradeUnlocked( int upgrade )
{
	CheckStatusKnowledge( UNLT_UPGRADE, upgrade);

	return Unlocked( UNLT_UPGRADE, upgrade);
}

bool BG_BuildableUnlocked( int buildable )
{
	CheckStatusKnowledge( UNLT_BUILDABLE, buildable);

	return Unlocked( UNLT_BUILDABLE, buildable);
}

bool BG_ClassUnlocked( int class_ )
{
	CheckStatusKnowledge( UNLT_CLASS, class_);

	return Unlocked( UNLT_CLASS, class_);
}

static int NextUnlockThreshold( int threshold )
{
	int next = 1 << 30;
	int i;

	for ( i = 0; i < NUM_UNLOCKABLES; ++i )
	{
		int thisThreshold = unlockables[ i ].unlocked ? unlockables[ i ].lockThreshold : unlockables[ i ].unlockThreshold;

		if ( thisThreshold > threshold && thisThreshold < next )
		{
			next = thisThreshold;
		}
	}

	return next < ( 1 << 30 ) ? next : 0;
}

unlockThresholdIterator_t BG_IterateUnlockThresholds( unlockThresholdIterator_t unlockableIter, team_t team, int *threshold, bool *unlocked )
{
	static const unlockThresholdIterator_t finished = { -1, 0 };

	if ( unlockableIter.num < 0 )
	{
		unlockableIter.num = -1;
	}

	for ( ++unlockableIter.num; unlockableIter.num < NUM_UNLOCKABLES; unlockableIter.num++ )
	{
		unlockable_t unlockable = unlockables[ unlockableIter.num ];
		int          thisThreshold = unlockable.unlocked ? unlockable.lockThreshold : unlockable.unlockThreshold;

		if ( unlockable.team == team && unlockable.unlockThreshold && ( !unlockableIter.threshold || unlockableIter.threshold == thisThreshold ) )
		{
			*unlocked = unlockable.unlocked;
			*threshold = thisThreshold;

			return unlockableIter;
		}
	}

	if ( unlockableIter.threshold )
	{
			unlockableIter.threshold = NextUnlockThreshold( unlockableIter.threshold );

		if ( unlockableIter.threshold )
		{
			unlockableIter.num = -1;
				return BG_IterateUnlockThresholds( unlockableIter, team, threshold, unlocked );
		}
	}

	return finished;
}
