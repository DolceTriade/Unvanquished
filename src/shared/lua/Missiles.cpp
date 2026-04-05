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

#include "shared/lua/Missiles.h"
#include "shared/bg_attributes.h"
#include "shared/lua/Utils.h"

namespace Shared {
namespace Lua {

#define GETTER(name) { #name, Get##name }

namespace {

#define ATTR_INT(struct_type, lua_name, member) { { lua_name, BG_ATTR_INTEGER }, offsetof( struct_type, member ) }
#define ATTR_FLOAT(struct_type, lua_name, member) { { lua_name, BG_ATTR_FLOAT }, offsetof( struct_type, member ) }
#define ATTR_BOOL(struct_type, lua_name, member) { { lua_name, BG_ATTR_BOOL }, offsetof( struct_type, member ) }

const bgAttributeTrackedField_t missileAttributeFields[] =
{
	ATTR_BOOL( missileAttributes_t, "point_against_world", pointAgainstWorld ),
	ATTR_INT( missileAttributes_t, "damage", damage ),
	ATTR_INT( missileAttributes_t, "splash_damage", splashDamage ),
	ATTR_INT( missileAttributes_t, "splash_radius", splashRadius ),
	ATTR_INT( missileAttributes_t, "size", size ),
	ATTR_INT( missileAttributes_t, "speed", speed ),
	ATTR_FLOAT( missileAttributes_t, "lag", lag ),
	ATTR_INT( missileAttributes_t, "steering_period", steeringPeriod ),
	ATTR_INT( missileAttributes_t, "lifetime", lifetime ),
	ATTR_BOOL( missileAttributes_t, "life_end_explode", lifeEndExplode ),
	ATTR_BOOL( missileAttributes_t, "do_knockback", doKnockback ),
	ATTR_BOOL( missileAttributes_t, "do_locational_damage", doLocationalDamage ),
};

#undef ATTR_INT
#undef ATTR_FLOAT
#undef ATTR_BOOL

} // namespace

const bgAttributeTrackedField_t* MissileAttributeFields()
{
	return missileAttributeFields;
}

size_t NumMissileAttributeFields()
{
	return ARRAY_LEN( missileAttributeFields );
}

MissileProxy::MissileProxy( int missile ) :
	missile( missile ),
	attributes( BG_Missile( missile ) ) {}

#define GET_FUNC(var, type) \
static int Get##var( lua_State* L ) \
{ \
	MissileProxy* proxy = LuaLib<MissileProxy>::check( L, 1 ); \
	lua_push##type( L, proxy->attributes->var ); \
	return 1; \
}

#define GET_FUNC2(name, expr, type) \
static int Get##name( lua_State* L ) \
{ \
	MissileProxy* proxy = LuaLib<MissileProxy>::check( L, 1 ); \
	lua_push##type( L, expr ); \
	return 1; \
}

GET_FUNC( name, string )
GET_FUNC2( point_against_world, proxy->attributes->pointAgainstWorld, boolean )
GET_FUNC( damage, integer )
GET_FUNC2( splash_damage, proxy->attributes->splashDamage, integer )
GET_FUNC2( splash_radius, proxy->attributes->splashRadius, integer )
GET_FUNC( size, integer )
GET_FUNC( speed, integer )
GET_FUNC( lag, number )
GET_FUNC2( steering_period, proxy->attributes->steeringPeriod, integer )
GET_FUNC( lifetime, integer )
GET_FUNC2( life_end_explode, proxy->attributes->lifeEndExplode, boolean )
GET_FUNC2( do_knockback, proxy->attributes->doKnockback, boolean )
GET_FUNC2( do_locational_damage, proxy->attributes->doLocationalDamage, boolean )

#define SET_INT(name, field_name) \
static int Set##name( lua_State* L ) \
{ \
	MissileProxy* proxy = LuaLib<MissileProxy>::check( L, 1 ); \
	return proxy ? SetAttributeInt( L, BG_ATTR_MISSILE, proxy->missile - 1, "missile", field_name ) : 0; \
}

#define SET_FLOAT(name, field_name) \
static int Set##name( lua_State* L ) \
{ \
	MissileProxy* proxy = LuaLib<MissileProxy>::check( L, 1 ); \
	return proxy ? SetAttributeFloat( L, BG_ATTR_MISSILE, proxy->missile - 1, "missile", field_name ) : 0; \
}

#define SET_BOOL(name, field_name) \
static int Set##name( lua_State* L ) \
{ \
	MissileProxy* proxy = LuaLib<MissileProxy>::check( L, 1 ); \
	return proxy ? SetAttributeBool( L, BG_ATTR_MISSILE, proxy->missile - 1, "missile", field_name ) : 0; \
}

SET_BOOL(point_against_world, "point_against_world")
SET_INT(damage, "damage")
SET_INT(splash_damage, "splash_damage")
SET_INT(splash_radius, "splash_radius")
SET_INT(size, "size")
SET_INT(speed, "speed")
SET_FLOAT(lag, "lag")
SET_INT(steering_period, "steering_period")
SET_INT(lifetime, "lifetime")
SET_BOOL(life_end_explode, "life_end_explode")
SET_BOOL(do_knockback, "do_knockback")
SET_BOOL(do_locational_damage, "do_locational_damage")

#undef SET_INT
#undef SET_FLOAT
#undef SET_BOOL

static int Methodreset( lua_State* L, MissileProxy* proxy )
{
	const char* fieldName = luaL_checkstring( L, 1 );
	return ResetAttribute( L, BG_ATTR_MISSILE, proxy->missile - 1, "missile", fieldName );
}

template<> void ExtraInit<MissileProxy>( lua_State* /*L*/, int /*metatable_index*/ ) {}

RegType<MissileProxy> MissileProxyMethods[] =
{
	{ "reset", Methodreset },
	{ nullptr, nullptr },
};

luaL_Reg MissileProxyGetters[] =
{
	GETTER(name),
	GETTER(point_against_world),
	GETTER(damage),
	GETTER(splash_damage),
	GETTER(splash_radius),
	GETTER(size),
	GETTER(speed),
	GETTER(lag),
	GETTER(steering_period),
	GETTER(lifetime),
	GETTER(life_end_explode),
	GETTER(do_knockback),
	GETTER(do_locational_damage),
	{ nullptr, nullptr },
};

luaL_Reg MissileProxySetters[] =
{
	{ "point_against_world", Setpoint_against_world },
	{ "damage", Setdamage },
	{ "splash_damage", Setsplash_damage },
	{ "splash_radius", Setsplash_radius },
	{ "size", Setsize },
	{ "speed", Setspeed },
	{ "lag", Setlag },
	{ "steering_period", Setsteering_period },
	{ "lifetime", Setlifetime },
	{ "life_end_explode", Setlife_end_explode },
	{ "do_knockback", Setdo_knockback },
	{ "do_locational_damage", Setdo_locational_damage },
	{ nullptr, nullptr },
};

LUACORETYPEDEFINE(MissileProxy)

int Missiles::index( lua_State* L )
{
	const char* missileName = luaL_checkstring( L, -1 );
	missile_t missile = BG_MissileByName( missileName )->number;
	if ( missile > 0 && static_cast<size_t>( missile ) - 1 < missiles.size() )
	{
		LuaLib<MissileProxy>::push( L, &missiles[ missile - 1 ] );
		return 1;
	}
	return 0;
}

int Missiles::pairs( lua_State* L )
{
	return CreatePairsHelper( L, []( lua_State* L, size_t i ) {
		if ( i >= missiles.size() )
		{
			return 0;
		}
		lua_pushstring( L, missiles[ i ].attributes->name );
		LuaLib<MissileProxy>::push( L, &missiles[ i ] );
		return 2;
	} );
}

int Missiles::reset( lua_State* L, Missiles* /*self*/ )
{
	const char* missileName = luaL_checkstring( L, 1 );
	const char* fieldName = luaL_checkstring( L, 2 );
	int objectIndex = BG_FindAttributeObject( BG_ATTR_MISSILE, missileName );
	int field = BG_FindAttributeField( BG_ATTR_MISSILE, fieldName );
	if ( objectIndex < 0 ) return luaL_error( L, "unknown missile '%s'", missileName );
	if ( field < 0 ) return luaL_error( L, "unknown missile field '%s'", fieldName );

	return ResetAttribute( L, BG_ATTR_MISSILE, objectIndex, "missile", fieldName );
}

int Missiles::reset_all( lua_State* L, Missiles* /*self*/ )
{
	return ResetAttributeFamily( L, BG_ATTR_MISSILE, "missile" );
}

std::vector<MissileProxy> Missiles::missiles;

template<> void ExtraInit<Missiles>( lua_State* L, int metatable_index )
{
	lua_pushcfunction( L, Missiles::index );
	lua_setfield( L, metatable_index, "__index" );
	lua_pushcfunction( L, Missiles::pairs );
	lua_setfield( L, metatable_index, "__pairs" );

	for ( int i = MIS_NONE + 1; i < MIS_NUM_MISSILES; ++i )
	{
		Missiles::missiles.push_back( MissileProxy( i ) );
	}
}

RegType<Missiles> MissilesMethods[] =
{
	{ "reset", Missiles::reset },
	{ "reset_all", Missiles::reset_all },
	{ nullptr, nullptr },
};

luaL_Reg MissilesGetters[] =
{
	{ nullptr, nullptr },
};

luaL_Reg MissilesSetters[] =
{
	{ nullptr, nullptr },
};

LUACORETYPEDEFINE(Missiles)

} // namespace Lua
} // namespace Shared
