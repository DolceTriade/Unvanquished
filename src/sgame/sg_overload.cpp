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
#include "shared/bg_attributes.h"
#include "shared/bg_teamprogress.h"

#include <limits>
#include <sstream>
#include <vector>

namespace {

enum class overloadPurchaseKind_t
{
	BP_BUNDLE,
	UNLOCK,
	UPGRADE,
};

enum class effectTarget_t
{
	GAMEPLAY,
	ATTRIBUTE,
};

enum class effectValueType_t
{
	INTEGER,
	FLOAT,
};

struct overloadEffect_t
{
	effectTarget_t      target;
	effectValueType_t   valueType;
	int                 gameplayIndex;
	bgAttributeFamily_t attributeFamily;
	int                 attributeObject;
	int                 attributeField;
	double              baseline;
	double              step;
	double              minValue;
	double              maxValue;
};

struct overloadPurchaseDef_t
{
	overloadPurchaseKind_t kind;
	team_t                 team;
	std::string            thing;
	std::string            stat;
	std::string            displayName;
	std::string            uiDescription;
	int                    requiredCompletedCount;
	int                    baseCost;
	int                    costStep;
	int                    bundleAmount;
	int                    maxRanks;
	bgAttributeFamily_t    unlockFamily;
	int                    unlockObject;
	int                    unlockField;
	std::vector<overloadEffect_t> effects;
};

std::vector<overloadPurchaseDef_t> overloadPurchases;
bool overloadCatalogReady = false;

static void BuildOverloadCatalog();

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

static void CheckOverloadCatalogReady()
{
	if ( !overloadCatalogReady )
	{
		Sys::Error( "Overload catalog accessed before initialization" );
	}
}

constexpr int OVERLOAD_STAGE2_COUNT = 3;
constexpr int OVERLOAD_STAGE3_COUNT = 6;
constexpr int OVERLOAD_BP_BUNDLE_COST = 350;
constexpr int OVERLOAD_BP_BUNDLE_AMOUNT = 25;
constexpr int OVERLOAD_UNCAPPED_RANKS = std::numeric_limits<int>::max();

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
	       << ";ic=" << EncodeIndexValuePairs( economy.investedCredits )
	       << ";rc=" << EncodeIndexValuePairs( economy.repeatCounts )
	       << ";op=" << EncodeOwnedPurchases( economy.ownedPurchases );

	std::string config = stream.str();
	if ( config.size() >= BIG_INFO_STRING )
	{
		Sys::Error( "team economy configstring exceeded BIG_INFO_STRING (%zu >= %d)",
		            config.size(), BIG_INFO_STRING );
	}

	trap_SetConfigstring( CS_OVERLOAD + team, config.c_str() );
}

