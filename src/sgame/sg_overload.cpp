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
#include "shared/bg_teamprogress.h"

#include <limits>
#include <sstream>
#include <vector>

std::vector<overloadPurchaseDef_t> overloadPurchases;
bool overloadCatalogReady = false;

static void BuildOverloadCatalog();
static int FindUnlockThresholdField( bgAttributeFamily_t family );
static int OverloadNextCost( const overloadPurchaseDef_t& entry, int entryIndex, team_t team );
static int AutoDonateSpendCapacity( team_t team, int purchaseIndex );


static bool OverloadEntryMatchesTeam( team_t team, int purchaseIndex )
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
	return entry.team == TEAM_NONE || entry.team == team;
}

static bool OverloadEntryCanAutoDonate( team_t team, int purchaseIndex )
{
	if ( !OverloadEntryMatchesTeam( team, purchaseIndex ) )
	{
		return false;
	}

	const overloadPurchaseDef_t& entry = overloadPurchases[ purchaseIndex ];
	return EntryIsAvailable( team, entry ) && RemainingSpendCapacity( entry, purchaseIndex, team ) > 0;
}

static bool OverloadEntryIsPartial( team_t team, int purchaseIndex )
{
	if ( !OverloadEntryCanAutoDonate( team, purchaseIndex ) )
	{
		return false;
	}

	return TeamEconomy( team ).investedCredits[ purchaseIndex ] > 0;
}

static bool OverloadEntryIsUnlock( team_t team, int purchaseIndex )
{
	return OverloadEntryMatchesTeam( team, purchaseIndex ) &&
	       overloadPurchases[ purchaseIndex ].kind == overloadPurchaseKind_t::UNLOCK;
}

static bool OverloadEntryIsBPBundle( team_t team, int purchaseIndex )
{
	return OverloadEntryMatchesTeam( team, purchaseIndex ) &&
	       overloadPurchases[ purchaseIndex ].kind == overloadPurchaseKind_t::BP_BUNDLE;
}

static bool OverloadEntryIsUpgrade( team_t team, int purchaseIndex )
{
	return OverloadEntryMatchesTeam( team, purchaseIndex ) &&
	       overloadPurchases[ purchaseIndex ].kind == overloadPurchaseKind_t::UPGRADE;
}

static bool TeamHasEstimatedBuildable( buildable_t buildable )
{
	return level.numBuildablesEstimate[ buildable ] > 0;
}

static int TeamAliveBuildableValue( team_t team )
{
	int total = 0;

	for ( int buildable = BA_NONE + 1; buildable < BA_NUM_BUILDABLES; ++buildable )
	{
		const buildableAttributes_t* attributes = BG_Buildable( buildable );
		if ( !attributes || attributes->team != team )
		{
			continue;
		}

		total += level.numBuildablesEstimate[ buildable ] * attributes->buildPoints;
	}

	return total;
}

static bool TeamNeedsAutoDonateBP( team_t team )
{
	static constexpr int OVERLOAD_AUTODONATE_BP_LOW_FREE_THRESHOLD = 10;
	static constexpr int OVERLOAD_AUTODONATE_BP_LOW_BASE_THRESHOLD = 75;
	static constexpr int OVERLOAD_AUTODONATE_BP_LOW_SPAWN_THRESHOLD = 2;

	if ( G_GetFreeBudget( team ) >= OVERLOAD_AUTODONATE_BP_LOW_FREE_THRESHOLD )
	{
		return false;
	}

	if ( level.team[ team ].numSpawns < OVERLOAD_AUTODONATE_BP_LOW_SPAWN_THRESHOLD )
	{
		return true;
	}

	if ( team == TEAM_HUMANS &&
	     ( !TeamHasEstimatedBuildable( BA_H_ARMOURY ) ||
	       !TeamHasEstimatedBuildable( BA_H_MEDISTAT ) ) )
	{
		return true;
	}

	if ( team == TEAM_ALIENS && BG_BuildableUnlocked( BA_A_BOOSTER ) &&
	     !TeamHasEstimatedBuildable( BA_A_BOOSTER ) )
	{
		return true;
	}

	return TeamAliveBuildableValue( team ) < OVERLOAD_AUTODONATE_BP_LOW_BASE_THRESHOLD;
}

static int FindAutoDonatePartialPurchase( team_t team )
{
	int bestPurchaseIndex = -1;
	int bestRemaining = std::numeric_limits<int>::max();

	for ( int i = 0; i < G_OverloadPurchaseCount(); ++i )
	{
		if ( !OverloadEntryIsPartial( team, i ) )
		{
			continue;
		}

		const int remaining = AutoDonateSpendCapacity( team, i );
		if ( remaining > 0 && remaining < bestRemaining )
		{
			bestPurchaseIndex = i;
			bestRemaining = remaining;
		}
	}

	return bestPurchaseIndex;
}

static int FindAutoDonateBPPurchase( team_t team )
{
	if ( !TeamNeedsAutoDonateBP( team ) )
	{
		return -1;
	}

	for ( int i = 0; i < G_OverloadPurchaseCount(); ++i )
	{
		if ( OverloadEntryCanAutoDonate( team, i ) && OverloadEntryIsBPBundle( team, i ) )
		{
			return i;
		}
	}

	return -1;
}

static int FindAutoDonateUnlockPurchase( team_t team )
{
	int bestPurchaseIndex = -1;
	int bestRemaining = std::numeric_limits<int>::max();

	for ( int i = 0; i < G_OverloadPurchaseCount(); ++i )
	{
		if ( !OverloadEntryCanAutoDonate( team, i ) || !OverloadEntryIsUnlock( team, i ) )
		{
			continue;
		}

		const int remaining = RemainingSpendCapacity( overloadPurchases[ i ], i, team );
		if ( remaining > 0 && remaining < bestRemaining )
		{
			bestPurchaseIndex = i;
			bestRemaining = remaining;
		}
	}

	return bestPurchaseIndex;
}

static int FindAutoDonateUpgradePurchase( team_t team )
{
	std::vector<int> upgradePurchaseIndices;

	for ( int i = 0; i < G_OverloadPurchaseCount(); ++i )
	{
		if ( OverloadEntryCanAutoDonate( team, i ) && OverloadEntryIsUpgrade( team, i ) )
		{
			upgradePurchaseIndices.push_back( i );
		}
	}

	if ( upgradePurchaseIndices.empty() )
	{
		return -1;
	}

	const int randomIndex = static_cast<int>( BG_random() * upgradePurchaseIndices.size() );
	return upgradePurchaseIndices[ std::min( randomIndex, static_cast<int>( upgradePurchaseIndices.size() ) - 1 ) ];
}

static int FindAutoDonatePurchase( team_t team )
{
	const int bpPurchase = FindAutoDonateBPPurchase( team );
	if ( bpPurchase >= 0 )
	{
		return bpPurchase;
	}

	const int partialPurchase = FindAutoDonatePartialPurchase( team );
	if ( partialPurchase >= 0 )
	{
		return partialPurchase;
	}

	const int unlockPurchase = FindAutoDonateUnlockPurchase( team );
	if ( unlockPurchase >= 0 )
	{
		return unlockPurchase;
	}

	return FindAutoDonateUpgradePurchase( team );
}

static int AutoDonateSpendCapacity( team_t team, int purchaseIndex )
{
	if ( !OverloadEntryMatchesTeam( team, purchaseIndex ) )
	{
		return 0;
	}

	const overloadPurchaseDef_t& entry = overloadPurchases[ purchaseIndex ];
	if ( entry.kind == overloadPurchaseKind_t::BP_BUNDLE ||
	     entry.kind == overloadPurchaseKind_t::UPGRADE )
	{
		const int invested = TeamEconomy( team ).investedCredits[ purchaseIndex ];
		return std::max( 0, OverloadNextCost( entry, purchaseIndex, team ) - invested );
	}

	return RemainingSpendCapacity( entry, purchaseIndex, team );
}

static int weaponUnlockPurchaseIndex[ NUM_TEAMS ][ WP_NUM_WEAPONS ];
static int upgradeUnlockPurchaseIndex[ NUM_TEAMS ][ UP_NUM_UPGRADES ];
static int buildableUnlockPurchaseIndex[ NUM_TEAMS ][ BA_NUM_BUILDABLES ];
static int classUnlockPurchaseIndex[ NUM_TEAMS ][ PCL_NUM_CLASSES ];

static void ResetUnlockPurchaseIndexMaps()
{
	memset( weaponUnlockPurchaseIndex, -1, sizeof( weaponUnlockPurchaseIndex ) );
	memset( upgradeUnlockPurchaseIndex, -1, sizeof( upgradeUnlockPurchaseIndex ) );
	memset( buildableUnlockPurchaseIndex, -1, sizeof( buildableUnlockPurchaseIndex ) );
	memset( classUnlockPurchaseIndex, -1, sizeof( classUnlockPurchaseIndex ) );
}

static void SetUnlockPurchaseIndex( team_t team, unlockableType_t type, int itemNum, int purchaseIndex )
{
	switch ( type )
	{
		case UNLT_WEAPON:
			weaponUnlockPurchaseIndex[ team ][ itemNum ] = purchaseIndex;
			return;

		case UNLT_UPGRADE:
			upgradeUnlockPurchaseIndex[ team ][ itemNum ] = purchaseIndex;
			return;

		case UNLT_BUILDABLE:
			buildableUnlockPurchaseIndex[ team ][ itemNum ] = purchaseIndex;
			return;

		case UNLT_CLASS:
			classUnlockPurchaseIndex[ team ][ itemNum ] = purchaseIndex;
			return;

		case UNLT_NUM_UNLOCKABLETYPES:
			break;
	}

	Sys::Error( "SetUnlockPurchaseIndex: invalid unlockable type" );
}

static int GetUnlockPurchaseIndex( team_t team, unlockableType_t type, int itemNum )
{
	switch ( type )
	{
		case UNLT_WEAPON:
			return weaponUnlockPurchaseIndex[ team ][ itemNum ];

		case UNLT_UPGRADE:
			return upgradeUnlockPurchaseIndex[ team ][ itemNum ];

		case UNLT_BUILDABLE:
			return buildableUnlockPurchaseIndex[ team ][ itemNum ];

		case UNLT_CLASS:
			return classUnlockPurchaseIndex[ team ][ itemNum ];

		case UNLT_NUM_UNLOCKABLETYPES:
			break;
	}

	Sys::Error( "GetUnlockPurchaseIndex: invalid unlockable type" );
	return -1;
}

