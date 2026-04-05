/*
===========================================================================

Unvanquished GPL Source Code
Copyright (C) 2026 Unvanquished Developers

This file is part of the Unvanquished GPL Source Code (Unvanquished Source Code).

Unvanquished Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Unvanquished Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Unvanquished Source Code.  If not, see <http://www.gnu.org/licenses/>.

===========================================================================
*/

#include "common/Common.h"
#include "bg_attributes.h"
#include "bg_public.h"
#include "shared/lua/Beacons.h"
#include "shared/lua/Buildables.h"
#include "shared/lua/Classes.h"
#include "shared/lua/Missiles.h"
#include "shared/lua/Upgrades.h"
#include "shared/lua/Weapons.h"

#include <array>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <vector>

#ifdef BUILD_SGAME
#include "sgame/sg_local.h"
#endif

namespace {

constexpr int ATTRIBUTE_CONFIG_SCHEMA = 1;

using bgAttributeFieldState_t = bgAttributeTrackedField_t;

struct bgAttributeOverrideState_t
{
	bgAttributeValue_t value;
	bool present;
};

struct bgAttributeFamilyState_t
{
	bgAttributeFamily_t family;
	const char* name;
	const bgAttributeTrackedField_t* fields;
	size_t fieldCount;
	size_t objectCount;
	size_t objectSize;
	void* baselines;
	std::vector<bgAttributeOverrideState_t> overrides;
};

buildableAttributes_t buildableBaselines[ BA_NUM_BUILDABLES ];
classAttributes_t classBaselines[ PCL_NUM_CLASSES ];
weaponAttributes_t weaponBaselines[ WP_NUM_WEAPONS ];
upgradeAttributes_t upgradeBaselines[ UP_NUM_UPGRADES ];
missileAttributes_t missileBaselines[ MIS_NUM_MISSILES ];
beaconAttributes_t beaconBaselines[ NUM_BEACON_TYPES ];

void SetError( std::string* error, Str::StringRef message )
{
	if ( error )
	{
		*error = message;
	}
}

float CanonicalizeFloat( float value )
{
	std::ostringstream stream;
	stream.imbue( std::locale::classic() );
	stream << std::setprecision( std::numeric_limits<float>::max_digits10 ) << value;

	std::istringstream parse( stream.str() );
	parse.imbue( std::locale::classic() );

	float canonical = value;
	if ( !( parse >> canonical ) )
	{
		Sys::Drop( "failed to canonicalize attribute float" );
	}

	return canonical;
}

std::array<bgAttributeFamilyState_t, BG_NUM_ATTRIBUTE_FAMILIES>& Families()
{
	static std::array<bgAttributeFamilyState_t, BG_NUM_ATTRIBUTE_FAMILIES> families =
	{{
		{ BG_ATTR_BUILDABLE, "buildables", Shared::Lua::BuildableAttributeFields(), Shared::Lua::NumBuildableAttributeFields(), BA_NUM_BUILDABLES - 1, sizeof( buildableAttributes_t ), buildableBaselines, {} },
		{ BG_ATTR_CLASS, "classes", Shared::Lua::ClassAttributeFields(), Shared::Lua::NumClassAttributeFields(), PCL_NUM_CLASSES - 1, sizeof( classAttributes_t ), classBaselines, {} },
		{ BG_ATTR_WEAPON, "weapons", Shared::Lua::WeaponAttributeFields(), Shared::Lua::NumWeaponAttributeFields(), WP_NUM_WEAPONS - 1, sizeof( weaponAttributes_t ), weaponBaselines, {} },
		{ BG_ATTR_UPGRADE, "upgrades", Shared::Lua::UpgradeAttributeFields(), Shared::Lua::NumUpgradeAttributeFields(), UP_NUM_UPGRADES - 1, sizeof( upgradeAttributes_t ), upgradeBaselines, {} },
		{ BG_ATTR_MISSILE, "missiles", Shared::Lua::MissileAttributeFields(), Shared::Lua::NumMissileAttributeFields(), MIS_NUM_MISSILES - 1, sizeof( missileAttributes_t ), missileBaselines, {} },
		{ BG_ATTR_BEACON, "beacons", Shared::Lua::BeaconAttributeFields(), Shared::Lua::NumBeaconAttributeFields(), NUM_BEACON_TYPES - 1, sizeof( beaconAttributes_t ), beaconBaselines, {} },
	}};

	return families;
}

bgAttributeFamilyState_t& FamilyState( bgAttributeFamily_t family )
{
	return Families()[ static_cast<size_t>( family ) ];
}

size_t ArrayIndexFromObjectIndex( size_t objectIndex )
{
	return objectIndex + 1;
}

void* CurrentObject( bgAttributeFamily_t family, size_t objectIndex )
{
	const size_t arrayIndex = ArrayIndexFromObjectIndex( objectIndex );

	switch ( family )
	{
		case BG_ATTR_BUILDABLE: return const_cast<buildableAttributes_t*>( BG_Buildable( arrayIndex ) );
		case BG_ATTR_CLASS: return const_cast<classAttributes_t*>( BG_Class( arrayIndex ) );
		case BG_ATTR_WEAPON: return const_cast<weaponAttributes_t*>( BG_Weapon( arrayIndex ) );
		case BG_ATTR_UPGRADE: return const_cast<upgradeAttributes_t*>( BG_Upgrade( arrayIndex ) );
		case BG_ATTR_MISSILE: return const_cast<missileAttributes_t*>( BG_Missile( arrayIndex ) );
		case BG_ATTR_BEACON: return const_cast<beaconAttributes_t*>( BG_Beacon( arrayIndex ) );
		default: return nullptr;
	}
}

const void* BaselineObject( bgAttributeFamily_t family, size_t objectIndex )
{
	const bgAttributeFamilyState_t& state = FamilyState( family );
	return static_cast<const char*>( state.baselines ) + state.objectSize * ArrayIndexFromObjectIndex( objectIndex );
}

bgAttributeOverrideState_t& OverrideState( bgAttributeFamily_t family, size_t objectIndex, size_t fieldIndex )
{
	bgAttributeFamilyState_t& state = FamilyState( family );
	return state.overrides[ objectIndex * state.fieldCount + fieldIndex ];
}

const bgAttributeTrackedField_t* FieldState( bgAttributeFamily_t family, size_t fieldIndex )
{
	const bgAttributeFamilyState_t& state = FamilyState( family );
	return fieldIndex < state.fieldCount ? &state.fields[ fieldIndex ] : nullptr;
}

bool ValidateIndices( bgAttributeFamily_t family, size_t objectIndex, size_t fieldIndex, std::string* error )
{
	const bgAttributeFamilyState_t& state = FamilyState( family );
	if ( objectIndex >= state.objectCount )
	{
		SetError( error, "invalid attribute object index" );
		return false;
	}

	if ( fieldIndex >= state.fieldCount )
	{
		SetError( error, "invalid attribute field index" );
		return false;
	}

	return true;
}

bgAttributeValue_t ReadValue( const void* object, const bgAttributeFieldState_t& field )
{
	bgAttributeValue_t value{};
	const char* data = static_cast<const char*>( object ) + field.offset;

	switch ( field.info.type )
	{
		case BG_ATTR_INTEGER: value.integer = *reinterpret_cast<const int*>( data ); break;
		case BG_ATTR_FLOAT: value.number = *reinterpret_cast<const float*>( data ); break;
		case BG_ATTR_BOOL: value.boolean = *reinterpret_cast<const bool*>( data ); break;
	}

	return value;
}

void WriteValue( void* object, const bgAttributeFieldState_t& field, bgAttributeValue_t value )
{
	char* data = static_cast<char*>( object ) + field.offset;

	switch ( field.info.type )
	{
		case BG_ATTR_INTEGER: *reinterpret_cast<int*>( data ) = value.integer; break;
		case BG_ATTR_FLOAT: *reinterpret_cast<float*>( data ) = value.number; break;
		case BG_ATTR_BOOL: *reinterpret_cast<bool*>( data ) = value.boolean; break;
	}
}

bool ValuesEqual( const bgAttributeFieldState_t& field, bgAttributeValue_t lhs, bgAttributeValue_t rhs )
{
	switch ( field.info.type )
	{
		case BG_ATTR_INTEGER: return lhs.integer == rhs.integer;
		case BG_ATTR_FLOAT: return lhs.number == rhs.number;
		case BG_ATTR_BOOL: return lhs.boolean == rhs.boolean;
	}

	return false;
}

void EnsureOverrideStorage()
{
	for ( bgAttributeFamilyState_t& state : Families() )
	{
		if ( state.overrides.empty() )
		{
			state.overrides.resize( state.objectCount * state.fieldCount );
		}
	}
}

void RefreshOverrideState( bgAttributeFamily_t family, size_t objectIndex, size_t fieldIndex )
{
	const bgAttributeFieldState_t& field = *FieldState( family, fieldIndex );
	const bgAttributeValue_t current = ReadValue( CurrentObject( family, objectIndex ), field );
	const bgAttributeValue_t baseline = ReadValue( BaselineObject( family, objectIndex ), field );
	bgAttributeOverrideState_t& override = OverrideState( family, objectIndex, fieldIndex );

	if ( ValuesEqual( field, current, baseline ) )
	{
		override.present = false;
		return;
	}

	override.present = true;
	override.value = current;
}

void ResetTrackedFieldsToBaseline()
{
	for ( const bgAttributeFamilyState_t& familyState : Families() )
	{
		for ( size_t objectIndex = 0; objectIndex < familyState.objectCount; ++objectIndex )
		{
			void* current = CurrentObject( familyState.family, objectIndex );
			const void* baseline = BaselineObject( familyState.family, objectIndex );

			for ( size_t fieldIndex = 0; fieldIndex < familyState.fieldCount; ++fieldIndex )
			{
				const bgAttributeFieldState_t& field = familyState.fields[ fieldIndex ];
				WriteValue( current, field, ReadValue( baseline, field ) );
			}
		}
	}
}

template<typename T>
void CaptureBaselinesForFamily( bgAttributeFamily_t family, T* baselines, int first, int last )
{
	for ( int i = first; i < last; ++i )
	{
		baselines[ i ] = *static_cast<T*>( CurrentObject( family, i - 1 ) );
	}
}

} // namespace