static void PublishAllTeamEconomyStates()
{
	for ( team_t team = TEAM_NONE; ( team = G_IterateTeams( team ) ); )
	{
		::G_PublishOverloadState( team );
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

static void PublishOverloadCatalog()
{
	CheckOverloadCatalogReady();

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
			Info_SetValueForKey( config, "stat", entry.stat.c_str(), false );
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

static int StageForCompletedPurchases( int completedPurchases )
{
	if ( completedPurchases >= OVERLOAD_STAGE3_COUNT )
	{
		return 3;
	}

	if ( completedPurchases >= OVERLOAD_STAGE2_COUNT )
	{
		return 2;
	}

	return 1;
}

static void NotifyLegacyStageSensors( team_t team, int oldCompletedPurchases, int newCompletedPurchases )
{
	int oldStage = StageForCompletedPurchases( oldCompletedPurchases );
	int newStage = StageForCompletedPurchases( newCompletedPurchases );

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

static void TeamCenterPrint( team_t team, const std::string& message )
{
	if ( message.empty() )
	{
		return;
	}

	G_TeamCommand( team, va( "cp %s", Quote( message.c_str() ) ) );
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

static const char* CanonicalThingName( const char* thing )
{
	if ( !Q_stricmp( thing, "basilisk" ) )
	{
		return "mantis";
	}

	if ( !Q_stricmp( thing, "adv_basilisk" ) )
	{
		return "adv_mantis";
	}

	return thing;
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
	if ( fieldIndex < 0 )
	{
		if ( !Q_stricmp( fieldName, "maxClips" ) )
		{
			fieldIndex = BG_FindAttributeField( family, "clips" );
		}
		else if ( !Q_stricmp( fieldName, "maxAmmo" ) )
		{
			fieldIndex = BG_FindAttributeField( family, "ammo" );
		}
		else if ( !Q_stricmp( fieldName, "unlockThreshold" ) )
		{
			fieldIndex = BG_FindAttributeField( family, "unlock_threshold" );
		}
	}
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

static int DefaultUpgradeBaseCost( int requiredCompletedCount )
{
	return 250 + requiredCompletedCount * 50;
}

static int UnlockCost( bgAttributeFamily_t family, int objectIndex, int requiredCompletedCount )
{
	int intrinsic = 0;

	switch ( family )
	{
		case BG_ATTR_WEAPON: intrinsic = BG_Weapon( objectIndex + 1 )->price; break;
		case BG_ATTR_UPGRADE: intrinsic = BG_Upgrade( objectIndex + 1 )->price; break;
		case BG_ATTR_BUILDABLE: intrinsic = BG_Buildable( objectIndex + 1 )->buildPoints * 10; break;
		case BG_ATTR_CLASS: intrinsic = BG_Class( objectIndex )->price / 2; break;
		default: intrinsic = 0; break;
	}

	return std::max( 200 + requiredCompletedCount * 50, intrinsic );
}

static void AddUpgrade( team_t team, int requiredCompletedCount, int baseCost, int costStep, int maxRanks,
                        const char* thing, const char* stat, const char* displayName, const char* uiDescription,
                        std::initializer_list<overloadEffect_t> effects )
{
	overloadPurchaseDef_t entry{};
	entry.kind = overloadPurchaseKind_t::UPGRADE;
	entry.team = team;
	entry.thing = thing;
	entry.stat = stat;
	entry.displayName = displayName;
	entry.uiDescription = uiDescription;
	entry.requiredCompletedCount = requiredCompletedCount;
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

static void AddUnlock( team_t team, int requiredCompletedCount, unlockableType_t unlockableType, int itemNum,
                       bgAttributeFamily_t family, int objectIndex, int unlockField,
                       const char* thing, const char* displayName, const char* uiDescription )
{
	overloadPurchaseDef_t entry{};
	entry.kind = overloadPurchaseKind_t::UNLOCK;
	entry.team = team;
	entry.thing = thing;
	entry.displayName = displayName;
	entry.uiDescription = uiDescription;
	entry.requiredCompletedCount = requiredCompletedCount;
	entry.baseCost = UnlockCost( family, objectIndex, requiredCompletedCount );
	entry.costStep = 0;
	entry.bundleAmount = 0;
	entry.maxRanks = 1;
	entry.unlockFamily = family;
	entry.unlockObject = objectIndex;
	entry.unlockField = unlockField;
	overloadPurchases.push_back( std::move( entry ) );
	SetUnlockPurchaseIndex( team, unlockableType, itemNum, static_cast<int>( overloadPurchases.size() ) - 1 );
}

static int FindUnlockThresholdField( bgAttributeFamily_t family )
{
	int field = BG_FindAttributeField( family, "unlock_threshold" );
	if ( field >= 0 )
	{
		return field;
	}

	// Tolerate legacy camelCase naming if a family ever exposes it that way.
	return BG_FindAttributeField( family, "unlockThreshold" );
}

static void AddUnlockEntriesForFamily( bgAttributeFamily_t family, unlockableType_t unlockableType, int start, int end )
{
	const int unlockField = FindUnlockThresholdField( family );
	if ( unlockField < 0 )
	{
		Sys::Error( "unlockThreshold field missing for family %d", family );
	}

	for ( int itemNum = start; itemNum < end; ++itemNum )
	{
		team_t team = TEAM_NONE;
		int unlockThreshold = 0;
		const char* thing = nullptr;

		switch ( unlockableType )
		{
			case UNLT_WEAPON:
				team = BG_Weapon( itemNum )->team;
				unlockThreshold = BG_Weapon( itemNum )->unlockThreshold;
				thing = BG_Weapon( itemNum )->name;
				break;

			case UNLT_UPGRADE:
				team = BG_Upgrade( itemNum )->team;
				unlockThreshold = BG_Upgrade( itemNum )->unlockThreshold;
				thing = BG_Upgrade( itemNum )->name;
				break;

			case UNLT_BUILDABLE:
				team = BG_Buildable( itemNum )->team;
				unlockThreshold = BG_Buildable( itemNum )->unlockThreshold;
				thing = BG_Buildable( itemNum )->name;
				break;

			case UNLT_CLASS:
				team = BG_Class( itemNum )->team;
				unlockThreshold = BG_Class( itemNum )->unlockThreshold;
				thing = BG_Class( itemNum )->name;
				break;

			case UNLT_NUM_UNLOCKABLETYPES:
				Sys::Error( "AddUnlockEntriesForFamily: invalid unlockable type" );
		}

		if ( !G_IsPlayableTeam( team ) || unlockThreshold <= 0 )
		{
			continue;
		}

		const int objectIndex = BG_FindAttributeObject( family, thing );
		if ( objectIndex < 0 )
		{
			Sys::Error( "could not resolve attribute object for %s", thing );
		}

		const int requiredCompletedCount = BG_NormalizeUnlockThreshold( unlockThreshold );
		std::string displayName = UnlockableDisplayName( unlockableType, itemNum );
		std::string uiDescription = UnlockableDescription( unlockableType, itemNum );
		AddUnlock( team, requiredCompletedCount, unlockableType, itemNum, family, objectIndex, unlockField,
		           thing, displayName.c_str(), uiDescription.c_str() );
	}
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
	bpBundle.thing = "bp_25";
	bpBundle.displayName = "BP +25";
	bpBundle.uiDescription = "Add 25 team BP.";
	bpBundle.requiredCompletedCount = 0;
	bpBundle.baseCost = OVERLOAD_BP_BUNDLE_COST;
	bpBundle.costStep = 0;
	bpBundle.bundleAmount = OVERLOAD_BP_BUNDLE_AMOUNT;
	bpBundle.maxRanks = std::numeric_limits<int>::max();
	bpBundle.unlockFamily = BG_NUM_ATTRIBUTE_FAMILIES;
	bpBundle.unlockObject = -1;
	bpBundle.unlockField = -1;
	overloadPurchases.push_back( bpBundle );

	AddUnlockEntriesForFamily( BG_ATTR_WEAPON, UNLT_WEAPON, WP_NONE + 1, WP_NUM_WEAPONS );
	AddUnlockEntriesForFamily( BG_ATTR_UPGRADE, UNLT_UPGRADE, UP_NONE + 1, UP_NUM_UPGRADES );
	AddUnlockEntriesForFamily( BG_ATTR_BUILDABLE, UNLT_BUILDABLE, BA_NONE + 1, BA_NUM_BUILDABLES );
	AddUnlockEntriesForFamily( BG_ATTR_CLASS, UNLT_CLASS, PCL_NONE + 1, PCL_NUM_CLASSES );

	AddUpgrade( TEAM_HUMANS, 0, DefaultUpgradeBaseCost( 0 ), 100, OVERLOAD_UNCAPPED_RANKS, "rifle", "damage", "Rifle Damage", "Increase rifle damage.",
	            { GameplayEffect( "RIFLE_DMG", 3.0 ) } );
	AddUpgrade( TEAM_HUMANS, 0, DefaultUpgradeBaseCost( 0 ), 100, OVERLOAD_UNCAPPED_RANKS, "rifle", "clips", "Rifle Clips", "Increase rifle magazine count.",
	            { AttributeEffect( BG_ATTR_WEAPON, "rifle", "clips", 1.0, 0.0 ) } );
	AddUpgrade( TEAM_HUMANS, 0, DefaultUpgradeBaseCost( 0 ), 100, OVERLOAD_UNCAPPED_RANKS, "shotgun", "damage", "Shotgun Damage", "Increase shotgun pellet damage.",
	            { GameplayEffect( "SHOTGUN_DMG", 1.0 ) } );
	AddUpgrade( TEAM_HUMANS, 0, DefaultUpgradeBaseCost( 0 ), 100, OVERLOAD_UNCAPPED_RANKS, "shotgun", "clips", "Shotgun Clips", "Increase shotgun magazine count.",
	            { AttributeEffect( BG_ATTR_WEAPON, "shotgun", "clips", 1.0, 0.0 ) } );
	AddUpgrade( TEAM_HUMANS, 0, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), 125, OVERLOAD_UNCAPPED_RANKS, "lasgun", "damage", "Lasgun Damage", "Increase lasgun damage.",
	            { GameplayEffect( "LASGUN_DAMAGE", 3.0 ) } );
	AddUpgrade( TEAM_HUMANS, 0, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), 125, OVERLOAD_UNCAPPED_RANKS, "lasgun", "ammo", "Lasgun Ammo", "Increase lasgun ammo reserve.",
	            { AttributeEffect( BG_ATTR_WEAPON, "lgun", "ammo", 25.0, 1.0 ) } );
	AddUpgrade( TEAM_HUMANS, 0, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), 125, OVERLOAD_UNCAPPED_RANKS, "chaingun", "damage", "Chaingun Damage", "Increase chaingun damage.",
	            { GameplayEffect( "CHAINGUN_DMG", 2.0 ) } );
	AddUpgrade( TEAM_HUMANS, 0, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), 125, OVERLOAD_UNCAPPED_RANKS, "chaingun", "clips", "Chaingun Clips", "Increase chaingun magazine count.",
	            { AttributeEffect( BG_ATTR_WEAPON, "chaingun", "clips", 1.0, 0.0 ) } );
	AddUpgrade( TEAM_HUMANS, 0, DefaultUpgradeBaseCost( OVERLOAD_STAGE3_COUNT ), 150, OVERLOAD_UNCAPPED_RANKS, "lcannon", "damage", "Lucifer Cannon Damage", "Increase lucifer cannon damage.",
	            { GameplayEffect( "LCANNON_DAMAGE", 15.0 ) } );
	AddUpgrade( TEAM_HUMANS, 0, DefaultUpgradeBaseCost( OVERLOAD_STAGE3_COUNT ), 150, OVERLOAD_UNCAPPED_RANKS, "lcannon", "ammo", "Lucifer Cannon Ammo", "Increase lucifer cannon ammo reserve.",
	            { AttributeEffect( BG_ATTR_WEAPON, "lcannon", "ammo", 10.0, 1.0 ) } );
	AddUpgrade( TEAM_HUMANS, 0, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), 125, OVERLOAD_UNCAPPED_RANKS, "jetpack", "fuel", "Jetpack Fuel", "Increase jetpack fuel capacity.",
	            { GameplayEffect( "JETPACK_FUEL_MAX", 2500.0, 1.0 ) } );
	AddUpgrade( TEAM_HUMANS, 0, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), 125, OVERLOAD_UNCAPPED_RANKS, "jetpack", "recharge", "Jetpack Recharge", "Increase jetpack fuel recharge.",
	            { GameplayEffect( "JETPACK_FUEL_RESTORE", 1.0, 1.0 ) } );
	AddUpgrade( TEAM_HUMANS, 0, DefaultUpgradeBaseCost( 0 ), 100, OVERLOAD_UNCAPPED_RANKS, "medkit", "startup", "Medkit Startup", "Speed up medkit activation.",
	            { GameplayEffect( "MEDKIT_STARTUP_SPEED", 50.0 ) } );
	AddUpgrade( TEAM_HUMANS, 0, DefaultUpgradeBaseCost( 0 ), 100, OVERLOAD_UNCAPPED_RANKS, "medkit", "poison", "Medkit Poison Protection", "Extend medkit poison immunity.",
	            { GameplayEffect( "MEDKIT_POISON_IMMUNITY_TIME", 1000.0, 0.0 ) } );
	AddUpgrade( TEAM_HUMANS, 0, DefaultUpgradeBaseCost( 0 ), 100, OVERLOAD_UNCAPPED_RANKS, "mgturret", "range", "Machinegun Turret Range", "Increase machinegun turret range.",
	            { GameplayEffect( "MGTURRET_RANGE", 25.0 ) } );
	AddUpgrade( TEAM_HUMANS, 0, DefaultUpgradeBaseCost( 0 ), 100, OVERLOAD_UNCAPPED_RANKS, "mgturret", "repeat", "Machinegun Turret Rate", "Increase machinegun turret fire rate.",
	            { GameplayEffect( "MGTURRET_ATTACK_PERIOD", -10.0, 25.0 ) } );
	AddUpgrade( TEAM_HUMANS, 0, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), 125, OVERLOAD_UNCAPPED_RANKS, "rocketpod", "range", "Rocketpod Range", "Increase rocketpod range.",
	            { GameplayEffect( "ROCKETPOD_RANGE", 80.0 ) } );
	AddUpgrade( TEAM_HUMANS, 0, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), 125, OVERLOAD_UNCAPPED_RANKS, "rocketpod", "repeat", "Rocketpod Rate", "Increase rocketpod fire rate.",
	            { GameplayEffect( "ROCKETPOD_ATTACK_PERIOD", -75.0, 250.0 ) } );

	AddUpgrade( TEAM_ALIENS, 0, DefaultUpgradeBaseCost( 0 ), 100, OVERLOAD_UNCAPPED_RANKS, "dretch", "damage", "Dretch Damage", "Increase dretch bite damage.",
	            { GameplayEffect( "LEVEL0_BITE_DMG", 3.0 ) } );
	AddUpgrade( TEAM_ALIENS, 0, DefaultUpgradeBaseCost( 0 ), 100, OVERLOAD_UNCAPPED_RANKS, "dretch", "range", "Dretch Range", "Increase dretch bite range.",
	            { GameplayEffect( "LEVEL0_BITE_RANGE", 5.0 ) } );
	AddUpgrade( TEAM_ALIENS, 0, DefaultUpgradeBaseCost( 0 ), 100, OVERLOAD_UNCAPPED_RANKS, "mantis", "damage", "Mantis Damage", "Increase mantis claw damage.",
	            { GameplayEffect( "LEVEL2_CLAW_DMG", 4.0 ) } );
	AddUpgrade( TEAM_ALIENS, 0, DefaultUpgradeBaseCost( 0 ), 100, OVERLOAD_UNCAPPED_RANKS, "mantis", "range", "Mantis Range", "Increase mantis claw range.",
	            { GameplayEffect( "LEVEL2_CLAW_RANGE", 5.0 ) } );
	AddUpgrade( TEAM_ALIENS, 0, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), 125, OVERLOAD_UNCAPPED_RANKS, "adv_mantis", "damage", "Advanced Mantis Zap Damage", "Increase advanced mantis zap damage.",
	            { GameplayEffect( "LEVEL2_AREAZAP_DMG", 4.0 ) } );
	AddUpgrade( TEAM_ALIENS, 0, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), 125, OVERLOAD_UNCAPPED_RANKS, "adv_mantis", "range", "Advanced Mantis Zap Range", "Increase advanced mantis zap range.",
	            { GameplayEffect( "LEVEL2_AREAZAP_RANGE", 10.0 ) } );
	AddUpgrade( TEAM_ALIENS, 0, DefaultUpgradeBaseCost( 0 ), 100, OVERLOAD_UNCAPPED_RANKS, "marauder", "damage", "Marauder Damage", "Increase marauder zap damage.",
	            { GameplayEffect( "LEVEL2_AREAZAP_DMG", 4.0 ) } );
	AddUpgrade( TEAM_ALIENS, 0, DefaultUpgradeBaseCost( 0 ), 100, OVERLOAD_UNCAPPED_RANKS, "marauder", "range", "Marauder Range", "Increase marauder zap range.",
	            { GameplayEffect( "LEVEL2_AREAZAP_RANGE", 10.0 ) } );
	AddUpgrade( TEAM_ALIENS, 0, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), 125, OVERLOAD_UNCAPPED_RANKS, "dragoon", "damage", "Dragoon Damage", "Increase dragoon claw damage.",
	            { GameplayEffect( "LEVEL3_CLAW_DMG", 5.0 ) } );
	AddUpgrade( TEAM_ALIENS, 0, DefaultUpgradeBaseCost( OVERLOAD_STAGE2_COUNT ), 125, OVERLOAD_UNCAPPED_RANKS, "dragoon", "pounce_damage", "Dragoon Pounce Damage", "Increase dragoon pounce damage.",
	            { GameplayEffect( "LEVEL3_POUNCE_DMG", 10.0 ) } );
	AddUpgrade( TEAM_ALIENS, 0, DefaultUpgradeBaseCost( OVERLOAD_STAGE3_COUNT ), 150, OVERLOAD_UNCAPPED_RANKS, "adv_dragoon", "damage", "Advanced Dragoon Damage", "Increase advanced dragoon claw damage.",
	            { GameplayEffect( "LEVEL3_CLAW_DMG", 5.0 ) } );
	AddUpgrade( TEAM_ALIENS, 0, DefaultUpgradeBaseCost( OVERLOAD_STAGE3_COUNT ), 150, OVERLOAD_UNCAPPED_RANKS, "adv_dragoon", "pounce_range", "Advanced Dragoon Pounce Range", "Increase advanced dragoon pounce range.",
	            { GameplayEffect( "LEVEL3_POUNCE_UPG_RANGE", 15.0 ) } );
	AddUpgrade( TEAM_ALIENS, 0, DefaultUpgradeBaseCost( OVERLOAD_STAGE3_COUNT ), 150, OVERLOAD_UNCAPPED_RANKS, "tyrant", "damage", "Tyrant Damage", "Increase tyrant claw damage.",
	            { GameplayEffect( "LEVEL4_CLAW_DMG", 8.0 ) } );
	AddUpgrade( TEAM_ALIENS, 0, DefaultUpgradeBaseCost( OVERLOAD_STAGE3_COUNT ), 150, OVERLOAD_UNCAPPED_RANKS, "tyrant", "trample_damage", "Tyrant Trample Damage", "Increase tyrant trample damage.",
	            { GameplayEffect( "LEVEL4_TRAMPLE_DMG", 10.0 ) } );

	if ( overloadPurchases.size() > MAX_OVERLOAD_PURCHASES )
	{
		Sys::Error( "Overload purchase catalog exceeds MAX_OVERLOAD_PURCHASES (%zu > %d)",
		            overloadPurchases.size(), MAX_OVERLOAD_PURCHASES );
	}

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
		const char* requestedThing = CanonicalThingName( args.Argv( 2 ).c_str() );
		return args.Argc() >= 3 &&
		       !Q_stricmp( args.Argv( 1 ).c_str(), "unlock" ) &&
		       !Q_stricmp( requestedThing, entry.thing.c_str() );
	}

	const char* requestedThing = CanonicalThingName( args.Argv( 2 ).c_str() );
	return args.Argc() >= 4 &&
	       !Q_stricmp( args.Argv( 1 ).c_str(), "upgrade" ) &&
	       !Q_stricmp( requestedThing, entry.thing.c_str() ) &&
	       !Q_stricmp( args.Argv( 3 ).c_str(), entry.stat.c_str() );
}