constexpr int OVERLOAD_UNCAPPED_RANKS = std::numeric_limits<int>::max();

int G_InitialBudgetForTeam( team_t team )
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

TeamEconomyState& TeamEconomy( team_t team )
{
	return level.team[ team ].economy;
}

static int OverloadCostMultiplierPermille( team_t team )
{
	const TeamEconomyState& economy = TeamEconomy( team );
	const int extraPlayers = std::max( 0, economy.peakClientsSeen - 1 );
	const float scale = std::max( 1.0f, 1.0f + g_overloadCostPerPlayer.Get() * extraPlayers );
	return std::max( 1000, static_cast<int>( std::lround( scale * 1000.0f ) ) );
}

static int ScaleOverloadCost( team_t team, int cost )
{
	if ( cost <= 0 )
	{
		return cost;
	}

	const int64_t scaled = static_cast<int64_t>( cost ) * OverloadCostMultiplierPermille( team );
	return static_cast<int>( ( scaled + 999 ) / 1000 );
}

static int OverloadNextCost( const overloadPurchaseDef_t& entry, int entryIndex, team_t team )
{
	if ( entry.kind == overloadPurchaseKind_t::UPGRADE || entry.kind == overloadPurchaseKind_t::BP_BUNDLE )
	{
		return ScaleOverloadCost( team, entry.baseCost + TeamEconomy( team ).repeatCounts[ entryIndex ] * entry.costStep );
	}

	return ScaleOverloadCost( team, entry.baseCost );
}

static std::string FormatOverloadCurrency( int value, team_t team )
{
	if ( team == TEAM_ALIENS )
	{
		int tenths = value * 10 / CREDITS_PER_EVO;
		return Str::Format( "%d.%d morph points", tenths / 10, std::abs( tenths % 10 ) );
	}

	return Str::Format( "%d credits", value );
}

static std::string EncodeIndexValuePairs( const int* values )
{
	std::ostringstream stream;
	bool first = true;

	for ( int i = 0; i < MAX_OVERLOAD_PURCHASES; ++i )
	{
		if ( values[ i ] == 0 )
		{
			continue;
		}

		if ( !first )
		{
			stream << ',';
		}

		stream << i << ':' << values[ i ];
		first = false;
	}

	return stream.str();
}

static std::string EncodeOwnedPurchases( const bool* values )
{
	std::ostringstream stream;
	bool first = true;

	for ( int i = 0; i < MAX_OVERLOAD_PURCHASES; ++i )
	{
		if ( !values[ i ] )
		{
			continue;
		}

		if ( !first )
		{
			stream << ',';
		}

		stream << i;
		first = false;
	}

	return stream.str();
}

static void PublishOverloadStateInternal( team_t team )
{
	if ( !G_IsPlayableTeam( team ) )
	{
		return;
	}

	const TeamEconomyState& economy = TeamEconomy( team );
	std::ostringstream stream;
	stream << "cp=" << economy.completedPurchases
	       << ";bp=" << economy.bpPurchased
	       << ";tb=" << level.team[ team ].totalBudget
	       << ";sb=" << level.team[ team ].spentBudget
	       << ";cm=" << OverloadCostMultiplierPermille( team )
	       << ";ic=" << EncodeIndexValuePairs( economy.investedCredits )
	       << ";rc=" << EncodeIndexValuePairs( economy.repeatCounts )
	       << ";op=" << EncodeOwnedPurchases( economy.ownedPurchases );

	std::string config = stream.str();
	if ( config.size() >= BIG_INFO_STRING )
	{
		Sys::Error( "team economy configstring exceeded BIG_INFO_STRING (%zu >= %d)",
		            config.size(), BIG_INFO_STRING );
	}

	trap_SetConfigstring( CS_OVERLOAD + static_cast<int>( team ), config.c_str() );
}

static void PublishAllTeamEconomyStates()
{
	for ( team_t team = TEAM_NONE; ( team = G_IterateTeams( team ) ); )
	{
		::G_PublishOverloadState( team );
	}
}

static void UpdateOverloadCostScalingInternal()
{
	for ( team_t team = TEAM_NONE; ( team = G_IterateTeams( team ) ); )
	{
		TeamEconomyState& economy = TeamEconomy( team );
		const int currentClients = level.team[ team ].numClients;
		if ( currentClients <= economy.peakClientsSeen )
		{
			continue;
		}

		economy.peakClientsSeen = currentClients;
		PublishOverloadStateInternal( team );
	}
}

static const char* PurchaseKindToken( overloadPurchaseKind_t kind )
{
	switch ( kind )
	{
		case overloadPurchaseKind_t::BP_BUNDLE: return "bp";
		case overloadPurchaseKind_t::UNLOCK: return "unlock";
		case overloadPurchaseKind_t::UPGRADE: return "upgrade";
	}

	Sys::Error( "unknown overload purchase kind" );
}

static std::string UnlockableDescription( unlockableType_t type, int itemNum )
{
	switch ( type )
	{
		case UNLT_WEAPON:    return BG_Weapon( itemNum )->info ? BG_Weapon( itemNum )->info : "";
		case UNLT_UPGRADE:   return BG_Upgrade( itemNum )->info ? BG_Upgrade( itemNum )->info : "";
		case UNLT_BUILDABLE: return BG_Buildable( itemNum )->info ? BG_Buildable( itemNum )->info : "";
		case UNLT_CLASS:     return BG_Class( itemNum )->info ? BG_Class( itemNum )->info : "";
		case UNLT_NUM_UNLOCKABLETYPES: break;
	}

	Sys::Error( "UnlockableDescription: unknown unlockable type" );
}

static const char* OverloadGroupForThing( team_t team, const char* thing )
{
	if ( !thing || !*thing )
	{
		return "Other";
	}

	if ( team == TEAM_HUMANS )
	{
		if ( !Q_stricmp( thing, "humans" ) )
		{
			return "Team";
		}

		if ( !Q_stricmp( thing, "medistat" ) ||
		     !Q_stricmp( thing, "mgturret" ) ||
		     !Q_stricmp( thing, "rocketpod" ) )
		{
			return "Structures";
		}

		if ( !Q_stricmp( thing, "rifle" ) ||
		     !Q_stricmp( thing, "psaw" ) ||
		     !Q_stricmp( thing, "shotgun" ) ||
		     !Q_stricmp( thing, "lgun" ) ||
		     !Q_stricmp( thing, "mdriver" ) ||
		     !Q_stricmp( thing, "chaingun" ) ||
		     !Q_stricmp( thing, "flamer" ) ||
		     !Q_stricmp( thing, "prifle" ) ||
		     !Q_stricmp( thing, "lcannon" ) )
		{
			return "Weapons";
		}

		return "Equipment";
	}

	if ( team == TEAM_ALIENS )
	{
		if ( !Q_stricmp( thing, "aliens" ) )
		{
			return "Team";
		}

		if ( !Q_stricmp( thing, "acid_tube" ) ||
		     !Q_stricmp( thing, "booster" ) ||
		     !Q_stricmp( thing, "spiker" ) ||
		     !Q_stricmp( thing, "trapper" ) ||
		     !Q_stricmp( thing, "hive" ) )
		{
			return "Structures";
		}

		return "Lifeforms";
	}

	return "Other";
}

static int OverloadSortIndexForThing( team_t team, const char* thing )
{
	if ( !thing || !*thing )
	{
		return 999;
	}

	if ( team == TEAM_HUMANS )
	{
		if ( !Q_stricmp( thing, "bp" ) ) return 0;
		if ( !Q_stricmp( thing, "psaw" ) ) return 10;
		if ( !Q_stricmp( thing, "shotgun" ) ) return 11;
		if ( !Q_stricmp( thing, "mdriver" ) ) return 12;
		if ( !Q_stricmp( thing, "chaingun" ) ) return 13;
		if ( !Q_stricmp( thing, "flamer" ) ) return 14;
		if ( !Q_stricmp( thing, "prifle" ) ) return 15;
		if ( !Q_stricmp( thing, "lcannon" ) ) return 16;
		if ( !Q_stricmp( thing, "armor" ) ) return 20;
		if ( !Q_stricmp( thing, "radar" ) ) return 21;
		if ( !Q_stricmp( thing, "jetpack" ) ) return 22;
		if ( !Q_stricmp( thing, "biokit" ) ) return 23;
		if ( !Q_stricmp( thing, "battlesuit" ) ) return 24;
		if ( !Q_stricmp( thing, "grenade" ) ) return 25;
		if ( !Q_stricmp( thing, "firebomb" ) ) return 26;
		if ( !Q_stricmp( thing, "rifle" ) ) return 30;
		if ( !Q_stricmp( thing, "lgun" ) ) return 31;
		if ( !Q_stricmp( thing, "humans" ) ) return 40;
		if ( !Q_stricmp( thing, "medistat" ) ) return 50;
		if ( !Q_stricmp( thing, "mgturret" ) ) return 51;
		if ( !Q_stricmp( thing, "rocketpod" ) ) return 52;
		return 199;
	}

	if ( team == TEAM_ALIENS )
	{
		if ( !Q_stricmp( thing, "bp" ) ) return 0;
		if ( !Q_stricmp( thing, "builderupg" ) ) return 10;
		if ( !Q_stricmp( thing, "level0" ) ) return 11;
		if ( !Q_stricmp( thing, "level1" ) ) return 12;
		if ( !Q_stricmp( thing, "level2" ) ) return 13;
		if ( !Q_stricmp( thing, "level2upg" ) ) return 14;
		if ( !Q_stricmp( thing, "level3" ) ) return 15;
		if ( !Q_stricmp( thing, "level3upg" ) ) return 16;
		if ( !Q_stricmp( thing, "level4" ) ) return 17;
		if ( !Q_stricmp( thing, "acid_tube" ) ) return 30;
		if ( !Q_stricmp( thing, "trapper" ) ) return 31;
		if ( !Q_stricmp( thing, "spiker" ) ) return 32;
		if ( !Q_stricmp( thing, "booster" ) ) return 33;
		if ( !Q_stricmp( thing, "hive" ) ) return 34;
		if ( !Q_stricmp( thing, "aliens" ) ) return 40;
		return 199;
	}

	return 999;
}