const char* BG_AttributeFamilyName( bgAttributeFamily_t family )
{
	return FamilyState( family ).name;
}

size_t BG_NumAttributeFields( bgAttributeFamily_t family )
{
	return FamilyState( family ).fieldCount;
}

const bgAttributeFieldInfo_t* BG_AttributeField( bgAttributeFamily_t family, size_t fieldIndex )
{
	const bgAttributeFieldState_t* field = FieldState( family, fieldIndex );
	return field ? &field->info : nullptr;
}

int BG_FindAttributeField( bgAttributeFamily_t family, const char* name )
{
	const bgAttributeFamilyState_t& state = FamilyState( family );
	for ( size_t i = 0; i < state.fieldCount; ++i )
	{
		if ( !Q_stricmp( state.fields[ i ].info.name, name ) )
		{
			return i;
		}
	}

	return -1;
}

size_t BG_NumAttributeObjects( bgAttributeFamily_t family )
{
	return FamilyState( family ).objectCount;
}

const char* BG_AttributeObjectName( bgAttributeFamily_t family, size_t objectIndex )
{
	if ( objectIndex >= BG_NumAttributeObjects( family ) )
	{
		return nullptr;
	}

	switch ( family )
	{
		case BG_ATTR_BUILDABLE: return BG_Buildable( objectIndex + 1 )->name;
		case BG_ATTR_CLASS: return BG_Class( objectIndex + 1 )->name;
		case BG_ATTR_WEAPON: return BG_Weapon( objectIndex + 1 )->name;
		case BG_ATTR_UPGRADE: return BG_Upgrade( objectIndex + 1 )->name;
		case BG_ATTR_MISSILE: return BG_Missile( objectIndex + 1 )->name;
		case BG_ATTR_BEACON: return BG_Beacon( objectIndex + 1 )->name;
		default: return nullptr;
	}
}