static const overloadPurchaseDef_t* FindPurchase( team_t team, const Cmd::Args& args, int* purchaseIndex = nullptr )
{
	CheckOverloadCatalogReady();

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

static bool EntryIsAvailable( team_t team, const overloadPurchaseDef_t& entry )
{
	if ( entry.team != TEAM_NONE && entry.team != team )
	{
		return false;
	}

	const TeamEconomyState& economy = TeamEconomy( team );
	const size_t index = &entry - overloadPurchases.data();

	if ( economy.completedPurchases < entry.requiredCompletedCount )
	{
		return false;
	}

	if ( entry.kind == overloadPurchaseKind_t::UNLOCK )
	{
		return !economy.ownedPurchases[ index ];
	}

	return economy.repeatCounts[ index ] < entry.maxRanks;
}

static int RemainingSpendCapacity( const overloadPurchaseDef_t& entry, int entryIndex, team_t team )
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
		return std::max( 0, entry.baseCost - invested );
	}

	if ( entry.maxRanks == OVERLOAD_UNCAPPED_RANKS )
	{
		return std::numeric_limits<int>::max();
	}

	int remaining = -invested;
	for ( int rank = currentRank; rank < entry.maxRanks; ++rank )
	{
		remaining += entry.baseCost + rank * entry.costStep;
	}

	return std::max( 0, remaining );
}