static void PublishOverloadCatalog()
{
	for ( int i = 0; i < MAX_OVERLOAD_PURCHASES; ++i )
	{
		char config[ BIG_INFO_STRING ];
		config[ 0 ] = '\0';

		if ( i < static_cast<int>( overloadPurchases.size() ) )
		{
			const overloadPurchaseDef_t& entry = overloadPurchases[ i ];
			Info_SetValueForKey( config, "k", PurchaseKindToken( entry.kind ), false );
			Info_SetValueForKey( config, "t", va( "%d", entry.team ), false );
			Info_SetValueForKey( config, "thing", entry.thing.c_str(), false );
			Info_SetValueForKey( config, "tl", entry.thingLabel.c_str(), false );
			Info_SetValueForKey( config, "grp", entry.groupLabel.c_str(), false );
			Info_SetValueForKey( config, "ord", va( "%d", entry.sortIndex ), false );
			Info_SetValueForKey( config, "stat", entry.stat.c_str(), false );
			Info_SetValueForKey( config, "sl", entry.statLabel.c_str(), false );
			Info_SetValueForKey( config, "name", entry.displayName.c_str(), false );
			Info_SetValueForKey( config, "desc", entry.uiDescription.c_str(), false );
			Info_SetValueForKey( config, "bc", va( "%d", entry.baseCost ), false );
			Info_SetValueForKey( config, "cs", va( "%d", entry.costStep ), false );
			Info_SetValueForKey( config, "ba", va( "%d", entry.bundleAmount ), false );
			Info_SetValueForKey( config, "req", va( "%d", entry.requiredCompletedCount ), false );
			Info_SetValueForKey( config, "mr", va( "%d", entry.maxRanks ), false );

			if ( strlen( config ) >= BIG_INFO_STRING )
			{
				Sys::Error( "overload catalog configstring exceeded BIG_INFO_STRING (%zu >= %d)",
				            strlen( config ), BIG_INFO_STRING );
			}
		}

		trap_SetConfigstring( CS_OVERLOAD_CATALOG + i, config );
	}
}

static void NotifyLegacyStageSensors( team_t team, int oldCompletedPurchases, int newCompletedPurchases )
{
	auto stageForCompletedPurchases = []( int completedPurchases ) {
		if ( completedPurchases >= OVERLOAD_STAGE3_COUNT )
		{
			return 3;
		}

		if ( completedPurchases >= OVERLOAD_STAGE2_COUNT )
		{
			return 2;
		}

		return 1;
	};

	int oldStage = stageForCompletedPurchases( oldCompletedPurchases );
	int newStage = stageForCompletedPurchases( newCompletedPurchases );

	for ( int stage = 2; stage <= 3; ++stage )
	{
		bool wasPast = oldStage >= stage;
		bool isPast  = newStage >= stage;

		if ( wasPast == isPast )
		{
			continue;
		}

		if ( isPast )
		{
			G_notify_sensor_stage( team, stage - 2, stage - 1 );
		}
		else
		{
			G_notify_sensor_stage( team, stage - 1, stage - 2 );
		}
	}
}

static void SyncOverloadProgress( team_t team )
{
	int progress = G_OverloadProgressValue( team );
	level.team[ team ].overloadProgress = progress;

	for ( int playerNum = 0; playerNum < level.maxclients; ++playerNum )
	{
		gclient_t *client = level.clients + playerNum;
		if ( client->pers.connected != CON_CONNECTED || client->pers.team != team )
		{
			continue;
		}

		client->ps.persistant[ PERS_OVERLOAD ] = static_cast<short>( progress * 10 );
	}

	G_UpdateUnlockables();
}

static std::string UnlockableDisplayName( unlockableType_t type, int itemNum )
{
	switch ( type )
	{
		case UNLT_WEAPON:    return BG_Weapon( itemNum )->humanName;
		case UNLT_UPGRADE:   return BG_Upgrade( itemNum )->humanName;
		case UNLT_BUILDABLE: return BG_Buildable( itemNum )->humanName;
		case UNLT_CLASS:     return BG_ClassModelConfig( itemNum )->humanName;
		case UNLT_NUM_UNLOCKABLETYPES: break;
	}

	Sys::Error( "UnlockableDisplayName: unknown unlockable type" );
}

static const char* OverloadThingName( unlockableType_t type, int itemNum )
{
	switch ( type )
	{
		case UNLT_WEAPON:
			return BG_Weapon( itemNum )->name;

		case UNLT_UPGRADE:
			return BG_Upgrade( itemNum )->name;

		case UNLT_BUILDABLE:
			return BG_Buildable( itemNum )->name;

		case UNLT_CLASS:
			return BG_Class( itemNum )->name;

		case UNLT_NUM_UNLOCKABLETYPES:
			break;
	}

	Sys::Error( "OverloadThingName: invalid unlockable type" );
}

static void CaptureGameplayEffectBaseline( overloadEffect_t& effect )
{
	const gameplayVarInfo_t* info = BG_GameplayVar( effect.gameplayIndex );
	if ( !info )
	{
		Sys::Error( "invalid gameplay effect index %d", effect.gameplayIndex );
	}

	if ( info->type == GAMEPLAY_INTEGER )
	{
		effect.valueType = effectValueType_t::INTEGER;
		effect.baseline = *static_cast<int*>( info->storage );
	}
	else
	{
		effect.valueType = effectValueType_t::FLOAT;
		effect.baseline = *static_cast<float*>( info->storage );
	}
}

static void CaptureAttributeEffectBaseline( overloadEffect_t& effect )
{
	bgAttributeValue_t value{};
	std::string error;

	if ( !BG_GetAttributeValue( effect.attributeFamily, effect.attributeObject, effect.attributeField, &value, &error ) )
	{
		Sys::Error( "failed to capture attribute baseline: %s", error.c_str() );
	}

	const bgAttributeFieldInfo_t* field = BG_AttributeField( effect.attributeFamily, effect.attributeField );
	if ( !field )
	{
		Sys::Error( "invalid attribute field %d", effect.attributeField );
	}

	if ( field->type == BG_ATTR_INTEGER )
	{
		effect.valueType = effectValueType_t::INTEGER;
		effect.baseline = value.integer;
	}
	else if ( field->type == BG_ATTR_FLOAT )
	{
		effect.valueType = effectValueType_t::FLOAT;
		effect.baseline = value.number;
	}
	else
	{
		Sys::Error( "bool attributes are unsupported for overload effects" );
	}
}

static overloadEffect_t GameplayEffect( const char* gameplayVarName, double step, double minValue = -std::numeric_limits<double>::infinity(), double maxValue = std::numeric_limits<double>::infinity() )
{
	int gameplayIndex = BG_FindGameplayVarByName( gameplayVarName );
	if ( gameplayIndex < 0 )
	{
		Sys::Error( "unknown gameplay variable %s", gameplayVarName );
	}

	overloadEffect_t effect{};
	effect.target = effectTarget_t::GAMEPLAY;
	effect.gameplayIndex = gameplayIndex;
	effect.attributeFamily = BG_NUM_ATTRIBUTE_FAMILIES;
	effect.attributeObject = -1;
	effect.attributeField = -1;
	effect.step = step;
	effect.minValue = minValue;
	effect.maxValue = maxValue;
	CaptureGameplayEffectBaseline( effect );
	return effect;
}

static overloadEffect_t AttributeEffect( bgAttributeFamily_t family, const char* objectName, const char* fieldName, double step,
                                         double minValue = -std::numeric_limits<double>::infinity(),
                                         double maxValue = std::numeric_limits<double>::infinity() )
{
	int objectIndex = BG_FindAttributeObject( family, objectName );
	int fieldIndex = BG_FindAttributeField( family, fieldName );
	if ( objectIndex < 0 || fieldIndex < 0 )
	{
		Sys::Error( "unknown attribute target %s.%s", objectName, fieldName );
	}

	overloadEffect_t effect{};
	effect.target = effectTarget_t::ATTRIBUTE;
	effect.gameplayIndex = -1;
	effect.attributeFamily = family;
	effect.attributeObject = objectIndex;
	effect.attributeField = fieldIndex;
	effect.step = step;
	effect.minValue = minValue;
	effect.maxValue = maxValue;
	CaptureAttributeEffectBaseline( effect );
	return effect;
}

static overloadEffect_t PercentAttributeEffect( bgAttributeFamily_t family, const char* objectName, const char* fieldName, double fraction,
                                                double minValue = -std::numeric_limits<double>::infinity(),
                                                double maxValue = std::numeric_limits<double>::infinity() )
{
	overloadEffect_t effect = AttributeEffect( family, objectName, fieldName, 0.0, minValue, maxValue );
	effect.step = effect.baseline * fraction;
	return effect;
}

static int DefaultUpgradeBaseCost( int stage )
{
	if ( stage >= OVERLOAD_STAGE3_COUNT )
	{
		return OVERLOAD_UPGRADE_BASE_COST_STAGE3;
	}

	if ( stage >= OVERLOAD_STAGE2_COUNT )
	{
		return OVERLOAD_UPGRADE_BASE_COST_STAGE2;
	}

	return OVERLOAD_UPGRADE_BASE_COST_STAGE1;
}

static int DefaultUpgradeStepCost( int stage )
{
	if ( stage >= OVERLOAD_STAGE3_COUNT )
	{
		return OVERLOAD_UPGRADE_STEP_COST_STAGE3;
	}

	if ( stage >= OVERLOAD_STAGE2_COUNT )
	{
		return OVERLOAD_UPGRADE_STEP_COST_STAGE2;
	}

	return OVERLOAD_UPGRADE_STEP_COST_STAGE1;
}

static int UnlockCost( unlockableType_t unlockableType, int itemNum )
{
	int authoredUnlockValue = 0;

	switch ( unlockableType )
	{
		case UNLT_WEAPON: authoredUnlockValue = BG_Weapon( itemNum )->unlockThreshold; break;
		case UNLT_UPGRADE: authoredUnlockValue = BG_Upgrade( itemNum )->unlockThreshold; break;
		case UNLT_BUILDABLE: authoredUnlockValue = BG_Buildable( itemNum )->unlockThreshold; break;
		case UNLT_CLASS: authoredUnlockValue = BG_Class( itemNum )->unlockThreshold; break;
		default: authoredUnlockValue = 0; break;
	}

	if ( authoredUnlockValue <= 0 )
	{
		return 0;
	}

	return std::max( 0, static_cast<int>( std::lround(
		authoredUnlockValue * g_overloadUnlockCostSlope.Get() + g_overloadUnlockCostOffset.Get() ) ) );
}

