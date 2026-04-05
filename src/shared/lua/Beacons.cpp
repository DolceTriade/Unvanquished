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

#include "shared/lua/Beacons.h"
#include "shared/bg_attributes.h"
#include "shared/lua/Utils.h"

namespace Shared {
namespace Lua {

#define GETTER(name) { #name, Get##name }

namespace {

#define ATTR_INT(struct_type, lua_name, member) { { lua_name, BG_ATTR_INTEGER }, offsetof( struct_type, member ) }
#define ATTR_FLOAT(struct_type, lua_name, member) { { lua_name, BG_ATTR_FLOAT }, offsetof( struct_type, member ) }
#define ATTR_BOOL(struct_type, lua_name, member) { { lua_name, BG_ATTR_BOOL }, offsetof( struct_type, member ) }

const bgAttributeTrackedField_t beaconAttributeFields[] =
{
	ATTR_INT( beaconAttributes_t, "decay_time", decayTime ),
};

#undef ATTR_INT
#undef ATTR_FLOAT
#undef ATTR_BOOL

} // namespace

const bgAttributeTrackedField_t* BeaconAttributeFields()
{
	return beaconAttributeFields;
}

size_t NumBeaconAttributeFields()
{
	return ARRAY_LEN( beaconAttributeFields );
}

BeaconProxy::BeaconProxy( int beacon ) :
	beacon( beacon ),
	attributes( BG_Beacon( beacon ) ) {}

#define GET_FUNC(var, type) \
static int Get##var( lua_State* L ) \
{ \
	BeaconProxy* proxy = LuaLib<BeaconProxy>::check( L, 1 ); \
	lua_push##type( L, proxy->attributes->var ); \
	return 1; \
}

#define GET_FUNC2(name, expr, type) \
static int Get##name( lua_State* L ) \
{ \
	BeaconProxy* proxy = LuaLib<BeaconProxy>::check( L, 1 ); \
	lua_push##type( L, expr ); \
	return 1; \
}

GET_FUNC( name, string )
GET_FUNC( humanName, string )
GET_FUNC2( decay_time, proxy->attributes->decayTime, integer )

static int Setdecay_time( lua_State* L )
{
	BeaconProxy* proxy = LuaLib<BeaconProxy>::check( L, 1 );
	return proxy ? SetAttributeInt( L, BG_ATTR_BEACON, proxy->beacon - 1, "beacon", "decay_time" ) : 0;
}

static int Methodreset( lua_State* L, BeaconProxy* proxy )
{
	const char* fieldName = luaL_checkstring( L, 1 );
	return ResetAttribute( L, BG_ATTR_BEACON, proxy->beacon - 1, "beacon", fieldName );
}

template<> void ExtraInit<BeaconProxy>( lua_State* /*L*/, int /*metatable_index*/ ) {}

RegType<BeaconProxy> BeaconProxyMethods[] =
{
	{ "reset", Methodreset },
	{ nullptr, nullptr },
};

luaL_Reg BeaconProxyGetters[] =
{
	GETTER(name),
	GETTER(humanName),
	GETTER(decay_time),
	{ nullptr, nullptr },
};

luaL_Reg BeaconProxySetters[] =
{
	{ "decay_time", Setdecay_time },
	{ nullptr, nullptr },
};

LUACORETYPEDEFINE(BeaconProxy)

int Beacons::index( lua_State* L )
{
	const char* beaconName = luaL_checkstring( L, -1 );
	beaconType_t beacon = BG_BeaconByName( beaconName )->number;
	if ( beacon > 0 && static_cast<size_t>( beacon ) - 1 < beacons.size() )
	{
		LuaLib<BeaconProxy>::push( L, &beacons[ beacon - 1 ] );
		return 1;
	}
	return 0;
}

int Beacons::pairs( lua_State* L )
{
	return CreatePairsHelper( L, []( lua_State* L, size_t i ) {
		if ( i >= beacons.size() )
		{
			return 0;
		}
		lua_pushstring( L, beacons[ i ].attributes->name );
		LuaLib<BeaconProxy>::push( L, &beacons[ i ] );
		return 2;
	} );
}

int Beacons::reset( lua_State* L, Beacons* /*self*/ )
{
	const char* beaconName = luaL_checkstring( L, 1 );
	const char* fieldName = luaL_checkstring( L, 2 );
	int objectIndex = BG_FindAttributeObject( BG_ATTR_BEACON, beaconName );
	int field = BG_FindAttributeField( BG_ATTR_BEACON, fieldName );
	if ( objectIndex < 0 ) return luaL_error( L, "unknown beacon '%s'", beaconName );
	if ( field < 0 ) return luaL_error( L, "unknown beacon field '%s'", fieldName );

	return ResetAttribute( L, BG_ATTR_BEACON, objectIndex, "beacon", fieldName );
}

int Beacons::reset_all( lua_State* L, Beacons* /*self*/ )
{
	return ResetAttributeFamily( L, BG_ATTR_BEACON, "beacon" );
}

std::vector<BeaconProxy> Beacons::beacons;

template<> void ExtraInit<Beacons>( lua_State* L, int metatable_index )
{
	lua_pushcfunction( L, Beacons::index );
	lua_setfield( L, metatable_index, "__index" );
	lua_pushcfunction( L, Beacons::pairs );
	lua_setfield( L, metatable_index, "__pairs" );

	for ( int i = BCT_NONE + 1; i < NUM_BEACON_TYPES; ++i )
	{
		Beacons::beacons.push_back( BeaconProxy( i ) );
	}
}

RegType<Beacons> BeaconsMethods[] =
{
	{ "reset", Beacons::reset },
	{ "reset_all", Beacons::reset_all },
	{ nullptr, nullptr },
};

luaL_Reg BeaconsGetters[] =
{
	{ nullptr, nullptr },
};

luaL_Reg BeaconsSetters[] =
{
	{ nullptr, nullptr },
};

LUACORETYPEDEFINE(Beacons)

} // namespace Lua
} // namespace Shared
