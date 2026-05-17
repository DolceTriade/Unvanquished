/*
===========================================================================

Unvanquished GPL Source Code
Copyright (C) 2024 Unvanquished Developers

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

In addition, the Unvanquished Source Code is also subject to certain additional terms.
You should have received a copy of these additional terms immediately following the
terms and conditions of the GNU General Public License which accompanied the Unvanquished
Source Code.  If not, please request a copy in writing from id Software at the address
below.

If you have questions concerning this license or the applicable additional terms, you
may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville,
Maryland 20850 USA.

===========================================================================
*/
#include "shared/lua/Classes.h"
#include "shared/bg_attributes.h"
#include "shared/lua/Utils.h"

namespace Shared {
namespace Lua {

#define GETTER(name) { #name, Get##name }

namespace {

#define ATTR_INT(struct_type, lua_name, member) { { lua_name, BG_ATTR_INTEGER }, offsetof( struct_type, member ) }
#define ATTR_FLOAT(struct_type, lua_name, member) { { lua_name, BG_ATTR_FLOAT }, offsetof( struct_type, member ) }
#define ATTR_BOOL(struct_type, lua_name, member) { { lua_name, BG_ATTR_BOOL }, offsetof( struct_type, member ) }

const bgAttributeTrackedField_t classAttributeFields[] =
{
	ATTR_INT( classAttributes_t, "unlock_threshold", unlockThreshold ),
	ATTR_INT( classAttributes_t, "health", health ),
	ATTR_INT( classAttributes_t, "staminaJogRestore", staminaJogRestore ),
	ATTR_INT( classAttributes_t, "staminaWalkRestore", staminaWalkRestore ),
	ATTR_INT( classAttributes_t, "staminaStopRestore", staminaStopRestore ),
	ATTR_FLOAT( classAttributes_t, "regen_rate", regenRate ),
	ATTR_FLOAT( classAttributes_t, "speed", speed ),
	ATTR_INT( classAttributes_t, "mass", mass ),
	ATTR_FLOAT( classAttributes_t, "jump_magnitude", jumpMagnitude ),
	ATTR_INT( classAttributes_t, "price", price ),
};

#undef ATTR_INT
#undef ATTR_FLOAT
#undef ATTR_BOOL

} // namespace

const bgAttributeTrackedField_t* ClassAttributeFields()
{
	return classAttributeFields;
}

size_t NumClassAttributeFields()
{
	return ARRAY_LEN( classAttributeFields );
}

ClassProxy::ClassProxy( int clazz ) :
	clazz( clazz ),
	attributes( BG_Class( clazz ) ) {}

#define GET_FUNC( var, type ) \
static int Get##var( lua_State* L ) \
{ \
	ClassProxy* proxy = LuaLib<ClassProxy>::check( L, 1 ); \
	lua_push##type( L, proxy->attributes->var ); \
	return 1; \
}

#define GET_FUNC2( name, var, type ) \
static int Get##name( lua_State* L ) \
{ \
	ClassProxy* proxy = LuaLib<ClassProxy>::check( L, 1 ); \
	lua_push##type( L, var ); \
	return 1; \
}

GET_FUNC( name, string )
GET_FUNC( info, string )
GET_FUNC( icon, string )
GET_FUNC2( fov_cvar, proxy->attributes->fovCvar, string )
GET_FUNC2( unlock_threshold, proxy->attributes->unlockThreshold, integer )
GET_FUNC( health, integer )
GET_FUNC( staminaJogRestore, integer )
GET_FUNC( staminaWalkRestore, integer )
GET_FUNC( staminaStopRestore, integer )
GET_FUNC2( regen_rate, proxy->attributes->regenRate, number )
GET_FUNC( speed, number )
GET_FUNC( mass, integer )
GET_FUNC2( jump_magnitude, proxy->attributes->jumpMagnitude, number )
GET_FUNC( price, integer )
GET_FUNC2( team, BG_TeamName( proxy->attributes->team ), string )

#define SET_INT(name, field_name) \
static int Set##name( lua_State* L ) \
{ \
	ClassProxy* proxy = LuaLib<ClassProxy>::check( L, 1 ); \
	return proxy ? SetAttributeInt( L, BG_ATTR_CLASS, proxy->clazz - 1, "class", field_name ) : 0; \
}

#define SET_FLOAT(name, field_name) \
static int Set##name( lua_State* L ) \
{ \
	ClassProxy* proxy = LuaLib<ClassProxy>::check( L, 1 ); \
	return proxy ? SetAttributeFloat( L, BG_ATTR_CLASS, proxy->clazz - 1, "class", field_name ) : 0; \
}

SET_INT(unlock_threshold, "unlock_threshold")
SET_INT(health, "health")
SET_INT(staminaJogRestore, "staminaJogRestore")
SET_INT(staminaWalkRestore, "staminaWalkRestore")
SET_INT(staminaStopRestore, "staminaStopRestore")
SET_FLOAT(regen_rate, "regen_rate")
SET_FLOAT(speed, "speed")
SET_INT(mass, "mass")
SET_FLOAT(jump_magnitude, "jump_magnitude")
SET_INT(price, "price")

#undef SET_INT
#undef SET_FLOAT

static int Methodreset( lua_State* L, ClassProxy* proxy )
{
	const char* fieldName = luaL_checkstring( L, 1 );
	return ResetAttribute( L, BG_ATTR_CLASS, proxy->clazz - 1, "class", fieldName );
}

template<> void ExtraInit<ClassProxy>( lua_State* /*L*/, int /*metatable_index*/ ) {}

RegType<ClassProxy> ClassProxyMethods[] =
{
	{ "reset", Methodreset },
	{ nullptr, nullptr },
};

luaL_Reg ClassProxyGetters[] =
{
	GETTER(name),
	GETTER(info),
	GETTER(icon),
	GETTER(fov_cvar),
	GETTER(team),
	GETTER(unlock_threshold),
	GETTER(health),
	GETTER(staminaJogRestore),
	GETTER(staminaWalkRestore),
	GETTER(staminaStopRestore),
	GETTER(regen_rate),
	GETTER(speed),
	GETTER(mass),
	GETTER(jump_magnitude),
	GETTER(price),

	{ nullptr, nullptr }
};

luaL_Reg ClassProxySetters[] =
{
	{ "unlock_threshold", Setunlock_threshold },
	{ "health", Sethealth },
	{ "staminaJogRestore", SetstaminaJogRestore },
	{ "staminaWalkRestore", SetstaminaWalkRestore },
	{ "staminaStopRestore", SetstaminaStopRestore },
	{ "regen_rate", Setregen_rate },
	{ "speed", Setspeed },
	{ "mass", Setmass },
	{ "jump_magnitude", Setjump_magnitude },
	{ "price", Setprice },
	{ nullptr, nullptr },
};

LUACORETYPEDEFINE(ClassProxy)

int Classes::index( lua_State* L )
{
	const char *className = luaL_checkstring( L, -1 );
	class_t clazz = BG_ClassByName( className )->number;
	if ( clazz > 0 && static_cast<size_t>( clazz ) - 1 < classes.size() )
	{
		LuaLib<ClassProxy>::push( L, &classes[ clazz - 1 ] );
		return 1;
	}
	return 0;
}

int Classes::reset( lua_State* L, Classes* /*self*/ )
{
	const char* className = luaL_checkstring( L, 1 );
	const char* fieldName = luaL_checkstring( L, 2 );
	int objectIndex = BG_FindAttributeObject( BG_ATTR_CLASS, className );
	int field = BG_FindAttributeField( BG_ATTR_CLASS, fieldName );
	if ( objectIndex < 0 )
	{
		return luaL_error( L, "unknown class '%s'", className );
	}
	if ( field < 0 )
	{
		return luaL_error( L, "unknown class field '%s'", fieldName );
	}

	return ResetAttribute( L, BG_ATTR_CLASS, objectIndex, "class", fieldName );
}

int Classes::reset_all( lua_State* L, Classes* /*self*/ )
{
	return ResetAttributeFamily( L, BG_ATTR_CLASS, "class" );
}

int Classes::pairs( lua_State* L )
{
	return CreatePairsHelper( L, [](lua_State* L, size_t i ) {
		if ( i >= classes.size() )
		{
			return 0;
		}
		lua_pushstring( L, classes[ i ].attributes->name );
		LuaLib<ClassProxy>::push( L, &classes[ i ] );
		return 2;
	});
}

std::vector<ClassProxy> Classes::classes;

template<> void ExtraInit<Classes>( lua_State* L, int metatable_index )
{
	// overwrite index function
	lua_pushcfunction( L, Classes::index );
	lua_setfield( L, metatable_index, "__index" );
	lua_pushcfunction( L, Classes::pairs );
	lua_setfield( L, metatable_index, "__pairs" );

	for ( int i = PCL_NONE + 1; i < PCL_NUM_CLASSES; ++i)
	{
		Classes::classes.push_back( ClassProxy( i ) );
	}
}

RegType<Classes> ClassesMethods[] =
{
	{ "reset", Classes::reset },
	{ "reset_all", Classes::reset_all },
	{ nullptr, nullptr },
};

luaL_Reg ClassesGetters[] =
{
	{ nullptr, nullptr },
};

luaL_Reg ClassesSetters[] =
{
	{ nullptr, nullptr },
};

LUACORETYPEDEFINE(Classes)

} // namespace Lua
}  // namespace Shared