static void AddUpgrade( team_t team, int baseCost, int costStep, int maxRanks,
                        const char* thing, const char* thingLabel, const char* stat, const char* statLabel,
                        const char* displayName, const char* uiDescription,
                        std::initializer_list<overloadEffect_t> effects )
{
	overloadPurchaseDef_t entry{};
	entry.kind = overloadPurchaseKind_t::UPGRADE;
	entry.team = team;
	entry.thing = thing;
	entry.thingLabel = thingLabel;
	entry.groupLabel = OverloadGroupForThing( team, thing );
	entry.sortIndex = OverloadSortIndexForThing( team, thing );
	entry.stat = stat;
	entry.statLabel = statLabel;
	entry.displayName = displayName;
	entry.uiDescription = uiDescription;
	entry.requiredCompletedCount = 0;
	entry.baseCost = baseCost;
	entry.costStep = costStep;
	entry.bundleAmount = 0;
	entry.maxRanks = maxRanks;
	entry.unlockFamily = BG_NUM_ATTRIBUTE_FAMILIES;
	entry.unlockObject = -1;
	entry.unlockField = -1;
	entry.effects.assign( effects.begin(), effects.end() );
	overloadPurchases.push_back( std::move( entry ) );
}

static void AddUnlockWithCost( team_t team, unlockableType_t unlockableType, int itemNum,
                               bgAttributeFamily_t family, int objectIndex, int unlockField, int baseCost,
                               const char* thing, const char* thingLabel, const char* displayName, const char* uiDescription )
{
	overloadPurchaseDef_t entry{};
	entry.kind = overloadPurchaseKind_t::UNLOCK;
	entry.team = team;
	entry.thing = thing;
	entry.thingLabel = thingLabel;
	entry.groupLabel = OverloadGroupForThing( team, thing );
	entry.sortIndex = OverloadSortIndexForThing( team, thing );
	entry.displayName = displayName;
	entry.uiDescription = uiDescription;
	entry.requiredCompletedCount = 0;
	entry.baseCost = baseCost;
	entry.costStep = 0;
	entry.bundleAmount = 0;
	entry.maxRanks = 1;
	entry.unlockFamily = family;
	entry.unlockObject = objectIndex;
	entry.unlockField = unlockField;
	overloadPurchases.push_back( std::move( entry ) );
	SetUnlockPurchaseIndex( team, unlockableType, itemNum, static_cast<int>( overloadPurchases.size() ) - 1 );
}

static void AddUnlock( team_t team, unlockableType_t unlockableType, int itemNum,
                       bgAttributeFamily_t family, int objectIndex, int unlockField,
                       const char* thing, const char* thingLabel, const char* displayName, const char* uiDescription )
{
	AddUnlockWithCost( team, unlockableType, itemNum, family, objectIndex, unlockField,
	                   UnlockCost( unlockableType, itemNum ),
	                   thing, thingLabel, displayName, uiDescription );
}

static void AddWeaponUnlock( team_t team, weapon_t weapon )
{
	const int unlockField = FindUnlockThresholdField( BG_ATTR_WEAPON );
	const int objectIndex = BG_FindAttributeObject( BG_ATTR_WEAPON, BG_Weapon( weapon )->name );
	if ( objectIndex < 0 || unlockField < 0 )
	{
		Sys::Error( "AddWeaponUnlock: could not resolve attribute data for %s", BG_Weapon( weapon )->name );
	}

	std::string displayName = UnlockableDisplayName( UNLT_WEAPON, weapon );
	std::string uiDescription = UnlockableDescription( UNLT_WEAPON, weapon );
	AddUnlock( team, UNLT_WEAPON, weapon, BG_ATTR_WEAPON, objectIndex, unlockField,
	           OverloadThingName( UNLT_WEAPON, weapon ), displayName.c_str(), displayName.c_str(), uiDescription.c_str() );
}

static void AddUpgradeUnlock( team_t team, upgrade_t upgrade )
{
	const int unlockField = FindUnlockThresholdField( BG_ATTR_UPGRADE );
	const int objectIndex = BG_FindAttributeObject( BG_ATTR_UPGRADE, BG_Upgrade( upgrade )->name );
	if ( objectIndex < 0 || unlockField < 0 )
	{
		Sys::Error( "AddUpgradeUnlock: could not resolve attribute data for %s", BG_Upgrade( upgrade )->name );
	}

	std::string displayName = UnlockableDisplayName( UNLT_UPGRADE, upgrade );
	std::string uiDescription = UnlockableDescription( UNLT_UPGRADE, upgrade );
	AddUnlock( team, UNLT_UPGRADE, upgrade, BG_ATTR_UPGRADE, objectIndex, unlockField,
	           OverloadThingName( UNLT_UPGRADE, upgrade ), displayName.c_str(), displayName.c_str(), uiDescription.c_str() );
}

static void AddBuildableUnlock( team_t team, buildable_t buildable )
{
	const int unlockField = FindUnlockThresholdField( BG_ATTR_BUILDABLE );
	const int objectIndex = BG_FindAttributeObject( BG_ATTR_BUILDABLE, BG_Buildable( buildable )->name );
	if ( objectIndex < 0 || unlockField < 0 )
	{
		Sys::Error( "AddBuildableUnlock: could not resolve attribute data for %s", BG_Buildable( buildable )->name );
	}

	std::string displayName = UnlockableDisplayName( UNLT_BUILDABLE, buildable );
	std::string uiDescription = UnlockableDescription( UNLT_BUILDABLE, buildable );
	AddUnlock( team, UNLT_BUILDABLE, buildable, BG_ATTR_BUILDABLE, objectIndex, unlockField,
	           OverloadThingName( UNLT_BUILDABLE, buildable ), displayName.c_str(), displayName.c_str(), uiDescription.c_str() );
}

static void AddClassUnlock( team_t team, class_t classNum )
{
	const int unlockField = FindUnlockThresholdField( BG_ATTR_CLASS );
	const int objectIndex = BG_FindAttributeObject( BG_ATTR_CLASS, BG_Class( classNum )->name );
	if ( objectIndex < 0 || unlockField < 0 )
	{
		Sys::Error( "AddClassUnlock: could not resolve attribute data for %s", BG_Class( classNum )->name );
	}

	std::string displayName = UnlockableDisplayName( UNLT_CLASS, classNum );
	std::string uiDescription = UnlockableDescription( UNLT_CLASS, classNum );
	AddUnlock( team, UNLT_CLASS, classNum, BG_ATTR_CLASS, objectIndex, unlockField,
	           OverloadThingName( UNLT_CLASS, classNum ), displayName.c_str(), displayName.c_str(), uiDescription.c_str() );
}

static bool UpgradePrerequisiteMetForValidation( const overloadPurchaseDef_t& entry, team_t team, const std::vector<bool>& ownedUnlocks )
{
	for ( size_t i = 0; i < overloadPurchases.size(); ++i )
	{
		const overloadPurchaseDef_t& candidate = overloadPurchases[ i ];
		if ( candidate.kind != overloadPurchaseKind_t::UNLOCK || candidate.team != team )
		{
			continue;
		}

		if ( Q_stricmp( candidate.thing.c_str(), entry.thing.c_str() ) )
		{
			continue;
		}

		return ownedUnlocks[ i ];
	}

	return true;
}

static void ValidateOverloadGraph()
{
	for ( team_t team = TEAM_NONE; ( team = G_IterateTeams( team ) ); )
	{
		std::vector<bool> ownedUnlocks( overloadPurchases.size(), false );
		int reachableCompletedPurchases = 0;
		bool infiniteProgression = false;
		bool changed = true;

		while ( changed && !infiniteProgression )
		{
			changed = false;

			for ( size_t i = 0; i < overloadPurchases.size(); ++i )
			{
				const overloadPurchaseDef_t& entry = overloadPurchases[ i ];
				if ( entry.kind != overloadPurchaseKind_t::UNLOCK || entry.team != team || ownedUnlocks[ i ] )
				{
					continue;
				}

				if ( entry.requiredCompletedCount > reachableCompletedPurchases )
				{
					continue;
				}

				ownedUnlocks[ i ] = true;
				++reachableCompletedPurchases;
				changed = true;
			}

			for ( const overloadPurchaseDef_t& entry : overloadPurchases )
			{
				if ( entry.kind != overloadPurchaseKind_t::UPGRADE || entry.team != team )
				{
					continue;
				}

				if ( entry.requiredCompletedCount > reachableCompletedPurchases )
				{
					continue;
				}

				if ( UpgradePrerequisiteMetForValidation( entry, team, ownedUnlocks ) )
				{
					infiniteProgression = true;
					break;
				}
			}
		}

		if ( infiniteProgression )
		{
			continue;
		}

		for ( const overloadPurchaseDef_t& entry : overloadPurchases )
		{
			if ( entry.team != team || entry.kind == overloadPurchaseKind_t::BP_BUNDLE )
			{
				continue;
			}

			if ( entry.requiredCompletedCount > reachableCompletedPurchases )
			{
				Sys::Error( "Overload graph broken for team %d: %s '%s' requires %d completed purchases, but only %d are reachable before progression stalls",
				            team, PurchaseKindToken( entry.kind ), entry.thing.c_str(),
				            entry.requiredCompletedCount, reachableCompletedPurchases );
			}
		}
	}
}

static int FindUnlockThresholdField( bgAttributeFamily_t family )
{
	return BG_FindAttributeField( family, "unlock_threshold" );
}