static int RanksCompletedFromSpend( const overloadPurchaseDef_t& entry, int currentRank, int& investedCredits )
{
	if ( entry.kind == overloadPurchaseKind_t::UNLOCK )
	{
		if ( investedCredits >= entry.baseCost )
		{
			investedCredits = 0;
			return 1;
		}
		return 0;
	}

	if ( entry.kind == overloadPurchaseKind_t::BP_BUNDLE )
	{
		const int completed = investedCredits / entry.baseCost;
		investedCredits %= entry.baseCost;
		return completed;
	}

	int completed = 0;
	while ( currentRank + completed < entry.maxRanks )
	{
		int threshold = entry.baseCost + ( currentRank + completed ) * entry.costStep;
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
	CheckOverloadCatalogReady();

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

static void AnnounceAvailability( team_t team, int oldCompletedPurchases, int newCompletedPurchases )
{
	if ( newCompletedPurchases <= oldCompletedPurchases )
	{
		return;
	}

	const int oldStage = StageForCompletedPurchases( oldCompletedPurchases );
	const int newStage = StageForCompletedPurchases( newCompletedPurchases );
	if ( oldStage != newStage )
	{
		TeamCenterPrint( team, Str::Format( "Overload stage %d reached.", newStage ) );
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
			return Str::Format( "%s: spent %d credits, gained %d BP", entry.thing, actualSpend, totalGranted );
		}

		return Str::Format( "%s: invested %d/%d", entry.thing, economy.investedCredits[ entryIndex ], entry.baseCost );
	}

	if ( entry.kind == overloadPurchaseKind_t::UNLOCK )
	{
		if ( completionsApplied > 0 )
		{
			return Str::Format( "unlocked %s", entry.displayName );
		}

		return Str::Format( "%s unlock: invested %d/%d", entry.thing, economy.investedCredits[ entryIndex ], entry.baseCost );
	}

	if ( completionsApplied > 0 )
	{
		return Str::Format( "%s %s rank %d", entry.thing, entry.stat, economy.repeatCounts[ entryIndex ] );
	}

	int nextCost = entry.baseCost + economy.repeatCounts[ entryIndex ] * entry.costStep;
	return Str::Format( "%s %s: invested %d/%d", entry.thing, entry.stat, economy.investedCredits[ entryIndex ], nextCost );
}

} // namespace