int BG_FindAttributeObject( bgAttributeFamily_t family, const char* name )
{
	for ( size_t i = 0; i < BG_NumAttributeObjects( family ); ++i )
	{
		if ( !Q_stricmp( BG_AttributeObjectName( family, i ), name ) )
		{
			return i;
		}
	}

	return -1;
}

bool BG_GetAttributeValue( bgAttributeFamily_t family, size_t objectIndex, size_t fieldIndex, bgAttributeValue_t* value, std::string* error )
{
	if ( !value || !ValidateIndices( family, objectIndex, fieldIndex, error ) )
	{
		return false;
	}

	*value = ReadValue( CurrentObject( family, objectIndex ), *FieldState( family, fieldIndex ) );
	return true;
}

bool BG_SetAttributeInt( bgAttributeFamily_t family, size_t objectIndex, size_t fieldIndex, int value, bool updateOverride, std::string* error )
{
	if ( !ValidateIndices( family, objectIndex, fieldIndex, error ) )
	{
		return false;
	}

	const bgAttributeFieldState_t& field = *FieldState( family, fieldIndex );
	if ( field.info.type != BG_ATTR_INTEGER )
	{
		SetError( error, "attribute field expects a different type" );
		return false;
	}

	bgAttributeValue_t wrapped{};
	wrapped.integer = value;
	WriteValue( CurrentObject( family, objectIndex ), field, wrapped );

	if ( updateOverride )
	{
		RefreshOverrideState( family, objectIndex, fieldIndex );
	}

	return true;
}