static void BuildOverloadCatalog()
{
	if ( overloadCatalogReady )
	{
		return;
	}

	overloadPurchases.clear();
	ResetUnlockPurchaseIndexMaps();

	overloadPurchaseDef_t bpBundle{};
	bpBundle.kind = overloadPurchaseKind_t::BP_BUNDLE;
	bpBundle.team = TEAM_NONE;
	bpBundle.thing = "bp";
	bpBundle.thingLabel = "Build Points";
	bpBundle.groupLabel = "Build Points";
	bpBundle.sortIndex = 0;
	bpBundle.displayName = "BP +50";
	bpBundle.uiDescription = "Add 50 team BP. Each bundle costs more than the last.";
	bpBundle.requiredCompletedCount = 0;
	bpBundle.baseCost = OVERLOAD_BP_BUNDLE_COST;
	bpBundle.costStep = OVERLOAD_BP_BUNDLE_COST_STEP;
	bpBundle.bundleAmount = OVERLOAD_BP_BUNDLE_AMOUNT;
	bpBundle.maxRanks = std::numeric_limits<int>::max();
	bpBundle.unlockFamily = BG_NUM_ATTRIBUTE_FAMILIES;
	bpBundle.unlockObject = -1;
	bpBundle.unlockField = -1;
	overloadPurchases.push_back( bpBundle );

	AddWeaponUnlock( TEAM_HUMANS, WP_PAIN_SAW );
	AddWeaponUnlock( TEAM_HUMANS, WP_SHOTGUN );
	AddWeaponUnlock( TEAM_HUMANS, WP_MASS_DRIVER );
	AddWeaponUnlock( TEAM_HUMANS, WP_CHAINGUN );
	AddWeaponUnlock( TEAM_HUMANS, WP_FLAMER );
	AddWeaponUnlock( TEAM_HUMANS, WP_PULSE_RIFLE );
	AddWeaponUnlock( TEAM_HUMANS, WP_LUCIFER_CANNON );
	AddUpgradeUnlock( TEAM_HUMANS, UP_MEDIUMARMOUR );
	AddUpgradeUnlock( TEAM_HUMANS, UP_RADAR );
	AddUpgradeUnlock( TEAM_HUMANS, UP_JETPACK );
	AddUpgradeUnlock( TEAM_HUMANS, UP_BIOKIT );
	AddUpgradeUnlock( TEAM_HUMANS, UP_BATTLESUIT );
	AddUpgradeUnlock( TEAM_HUMANS, UP_GRENADE );
	AddUpgradeUnlock( TEAM_HUMANS, UP_FIREBOMB );
	AddBuildableUnlock( TEAM_HUMANS, BA_H_ROCKETPOD );

	// Alien structures: acid tube, trapper, spiker, booster, hive.
	AddUpgrade( TEAM_ALIENS, DefaultUpgradeBaseCost( 0 ), DefaultUpgradeStepCost( 0 ), OVERLOAD_UNCAPPED_RANKS, "acid_tube", "Acid Tube", "damage", "damage", "Acid Tube Damage", "Increase acid tube damage per second.",
	            { GameplayEffect( "ACIDTUBE_DAMAGE", 2.0, 1.0 ) } );
	AddBuildableUnlock( TEAM_ALIENS, BA_A_TRAPPER );
	AddBuildableUnlock( TEAM_ALIENS, BA_A_SPIKER );
	AddBuildableUnlock( TEAM_ALIENS, BA_A_BOOSTER );
	AddBuildableUnlock( TEAM_ALIENS, BA_A_HIVE );

	// Alien lifeforms: advanced granger, dretch, mantis, marauder, advanced marauder, dragoon, advanced dragoon, tyrant.
	AddClassUnlock( TEAM_ALIENS, PCL_ALIEN_BUILDER0_UPG );
	AddUpgrade( TEAM_ALIENS, DefaultUpgradeBaseCost( 0 ), DefaultUpgradeStepCost( OVERLOAD_STAGE3_COUNT ), OVERLOAD_UNCAPPED_RANKS, "level0", "Dretch", "damage", "damage", "Dretch Damage", "Increase dretch bite damage.",
	            { GameplayEffect( "LEVEL0_BITE_DMG", 5.0 ) } );
	AddUpgrade( TEAM_ALIENS, DefaultUpgradeBaseCost( 0 ), DefaultUpgradeStepCost( 0 ), OVERLOAD_UNCAPPED_RANKS, "level1", "Mantis", "damage", "damage", "Mantis Damage", "Increase mantis claw damage.",
	            { GameplayEffect( "LEVEL1_CLAW_DMG", 6.0 ) } );
	AddClassUnlock( TEAM_ALIENS, PCL_ALIEN_LEVEL2 );
	AddClassUnlock( TEAM_ALIENS, PCL_ALIEN_LEVEL2_UPG );
	AddClassUnlock( TEAM_ALIENS, PCL_ALIEN_LEVEL3 );
	AddClassUnlock( TEAM_ALIENS, PCL_ALIEN_LEVEL3_UPG );
	AddClassUnlock( TEAM_ALIENS, PCL_ALIEN_LEVEL4 );

	// Human weapon upgrades.
	// Covered weapons: blaster, rifle, psaw, shotgun, lasgun, mdriver, chaingun, flamer, prifle, lcannon.
	// No weapon overload upgrades should live outside this block.

	// Human weapons: stage 1 / always available.
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( OVERLOAD_STAGE3_COUNT ), DefaultUpgradeStepCost( 0 ), OVERLOAD_UNCAPPED_RANKS, "rifle", "Rifle", "damage", "damage", "Rifle Damage", "Increase rifle damage.",
	            { GameplayEffect( "RIFLE_DMG", 1.0 ) } );
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( 0 ), DefaultUpgradeStepCost( 0 ), OVERLOAD_UNCAPPED_RANKS, "rifle", "Rifle", "ammo", "ammo", "Rifle Ammo", "Increase rifle ammo reserve.",
	            { AttributeEffect( BG_ATTR_WEAPON, "rifle", "ammo", 5.0, 1.0 ) } );
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( 0 ), DefaultUpgradeStepCost( 0 ), OVERLOAD_UNCAPPED_RANKS, "psaw", "Pain Saw", "damage", "damage", "Pain Saw Damage", "Increase pain saw damage per hit.",
	            { GameplayEffect( "PAINSAW_DAMAGE", 2.0, 1.0 ) } );
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( 0 ), DefaultUpgradeStepCost( 0 ), OVERLOAD_UNCAPPED_RANKS, "shotgun", "Shotgun", "damage", "damage", "Shotgun Damage", "Increase shotgun pellet damage.",
	            { GameplayEffect( "SHOTGUN_DMG", 1.0 ) } );
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( 0 ), DefaultUpgradeStepCost( 0 ), OVERLOAD_UNCAPPED_RANKS, "shotgun", "Shotgun", "ammo", "ammo", "Shotgun Ammo", "Increase shotgun ammo reserve.",
	            { AttributeEffect( BG_ATTR_WEAPON, "shotgun", "ammo", 1.0, 1.0 ) } );

	// Human weapons: stage 2 unlocks.
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE2_COUNT ), OVERLOAD_UNCAPPED_RANKS, "lgun", "Lasgun", "damage", "damage", "Lasgun Damage", "Increase lasgun damage.",
	            { GameplayEffect( "LASGUN_DAMAGE", 2.0 ) } );
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE2_COUNT ), OVERLOAD_UNCAPPED_RANKS, "lgun", "Lasgun", "ammo", "ammo", "Lasgun Ammo", "Increase lasgun ammo reserve.",
	            { AttributeEffect( BG_ATTR_WEAPON, "lgun", "ammo", 10.0, 1.0 ) } );
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE2_COUNT ), OVERLOAD_UNCAPPED_RANKS, "mdriver", "Mass Driver", "damage", "damage", "Mass Driver Damage", "Increase mass driver damage.",
	            { GameplayEffect( "MDRIVER_DMG", 10.0 ) } );
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE2_COUNT ), OVERLOAD_UNCAPPED_RANKS, "mdriver", "Mass Driver", "ammo", "ammo", "Mass Driver Ammo", "Increase mass driver ammo reserve.",
	            { AttributeEffect( BG_ATTR_WEAPON, "mdriver", "ammo", 1.0, 1.0 ) } );
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE2_COUNT ), OVERLOAD_UNCAPPED_RANKS, "chaingun", "Chaingun", "damage", "damage", "Chaingun Damage", "Increase chaingun damage.",
	            { GameplayEffect( "CHAINGUN_DMG", 1.0 ) } );
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE2_COUNT ), OVERLOAD_UNCAPPED_RANKS, "chaingun", "Chaingun", "ammo", "ammo", "Chaingun Ammo", "Increase chaingun ammo reserve.",
	            { AttributeEffect( BG_ATTR_WEAPON, "chaingun", "ammo", 20.0, 1.0 ) } );
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE2_COUNT ), OVERLOAD_UNCAPPED_RANKS, "flamer", "Flamethrower", "ammo", "ammo", "Flamer Ammo", "Increase flamer ammo reserve.",
	            { AttributeEffect( BG_ATTR_WEAPON, "flamer", "ammo", 25.0, 1.0 ) } );
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE2_COUNT ), OVERLOAD_UNCAPPED_RANKS, "prifle", "Pulse Rifle", "damage", "damage", "Pulse Rifle Damage", "Increase pulse rifle projectile damage.",
	            { AttributeEffect( BG_ATTR_MISSILE, "prifle", "damage", 2.0, 1.0 ) } );
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE2_COUNT ), OVERLOAD_UNCAPPED_RANKS, "prifle", "Pulse Rifle", "ammo", "ammo", "Pulse Rifle Ammo", "Increase pulse rifle ammo reserve.",
	            { AttributeEffect( BG_ATTR_WEAPON, "prifle", "ammo", 5.0, 1.0 ) } );

	// Human weapons: stage 3 unlocks.
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( OVERLOAD_STAGE3_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE3_COUNT ), OVERLOAD_UNCAPPED_RANKS, "lcannon", "Lucifer Cannon", "damage", "damage", "Lucifer Cannon Damage", "Increase lucifer cannon damage.",
	            { GameplayEffect( "LCANNON_DAMAGE", 15.0 ) } );
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( OVERLOAD_STAGE3_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE3_COUNT ), OVERLOAD_UNCAPPED_RANKS, "lcannon", "Lucifer Cannon", "ammo", "ammo", "Lucifer Cannon Ammo", "Increase lucifer cannon ammo reserve.",
	            { AttributeEffect( BG_ATTR_WEAPON, "lcannon", "ammo", 10.0, 1.0 ) } );

	// Human equipment upgrades.
	// Covered upgrades: jetpack, human stamina.
	// Intentionally missing for now: medkit, lightarmour, medarmour, bsuit, radar, grenade, firebomb.
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE3_COUNT ), OVERLOAD_UNCAPPED_RANKS, "armor", "Armour", "integrity", "armor integrity", "Armor Integrity", "Increase max health for all human armour classes.",
	            { PercentAttributeEffect( BG_ATTR_CLASS, "human_light", "health", 0.10, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "human_medium", "health", 0.10, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "human_bsuit", "health", 0.10, 1.0 ) } );
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE2_COUNT ), OVERLOAD_UNCAPPED_RANKS, "jetpack", "Jetpack", "fuel", "fuel", "Jetpack Fuel", "Increase jetpack fuel capacity.",
	            { GameplayEffect( "JETPACK_FUEL_MAX", 2500.0, 1.0, std::numeric_limits< unsigned int >::max() ),
	              GameplayEffect( "JETPACK_FUEL_RESTORE", 2500.0 * JETPACK_FUEL_RESTORE / JETPACK_FUEL_MAX, 1.0 ) } );
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE2_COUNT ), OVERLOAD_UNCAPPED_RANKS, "humans", "Humans", "stamina", "stamina", "Human Stamina", "Increase maximum human stamina.",
	            { GameplayEffect( "STAMINA_MAX", 2500.0, 1.0, std::numeric_limits<unsigned short>::max() ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "human_naked", "staminaJogRestore", 2500.0 / STAMINA_MAX, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "human_naked", "staminaWalkRestore", 2500.0 / STAMINA_MAX, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "human_naked", "staminaStopRestore", 2500.0 / STAMINA_MAX, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "human_light", "staminaJogRestore", 2500.0 / STAMINA_MAX, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "human_light", "staminaWalkRestore", 2500.0 / STAMINA_MAX, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "human_light", "staminaStopRestore", 2500.0 / STAMINA_MAX, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "human_medium", "staminaJogRestore", 2500.0 / STAMINA_MAX, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "human_medium", "staminaWalkRestore", 2500.0 / STAMINA_MAX, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "human_medium", "staminaStopRestore", 2500.0 / STAMINA_MAX, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "human_bsuit", "staminaJogRestore", 2500.0 / STAMINA_MAX, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "human_bsuit", "staminaWalkRestore", 2500.0 / STAMINA_MAX, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "human_bsuit", "staminaStopRestore", 2500.0 / STAMINA_MAX, 1.0 ) } );

	// Human buildable weapon upgrades.
	// Covered buildables: medistat, mgturret, rocketpod.
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( 0 ), DefaultUpgradeStepCost( 0 ), OVERLOAD_UNCAPPED_RANKS, "medistat", "Medistation", "healing_rate", "healing rate", "Medistation Healing Rate", "Increase medistation health restoration.",
	            { GameplayEffect( "MEDISTAT_HEAL_RATE", 0.0075, 0.0 ),
				  GameplayEffect( "STAMINA_MEDISTAT_RESTORE", 33, 0 ) } );
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( 0 ), DefaultUpgradeStepCost( 0 ), OVERLOAD_UNCAPPED_RANKS, "mgturret", "Machinegun Turret", "damage", "damage", "Machinegun Turret Damage", "Increase machinegun turret damage.",
	            { GameplayEffect( "MGTURRET_MIN_DAMAGE", 1.0, 0.0 ),
	              GameplayEffect( "MGTURRET_MAX_DAMAGE", 2.0, 1.0 ) } );
	AddUpgrade( TEAM_HUMANS, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE2_COUNT ), OVERLOAD_UNCAPPED_RANKS, "rocketpod", "Rocket Pod", "damage", "damage", "Rocketpod Damage", "Increase rocketpod rocket damage.",
	            { AttributeEffect( BG_ATTR_MISSILE, "rocket", "damage", 12.0, 1.0 ),
	              AttributeEffect( BG_ATTR_MISSILE, "rocket", "splash_damage", 8.0, 0.0 ) } );

	// Alien buildable upgrades.
	// Covered buildables: acid_tube, trapper, hive.
	AddUpgrade( TEAM_ALIENS, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE2_COUNT ), OVERLOAD_UNCAPPED_RANKS, "trapper", "Trapper", "health", "max health", "Trapper Health", "Increase trapper durability.",
	            { AttributeEffect( BG_ATTR_BUILDABLE, "trapper", "health", 15.0, 1.0 ) } );
	AddUpgrade( TEAM_ALIENS, DefaultUpgradeBaseCost( OVERLOAD_STAGE3_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE3_COUNT ), OVERLOAD_UNCAPPED_RANKS, "hive", "Hive", "damage", "damage", "Hive Damage", "Increase hive missile damage.",
	            { AttributeEffect( BG_ATTR_MISSILE, "hive", "damage", 8.0, 1.0 ) } );

	// Alien class upgrades.
	// Covered classes: dretch, level1 scout, marauder, advanced marauder, dragoon, advanced dragoon, tyrant.
	// Intentionally missing for now: granger, advanced granger.
	AddUpgrade( TEAM_ALIENS, DefaultUpgradeBaseCost( 0 ), DefaultUpgradeStepCost( OVERLOAD_STAGE2_COUNT ), OVERLOAD_UNCAPPED_RANKS, "aliens", "Alien Lifeforms", "vitality", "vitality", "Alien Vitality", "Increase max health for all alien classes.",
	            { PercentAttributeEffect( BG_ATTR_CLASS, "builder", "health", 0.1, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "builderupg", "health", 0.1, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "level0", "health", 0.1, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "level1", "health", 0.1, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "level2", "health", 0.1, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "level2upg", "health", 0.1, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "level3", "health", 0.1, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "level3upg", "health", 0.1, 1.0 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "level4", "health", 0.1, 1.0 ) } );
	AddUpgrade( TEAM_ALIENS, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE2_COUNT ), OVERLOAD_UNCAPPED_RANKS, "aliens", "Alien Lifeforms", "heal_rate", "heal rate", "Alien Heal Rate", "Increase passive healing rate for all alien classes.",
	            { PercentAttributeEffect( BG_ATTR_CLASS, "builder", "regen_rate", 0.1, 0.001 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "builderupg", "regen_rate", 0.1, 0.001 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "level0", "regen_rate", 0.1, 0.001 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "level1", "regen_rate", 0.1, 0.001 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "level2", "regen_rate", 0.1, 0.001 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "level2upg", "regen_rate", 0.1, 0.001 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "level3", "regen_rate", 0.1, 0.001 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "level3upg", "regen_rate", 0.1, 0.001 ),
	              PercentAttributeEffect( BG_ATTR_CLASS, "level4", "regen_rate", 0.1, 0.001 ) } );
	AddUpgrade( TEAM_ALIENS, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE2_COUNT ), OVERLOAD_UNCAPPED_RANKS, "level2upg", "Advanced Marauder", "damage", "damage", "Advanced Marauder Zap Damage", "Increase advanced marauder zap damage.",
	            { GameplayEffect( "LEVEL2_AREAZAP_DMG", 4.0 ) } );
	AddUpgrade( TEAM_ALIENS, DefaultUpgradeBaseCost( 0 ), DefaultUpgradeStepCost( 0 ), OVERLOAD_UNCAPPED_RANKS, "level2", "Marauder", "damage", "damage", "Marauder Damage", "Increase marauder claw damage.",
	            { GameplayEffect( "LEVEL2_CLAW_DMG", 4.0 ) } );
	AddUpgrade( TEAM_ALIENS, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE2_COUNT ), OVERLOAD_UNCAPPED_RANKS, "level3", "Dragoon", "damage", "damage", "Dragoon Damage", "Increase dragoon claw damage.",
	            { GameplayEffect( "LEVEL3_CLAW_DMG", 5.0 ) } );
	AddUpgrade( TEAM_ALIENS, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE2_COUNT ), OVERLOAD_UNCAPPED_RANKS, "level3", "Dragoon", "pounce_damage", "pounce damage", "Dragoon Pounce Damage", "Increase dragoon pounce damage.",
	            { GameplayEffect( "LEVEL3_POUNCE_DMG", 10.0 ) } );
	AddUpgrade( TEAM_ALIENS, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE3_COUNT ), OVERLOAD_UNCAPPED_RANKS, "level3upg", "Advanced Dragoon", "barb_refresh", "barb refresh", "Advanced Dragoon Barb Refresh", "Reduce advanced dragoon barb regeneration time.",
	            { GameplayEffect( "LEVEL3_BOUNCEBALL_REGEN", -500.0, 100.0 ),
	              GameplayEffect( "LEVEL3_BOUNCEBALL_REGEN_BOOSTER", -500.0, 100.0 ),
	              GameplayEffect( "LEVEL3_BOUNCEBALL_REGEN_CREEP", -500.0, 100.0 ) } );
	AddUpgrade( TEAM_ALIENS, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE3_COUNT ), OVERLOAD_UNCAPPED_RANKS, "level3upg", "Advanced Dragoon", "barb_damage", "barb damage", "Advanced Dragoon Barb Damage", "Increase advanced dragoon barb impact and splash damage.",
	            { AttributeEffect( BG_ATTR_MISSILE, "bounceball", "damage", 10.0, 1.0 ),
	              AttributeEffect( BG_ATTR_MISSILE, "bounceball", "splash_damage", 6.0, 0.0 ) } );
	AddUpgrade( TEAM_ALIENS, DefaultUpgradeBaseCost( OVERLOAD_STAGE3_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE3_COUNT ), OVERLOAD_UNCAPPED_RANKS, "level4", "Tyrant", "damage", "damage", "Tyrant Damage", "Increase tyrant claw damage.",
	            { GameplayEffect( "LEVEL4_CLAW_DMG", 8.0 ) } );
	AddUpgrade( TEAM_ALIENS, DefaultUpgradeBaseCost( OVERLOAD_STAGE3_COUNT ), DefaultUpgradeStepCost( OVERLOAD_STAGE3_COUNT ), OVERLOAD_UNCAPPED_RANKS, "level4", "Tyrant", "trample_damage", "trample damage", "Tyrant Trample Damage", "Increase tyrant trample damage.",
	            { GameplayEffect( "LEVEL4_TRAMPLE_DMG", 10.0 ) } );

	if ( overloadPurchases.size() > MAX_OVERLOAD_PURCHASES )
	{
		Sys::Error( "Overload purchase catalog exceeds MAX_OVERLOAD_PURCHASES (%zu > %d)",
		            overloadPurchases.size(), MAX_OVERLOAD_PURCHASES );
	}

	ValidateOverloadGraph();

	overloadCatalogReady = true;
}