void G_PublishOverloadState( team_t team )
{
	if ( !G_IsPlayableTeam( team ) )
	{
		return;
	}

	PublishOverloadStateInternal( team );
}

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
	BuildOverloadCatalog();

	for ( team_t team = TEAM_NONE; ( team = G_IterateTeams( team ) ); )
	{
		level.team[ team ].totalBudget = InitialBudgetForTeam( team );
		level.team[ team ].spentBudget = 0;
		level.team[ team ].queuedBudget = 0;
		level.team[ team ].overloadProgress = 0;

		TeamEconomyState& economy = TeamEconomy( team );
		economy.completedPurchases = 0;
		economy.bpPurchased = 0;
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

bool G_OverloadUnlockPurchased( team_t team, unlockableType_t type, int itemNum )
{
	if ( !G_IsPlayableTeam( team ) )
	{
		return false;
	}

	CheckOverloadCatalogReady();

	int purchaseIndex = GetUnlockPurchaseIndex( team, type, itemNum );
	return purchaseIndex >= 0 && TeamEconomy( team ).ownedPurchases[ purchaseIndex ];
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
		if ( message ) *message = "not enough credits";
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
	economy.investedCredits[ purchaseIndex ] += actualSpend;

	int invested = economy.investedCredits[ purchaseIndex ];
	int completionsApplied = RanksCompletedFromSpend( *entry, economy.repeatCounts[ purchaseIndex ], invested );
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
	AnnounceAvailability( team, oldCompletedPurchases, economy.completedPurchases );

	if ( message )
	{
		*message = PurchaseProgressMessage( *entry, purchaseIndex, team, actualSpend, completionsApplied, totalGranted );
	}

	return true;
}