bool BG_SetAttributeFloat( bgAttributeFamily_t family, size_t objectIndex, size_t fieldIndex, float value, bool updateOverride, std::string* error )
{
	if ( !ValidateIndices( family, objectIndex, fieldIndex, error ) )
	{
		return false;
	}

	const bgAttributeFieldState_t& field = *FieldState( family, fieldIndex );
	if ( field.info.type != BG_ATTR_FLOAT )
	{
		SetError( error, "attribute field expects a different type" );
		return false;
	}

	bgAttributeValue_t wrapped{};
	wrapped.number = CanonicalizeFloat( value );
	WriteValue( CurrentObject( family, objectIndex ), field, wrapped );

	if ( updateOverride )
	{
		RefreshOverrideState( family, objectIndex, fieldIndex );
	}

	return true;
}

bool BG_SetAttributeBool( bgAttributeFamily_t family, size_t objectIndex, size_t fieldIndex, bool value, bool updateOverride, std::string* error )
{
	if ( !ValidateIndices( family, objectIndex, fieldIndex, error ) )
	{
		return false;
	}

	const bgAttributeFieldState_t& field = *FieldState( family, fieldIndex );
	if ( field.info.type != BG_ATTR_BOOL )
	{
		SetError( error, "attribute field expects a different type" );
		return false;
	}

	bgAttributeValue_t wrapped{};
	wrapped.boolean = value;
	WriteValue( CurrentObject( family, objectIndex ), field, wrapped );

	if ( updateOverride )
	{
		RefreshOverrideState( family, objectIndex, fieldIndex );
	}

	return true;
}

bool BG_ResetAttributeValue( bgAttributeFamily_t family, size_t objectIndex, size_t fieldIndex, bool updateOverride, std::string* error )
{
	if ( !ValidateIndices( family, objectIndex, fieldIndex, error ) )
	{
		return false;
	}

	const bgAttributeFieldState_t& field = *FieldState( family, fieldIndex );
	WriteValue( CurrentObject( family, objectIndex ), field, ReadValue( BaselineObject( family, objectIndex ), field ) );

	if ( updateOverride )
	{
		RefreshOverrideState( family, objectIndex, fieldIndex );
	}

	return true;
}

void BG_ResetAttributeFamilyOverrides( bgAttributeFamily_t family )
{
	EnsureOverrideStorage();

	const bgAttributeFamilyState_t& state = FamilyState( family );
	for ( size_t objectIndex = 0; objectIndex < state.objectCount; ++objectIndex )
	{
		for ( size_t fieldIndex = 0; fieldIndex < state.fieldCount; ++fieldIndex )
		{
			BG_ResetAttributeValue( family, objectIndex, fieldIndex, true, nullptr );
		}
	}
}

void BG_ResetAllAttributeOverrides()
{
	EnsureOverrideStorage();
	ResetTrackedFieldsToBaseline();

	for ( bgAttributeFamilyState_t& state : Families() )
	{
		for ( bgAttributeOverrideState_t& override : state.overrides )
		{
			override.present = false;
		}
	}
}

void BG_CommitAttributeBaselines()
{
	EnsureOverrideStorage();

	CaptureBaselinesForFamily( BG_ATTR_BUILDABLE, buildableBaselines, BA_NONE + 1, BA_NUM_BUILDABLES );
	CaptureBaselinesForFamily( BG_ATTR_CLASS, classBaselines, PCL_NONE + 1, PCL_NUM_CLASSES );
	CaptureBaselinesForFamily( BG_ATTR_WEAPON, weaponBaselines, WP_NONE + 1, WP_NUM_WEAPONS );
	CaptureBaselinesForFamily( BG_ATTR_UPGRADE, upgradeBaselines, UP_NONE + 1, UP_NUM_UPGRADES );
	CaptureBaselinesForFamily( BG_ATTR_MISSILE, missileBaselines, MIS_NONE + 1, MIS_NUM_MISSILES );
	CaptureBaselinesForFamily( BG_ATTR_BEACON, beaconBaselines, BCT_NONE + 1, NUM_BEACON_TYPES );

	for ( bgAttributeFamilyState_t& state : Families() )
	{
		for ( bgAttributeOverrideState_t& override : state.overrides )
		{
			override.present = false;
		}
	}
}