static bool EntryMatches( const overloadPurchaseDef_t& entry, const Cmd::Args& args )
{
	if ( entry.kind == overloadPurchaseKind_t::BP_BUNDLE )
	{
		return args.Argc() >= 2 && !Q_stricmp( args.Argv( 1 ).c_str(), entry.thing.c_str() );
	}

	if ( entry.kind == overloadPurchaseKind_t::UNLOCK )
	{
		return args.Argc() >= 3 &&
		       !Q_stricmp( args.Argv( 1 ).c_str(), "unlock" ) &&
		       !Q_stricmp( args.Argv( 2 ).c_str(), entry.thing.c_str() );
	}

	return args.Argc() >= 4 &&
	       !Q_stricmp( args.Argv( 1 ).c_str(), "upgrade" ) &&
	       !Q_stricmp( args.Argv( 2 ).c_str(), entry.thing.c_str() ) &&
	       !Q_stricmp( args.Argv( 3 ).c_str(), entry.stat.c_str() );
}

static const overloadPurchaseDef_t* FindPurchase( team_t team, const Cmd::Args& args, int* purchaseIndex = nullptr )
{
	for ( size_t i = 0; i < overloadPurchases.size(); ++i )
	{
		const overloadPurchaseDef_t& entry = overloadPurchases[ i ];
		if ( entry.team != TEAM_NONE && entry.team != team )
		{
			continue;
		}

		if ( EntryMatches( entry, args ) )
		{
			if ( purchaseIndex )
			{
				*purchaseIndex = i;
			}
			return &entry;
		}
	}

	return nullptr;
}

bool EntryIsAvailable( team_t team, const overloadPurchaseDef_t& entry )
{
	if ( entry.team != TEAM_NONE && entry.team != team )
	{
		return false;
	}

	const TeamEconomyState& economy = TeamEconomy( team );
	const size_t index = &entry - overloadPurchases.data();

	if ( entry.kind == overloadPurchaseKind_t::UNLOCK )
	{
		return !economy.ownedPurchases[ index ];
	}

	return economy.repeatCounts[ index ] < entry.maxRanks;
}

int RemainingSpendCapacity( const overloadPurchaseDef_t& entry, int entryIndex, team_t team )
{
	if ( entry.kind == overloadPurchaseKind_t::BP_BUNDLE )
	{
		return std::numeric_limits<int>::max();
	}

	const TeamEconomyState& economy = TeamEconomy( team );
	int currentRank = economy.repeatCounts[ entryIndex ];
	int invested = economy.investedCredits[ entryIndex ];

	if ( entry.kind == overloadPurchaseKind_t::UNLOCK )
	{
		return std::max( 0, OverloadNextCost( entry, entryIndex, team ) - invested );
	}

	if ( entry.maxRanks == OVERLOAD_UNCAPPED_RANKS )
	{
		return std::numeric_limits<int>::max();
	}

	int remaining = -invested;
	for ( int rank = currentRank; rank < entry.maxRanks; ++rank )
	{
		remaining += ScaleOverloadCost( team, entry.baseCost + rank * entry.costStep );
	}

	return std::max( 0, remaining );
}

static int RanksCompletedFromSpend( const overloadPurchaseDef_t& entry, int currentRank, int& investedCredits, team_t team )
{
	if ( entry.kind == overloadPurchaseKind_t::UNLOCK )
	{
		if ( investedCredits >= ScaleOverloadCost( team, entry.baseCost ) )
		{
			investedCredits = 0;
			return 1;
		}
		return 0;
	}

	if ( entry.kind == overloadPurchaseKind_t::BP_BUNDLE )
	{
		int completed = 0;
		while ( investedCredits >= ScaleOverloadCost( team, entry.baseCost + ( currentRank + completed ) * entry.costStep ) )
		{
			investedCredits -= ScaleOverloadCost( team, entry.baseCost + ( currentRank + completed ) * entry.costStep );
			++completed;
		}
		return completed;
	}

	int completed = 0;
	while ( currentRank + completed < entry.maxRanks )
	{
		int threshold = ScaleOverloadCost( team, entry.baseCost + ( currentRank + completed ) * entry.costStep );
		if ( investedCredits < threshold )
		{
			break;
		}

		investedCredits -= threshold;
		++completed;
	}

	return completed;
}

static double ClampEffectValue( const overloadEffect_t& effect, double value )
{
	return std::max( effect.minValue, std::min( effect.maxValue, value ) );
}

static bool SameEffectTarget( const overloadEffect_t& lhs, const overloadEffect_t& rhs )
{
	return lhs.target == rhs.target &&
	       lhs.valueType == rhs.valueType &&
	       lhs.gameplayIndex == rhs.gameplayIndex &&
	       lhs.attributeFamily == rhs.attributeFamily &&
	       lhs.attributeObject == rhs.attributeObject &&
	       lhs.attributeField == rhs.attributeField;
}

static double EffectiveEffectValue( const overloadEffect_t& effect, team_t team )
{
	double value = effect.baseline;
	const TeamEconomyState& economy = TeamEconomy( team );

	for ( size_t i = 0; i < overloadPurchases.size(); ++i )
	{
		const overloadPurchaseDef_t& entry = overloadPurchases[ i ];
		if ( entry.kind != overloadPurchaseKind_t::UPGRADE || entry.team != team )
		{
			continue;
		}

		for ( const overloadEffect_t& candidate : entry.effects )
		{
			if ( SameEffectTarget( effect, candidate ) )
			{
				value += candidate.step * economy.repeatCounts[ i ];
			}
		}
	}

	return ClampEffectValue( effect, value );
}

static void ApplyEffectValue( const overloadEffect_t& effect, team_t team, bool& gameplayDirty, bool& attributeDirty )
{
	double value = EffectiveEffectValue( effect, team );
	std::string error;

	if ( effect.target == effectTarget_t::GAMEPLAY )
	{
		if ( effect.valueType == effectValueType_t::INTEGER )
		{
			if ( !BG_SetGameplayInt( effect.gameplayIndex, static_cast<int>( lround( value ) ), true, &error ) )
			{
				Sys::Error( "failed to set gameplay override: %s", error.c_str() );
			}
		}
		else
		{
			if ( !BG_SetGameplayFloat( effect.gameplayIndex, static_cast<float>( value ), true, &error ) )
			{
				Sys::Error( "failed to set gameplay override: %s", error.c_str() );
			}
		}

		gameplayDirty = true;
		return;
	}

	if ( effect.valueType == effectValueType_t::INTEGER )
	{
		if ( !BG_SetAttributeInt( effect.attributeFamily, effect.attributeObject, effect.attributeField,
		                          static_cast<int>( lround( value ) ), true, &error ) )
		{
			Sys::Error( "failed to set attribute override: %s", error.c_str() );
		}
	}
	else
	{
		if ( !BG_SetAttributeFloat( effect.attributeFamily, effect.attributeObject, effect.attributeField,
		                            static_cast<float>( value ), true, &error ) )
		{
			Sys::Error( "failed to set attribute override: %s", error.c_str() );
		}
	}

	attributeDirty = true;
}

static void ApplyEntryState( const overloadPurchaseDef_t& entry, team_t team, bool& gameplayDirty, bool& attributeDirty )
{
	if ( entry.kind == overloadPurchaseKind_t::UNLOCK )
	{
		return;
	}

	if ( entry.kind == overloadPurchaseKind_t::BP_BUNDLE )
	{
		return;
	}

	for ( const overloadEffect_t& effect : entry.effects )
	{
		ApplyEffectValue( effect, team, gameplayDirty, attributeDirty );
	}
}

static int ApplyPurchaseToTeamState( const overloadPurchaseDef_t& entry, int entryIndex, team_t team,
                                     int completionsApplied, bool& gameplayDirty, bool& attributeDirty )
{
	TeamEconomyState& economy = TeamEconomy( team );
	int totalGranted = 0;

	if ( entry.kind == overloadPurchaseKind_t::BP_BUNDLE )
	{
		totalGranted = completionsApplied * entry.bundleAmount;
		if ( totalGranted > 0 )
		{
			economy.repeatCounts[ entryIndex ] += completionsApplied;
			economy.bpPurchased += totalGranted;
			level.team[ team ].totalBudget += totalGranted;
		}
	}
	else if ( entry.kind == overloadPurchaseKind_t::UNLOCK )
	{
		if ( completionsApplied > 0 )
		{
			economy.ownedPurchases[ entryIndex ] = true;
			economy.repeatCounts[ entryIndex ] = 1;
			economy.completedPurchases += 1;
		}
	}
	else if ( completionsApplied > 0 )
	{
		economy.repeatCounts[ entryIndex ] += completionsApplied;
		economy.completedPurchases += completionsApplied;
	}

	ApplyEntryState( entry, team, gameplayDirty, attributeDirty );
	return totalGranted;
}

static void ApplyAllOverloadState()
{
	bool gameplayDirty = false;
	bool attributeDirty = false;

	for ( size_t i = 0; i < overloadPurchases.size(); ++i )
	{
		for ( team_t team = TEAM_NONE; ( team = G_IterateTeams( team ) ); )
		{
			const overloadPurchaseDef_t& entry = overloadPurchases[ i ];
			if ( entry.team != TEAM_NONE && entry.team != team )
			{
				continue;
			}

			ApplyEntryState( entry, team, gameplayDirty, attributeDirty );
		}
	}

	if ( gameplayDirty )
	{
		BG_PublishGameplayConfig();
	}

	if ( attributeDirty )
	{
		BG_PublishAttributeConfig();
	}
}

static int ParseSpend( const Cmd::Args& args, std::string* error )
{
	if ( args.Argc() < 3 )
	{
		if ( error ) *error = "missing spend amount";
		return -1;
	}

	const char* token = args.Argv( args.Argc() - 1 ).c_str();
	int spend = atoi( token );
	if ( spend <= 0 )
	{
		if ( error ) *error = "spend must be positive";
		return -1;
	}

	return spend;
}

static std::string PurchaseProgressMessage( const overloadPurchaseDef_t& entry, int entryIndex, team_t team, int actualSpend,
                                            int completionsApplied, int totalGranted )
{
	const TeamEconomyState& economy = TeamEconomy( team );

	if ( entry.kind == overloadPurchaseKind_t::BP_BUNDLE )
	{
		if ( totalGranted > 0 )
		{
			return Str::Format( "%s: spent %s, gained %d BP", entry.thing, FormatOverloadCurrency( actualSpend, team ), totalGranted );
		}

		return Str::Format( "%s: invested %s / %s", entry.thing,
		                    FormatOverloadCurrency( economy.investedCredits[ entryIndex ], team ),
		                    FormatOverloadCurrency( OverloadNextCost( entry, entryIndex, team ), team ) );
	}

	if ( entry.kind == overloadPurchaseKind_t::UNLOCK )
	{
		if ( completionsApplied > 0 )
		{
			return Str::Format( "unlocked %s", entry.displayName );
		}

		return Str::Format( "%s unlock: invested %s / %s", entry.thing,
		                    FormatOverloadCurrency( economy.investedCredits[ entryIndex ], team ),
		                    FormatOverloadCurrency( OverloadNextCost( entry, entryIndex, team ), team ) );
	}

	if ( completionsApplied > 0 )
	{
		return Str::Format( "%s %s rank %d", entry.thing, entry.stat, economy.repeatCounts[ entryIndex ] );
	}

	int nextCost = OverloadNextCost( entry, entryIndex, team );
	return Str::Format( "%s %s: invested %s / %s", entry.thing, entry.stat,
	                    FormatOverloadCurrency( economy.investedCredits[ entryIndex ], team ),
	                    FormatOverloadCurrency( nextCost, team ) );
}