std::string BG_BuildAttributeConfig()
{
	EnsureOverrideStorage();

	std::ostringstream config;
	config.imbue( std::locale::classic() );
	config << std::setprecision( std::numeric_limits<float>::max_digits10 );

	size_t overrideCount = 0;
	for ( const bgAttributeFamilyState_t& state : Families() )
	{
		for ( const bgAttributeOverrideState_t& override : state.overrides )
		{
			if ( override.present )
			{
				++overrideCount;
			}
		}
	}

	config << ATTRIBUTE_CONFIG_SCHEMA << ' ' << overrideCount;

	for ( const bgAttributeFamilyState_t& state : Families() )
	{
		for ( size_t objectIndex = 0; objectIndex < state.objectCount; ++objectIndex )
		{
			for ( size_t fieldIndex = 0; fieldIndex < state.fieldCount; ++fieldIndex )
			{
				const bgAttributeOverrideState_t& override = OverrideState( state.family, objectIndex, fieldIndex );
				if ( !override.present )
				{
					continue;
				}

				config << ' ' << state.family << ' ' << objectIndex << ' ' << fieldIndex;
				switch ( state.fields[ fieldIndex ].info.type )
				{
					case BG_ATTR_INTEGER: config << ' ' << override.value.integer; break;
					case BG_ATTR_FLOAT: config << ' ' << override.value.number; break;
					case BG_ATTR_BOOL: config << ' ' << ( override.value.boolean ? 1 : 0 ); break;
				}
			}
		}
	}

	return config.str();
}

bool BG_ApplyAttributeConfig( const char* config, std::string* error )
{
	EnsureOverrideStorage();
	BG_ResetAllAttributeOverrides();

	if ( !config || !*config )
	{
		return true;
	}

	std::istringstream stream( config );
	stream.imbue( std::locale::classic() );

	int schema = 0;
	size_t overrideCount = 0;

	if ( !( stream >> schema >> overrideCount ) )
	{
		SetError( error, "malformed attribute config" );
		return false;
	}

	if ( schema != ATTRIBUTE_CONFIG_SCHEMA )
	{
		SetError( error, "unsupported attribute config schema" );
		return false;
	}

	for ( size_t i = 0; i < overrideCount; ++i )
	{
		int familyInt = 0;
		size_t objectIndex = 0;
		size_t fieldIndex = 0;

		if ( !( stream >> familyInt >> objectIndex >> fieldIndex ) )
		{
			SetError( error, "malformed attribute override header" );
			return false;
		}

		if ( familyInt < 0 || familyInt >= BG_NUM_ATTRIBUTE_FAMILIES )
		{
			SetError( error, "invalid attribute override family" );
			return false;
		}

		const bgAttributeFamily_t family = static_cast<bgAttributeFamily_t>( familyInt );
		if ( !ValidateIndices( family, objectIndex, fieldIndex, error ) )
		{
			return false;
		}

		const bgAttributeFieldState_t& field = *FieldState( family, fieldIndex );
		bgAttributeValue_t value{};

		switch ( field.info.type )
		{
			case BG_ATTR_INTEGER:
				if ( !( stream >> value.integer ) )
				{
					SetError( error, "malformed integer attribute override" );
					return false;
				}
				WriteValue( CurrentObject( family, objectIndex ), field, value );
				break;

			case BG_ATTR_FLOAT:
				if ( !( stream >> value.number ) )
				{
					SetError( error, "malformed float attribute override" );
					return false;
				}
				WriteValue( CurrentObject( family, objectIndex ), field, value );
				break;

			case BG_ATTR_BOOL:
			{
				int boolean = 0;
				if ( !( stream >> boolean ) )
				{
					SetError( error, "malformed bool attribute override" );
					return false;
				}
				value.boolean = boolean != 0;
				WriteValue( CurrentObject( family, objectIndex ), field, value );
				break;
			}
		}

		bgAttributeOverrideState_t& override = OverrideState( family, objectIndex, fieldIndex );
		override.present = true;
		override.value = value;
	}

	return true;
}

#ifdef BUILD_SGAME
void BG_PublishAttributeConfig()
{
	const std::string config = BG_BuildAttributeConfig();
	if ( config.size() >= BIG_INFO_STRING )
	{
		Sys::Drop( "attribute config too large to publish (%zu bytes, max %zu)",
		           config.size(), BIG_INFO_STRING - 1 );
	}
	trap_SetConfigstring( CS_ATTRIBUTES, config.c_str() );
}
#endif