void G_PublishOverloadState( team_t team )
{
	if ( !G_IsPlayableTeam( team ) )
	{
		return;
	}

	PublishOverloadStateInternal( team );
}

void G_InitOverloadEconomy()
{
	BuildOverloadCatalog();

	for ( team_t team = TEAM_NONE; ( team = G_IterateTeams( team ) ); )
	{
		level.team[ team ].totalBudget = G_InitialBudgetForTeam( team );
		level.team[ team ].spentBudget = 0;
		level.team[ team ].nextMinerPayoutTime = 0;
		level.team[ team ].overloadProgress = 0;

		TeamEconomyState& economy = TeamEconomy( team );
		economy.completedPurchases = 0;
		economy.bpPurchased = 0;
		economy.peakClientsSeen = 0;
		memset( economy.investedCredits, 0, sizeof( economy.investedCredits ) );
		memset( economy.repeatCounts, 0, sizeof( economy.repeatCounts ) );
		memset( economy.ownedPurchases, 0, sizeof( economy.ownedPurchases ) );
	}

	ApplyAllOverloadState();
	G_UpdateUnlockables();
	PublishAllTeamEconomyStates();
	PublishOverloadCatalog();
}

int G_OverloadProgressValue( team_t team )
{
	if ( !G_IsPlayableTeam( team ) )
	{
		return 0;
	}

	return TeamEconomy( team ).completedPurchases;
}

int G_OverloadPurchaseCount()
{
	return static_cast<int>( overloadPurchases.size() );
}

bool G_OverloadUnlockPurchased( team_t team, unlockableType_t type, int itemNum )
{
	if ( !G_IsPlayableTeam( team ) )
	{
		return false;
	}

	int purchaseIndex = GetUnlockPurchaseIndex( team, type, itemNum );
	return purchaseIndex >= 0 && TeamEconomy( team ).ownedPurchases[ purchaseIndex ];
}

bool G_OverloadHasUnlockEntry( team_t team, unlockableType_t type, int itemNum )
{
	if ( !G_IsPlayableTeam( team ) )
	{
		return false;
	}
	return GetUnlockPurchaseIndex( team, type, itemNum ) >= 0;
}

void G_OverloadUnlockAll( team_t team )
{
	if ( !G_IsPlayableTeam( team ) )
	{
		return;
	}

	TeamEconomyState& economy = TeamEconomy( team );

	for ( size_t i = 0; i < overloadPurchases.size(); ++i )
	{
		const overloadPurchaseDef_t& entry = overloadPurchases[ i ];
		if ( entry.kind != overloadPurchaseKind_t::UNLOCK || entry.team != team )
		{
			continue;
		}

		if ( economy.ownedPurchases[ i ] )
		{
			continue;
		}

		economy.ownedPurchases[ i ] = true;
		economy.repeatCounts[ i ] = 1;
		economy.investedCredits[ i ] = 0;
		economy.completedPurchases += 1;
	}

	ApplyAllOverloadState();
	SyncOverloadProgress( team );
	G_PublishOverloadState( team );
}

bool G_OverloadPurchase( gentity_t *ent, const Cmd::Args& args, std::string* message )
{
	if ( !ent || !ent->client )
	{
		if ( message ) *message = "invalid purchaser";
		return false;
	}

	team_t team = static_cast<team_t>( ent->client->pers.team );
	if ( !G_IsPlayableTeam( team ) )
	{
		if ( message ) *message = "you are not on a playable team";
		return false;
	}

	std::string error;
	int spend = ParseSpend( args, &error );
	if ( spend <= 0 )
	{
		if ( message ) *message = error;
		return false;
	}

	int purchaseIndex = -1;
	const overloadPurchaseDef_t* entry = FindPurchase( team, args, &purchaseIndex );
	if ( !entry )
	{
		if ( message ) *message = "unknown team purchase";
		return false;
	}

	TeamEconomyState& economy = TeamEconomy( team );
	if ( !EntryIsAvailable( team, *entry ) )
	{
		if ( message ) *message = "purchase is not available";
		return false;
	}

	if ( ent->client->pers.credit < spend )
	{
		if ( message ) *message = "not enough resources";
		return false;
	}

	const int actualSpend = std::min( spend, RemainingSpendCapacity( *entry, purchaseIndex, team ) );
	if ( actualSpend <= 0 )
	{
		if ( message ) *message = "purchase is already complete";
		return false;
	}

	const int oldCompletedPurchases = economy.completedPurchases;
	G_AddCreditToClient( ent->client, -actualSpend, false );
	if ( entry->kind == overloadPurchaseKind_t::BP_BUNDLE )
	{
		G_LogCreditSpendEvent( team, ent->num(), "teambuy_bp", entry->thing.c_str(), actualSpend );
	}
	else if ( entry->kind == overloadPurchaseKind_t::UNLOCK )
	{
		G_LogCreditSpendEvent( team, ent->num(), "teambuy_unlock", entry->thing.c_str(), actualSpend );
	}
	else
	{
		std::string item = entry->thing + "/" + entry->stat;
		G_LogCreditSpendEvent( team, ent->num(), "teambuy_upgrade", item.c_str(), actualSpend );
	}
	economy.investedCredits[ purchaseIndex ] += actualSpend;

	int invested = economy.investedCredits[ purchaseIndex ];
	int completionsApplied = RanksCompletedFromSpend( *entry, economy.repeatCounts[ purchaseIndex ], invested, team );
	economy.investedCredits[ purchaseIndex ] = invested;

	bool gameplayDirty = false;
	bool attributeDirty = false;
	int totalGranted = ApplyPurchaseToTeamState( *entry, purchaseIndex, team, completionsApplied,
	                                             gameplayDirty, attributeDirty );

	if ( gameplayDirty )
	{
		BG_PublishGameplayConfig();
	}

	if ( attributeDirty )
	{
		BG_PublishAttributeConfig();
	}

	SyncOverloadProgress( team );
	G_PublishOverloadState( team );
	NotifyLegacyStageSensors( team, oldCompletedPurchases, economy.completedPurchases );

	if ( message )
	{
		*message = PurchaseProgressMessage( *entry, purchaseIndex, team, actualSpend, completionsApplied, totalGranted );
	}

	return true;
}

bool G_OverloadPurchaseByIndex( gentity_t *ent, int purchaseIndex, int spend, std::string* message )
{
	if ( !ent || !ent->client )
	{
		if ( message ) *message = "invalid purchaser";
		return false;
	}

	team_t team = static_cast<team_t>( ent->client->pers.team );
	if ( !G_IsPlayableTeam( team ) )
	{
		if ( message ) *message = "you are not on a playable team";
		return false;
	}

	if ( purchaseIndex < 0 || purchaseIndex >= static_cast<int>( overloadPurchases.size() ) )
	{
		if ( message ) *message = "unknown team purchase";
		return false;
	}

	const overloadPurchaseDef_t& entry = overloadPurchases[ purchaseIndex ];
	if ( entry.team != TEAM_NONE && entry.team != team )
	{
		if ( message ) *message = "purchase is not available";
		return false;
	}

	if ( entry.kind == overloadPurchaseKind_t::BP_BUNDLE )
	{
		return G_OverloadPurchase( ent, Cmd::Args( Str::Format( "teambuy %s %d", entry.thing, spend ) ), message );
	}

	if ( entry.kind == overloadPurchaseKind_t::UNLOCK )
	{
		return G_OverloadPurchase( ent, Cmd::Args( Str::Format( "teambuy unlock %s %d", entry.thing, spend ) ), message );
	}

	return G_OverloadPurchase( ent, Cmd::Args( Str::Format( "teambuy upgrade %s %s %d", entry.thing, entry.stat, spend ) ), message );
}

bool G_OverloadAutoDonate( gentity_t *ent, int spend, std::string* message )
{
	if ( !ent || !ent->client )
	{
		if ( message ) *message = "invalid purchaser";
		return false;
	}

	team_t team = static_cast<team_t>( ent->client->pers.team );
	if ( !G_IsPlayableTeam( team ) )
	{
		if ( message ) *message = "you are not on a playable team";
		return false;
	}

	int remainingBudget = std::min( spend, ent->client->pers.credit );
	if ( remainingBudget <= 0 )
	{
		if ( message ) *message = "not enough resources";
		return false;
	}

	std::vector<std::string> purchaseMessages;
	int totalSpent = 0;

	while ( remainingBudget > 0 )
	{
		const int purchaseIndex = FindAutoDonatePurchase( team );
		if ( purchaseIndex < 0 )
		{
			break;
		}

		const int spendCapacity = AutoDonateSpendCapacity( team, purchaseIndex );
		const int spendThisPurchase = std::min( remainingBudget, spendCapacity );
		if ( spendThisPurchase <= 0 )
		{
			break;
		}

		std::string purchaseMessage;
		if ( !G_OverloadPurchaseByIndex( ent, purchaseIndex, spendThisPurchase, &purchaseMessage ) )
		{
			if ( totalSpent <= 0 )
			{
				if ( message ) *message = purchaseMessage.empty() ? "autodonate failed" : purchaseMessage;
				return false;
			}
			break;
		}

		totalSpent += spendThisPurchase;
		remainingBudget -= spendThisPurchase;
		if ( !purchaseMessage.empty() )
		{
			purchaseMessages.push_back( purchaseMessage );
		}
	}

	if ( totalSpent <= 0 )
	{
		if ( message ) *message = "no eligible overload purchase found";
		return false;
	}

	if ( message )
	{
		std::ostringstream stream;
		stream << "spent " << FormatOverloadCurrency( totalSpent, team );
		if ( !purchaseMessages.empty() )
		{
			stream << ": ";
			for ( size_t i = 0; i < purchaseMessages.size(); ++i )
			{
				if ( i > 0 )
				{
					stream << "; ";
				}
				stream << purchaseMessages[ i ];
			}
		}
		*message = stream.str();
	}

	return true;
}

void G_UpdateOverloadCostScaling()
{
	UpdateOverloadCostScalingInternal();
}
