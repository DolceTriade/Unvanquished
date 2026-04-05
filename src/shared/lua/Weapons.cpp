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
#include "common/Common.h"
#include "shared/lua/Weapons.h"
#include "shared/bg_attributes.h"
#include "shared/lua/Utils.h"

namespace Shared {
namespace Lua {

#define GETTER(name) { #name, Get##name }

namespace {

#define ATTR_INT(struct_type, lua_name, member) { { lua_name, BG_ATTR_INTEGER }, offsetof( struct_type, member ) }
#define ATTR_FLOAT(struct_type, lua_name, member) { { lua_name, BG_ATTR_FLOAT }, offsetof( struct_type, member ) }
#define ATTR_BOOL(struct_type, lua_name, member) { { lua_name, BG_ATTR_BOOL }, offsetof( struct_type, member ) }

const bgAttributeTrackedField_t weaponAttributeFields[] =
{
	ATTR_INT( weaponAttributes_t, "price", price ),
	ATTR_INT( weaponAttributes_t, "unlock_threshold", unlockThreshold ),
	ATTR_INT( weaponAttributes_t, "slots", slots ),
	ATTR_INT( weaponAttributes_t, "ammo", maxAmmo ),
	ATTR_INT( weaponAttributes_t, "clips", maxClips ),
	ATTR_BOOL( weaponAttributes_t, "infinite_ammo", infiniteAmmo ),
	ATTR_BOOL( weaponAttributes_t, "energy", usesEnergy ),
	ATTR_INT( weaponAttributes_t, "repeat_rate1", repeatRate1 ),
	ATTR_INT( weaponAttributes_t, "repeat_rate2", repeatRate2 ),
	ATTR_INT( weaponAttributes_t, "repeat_rate3", repeatRate3 ),
	ATTR_INT( weaponAttributes_t, "reload_time", reloadTime ),
	ATTR_BOOL( weaponAttributes_t, "alt_mode", hasAltMode ),
	ATTR_BOOL( weaponAttributes_t, "zoom", canZoom ),
	ATTR_BOOL( weaponAttributes_t, "purchasable", purchasable ),
	ATTR_BOOL( weaponAttributes_t, "long_ranged", longRanged ),
};

#undef ATTR_INT
#undef ATTR_FLOAT
#undef ATTR_BOOL

} // namespace

const bgAttributeTrackedField_t* WeaponAttributeFields()
{
	return weaponAttributeFields;
}

size_t NumWeaponAttributeFields()
{
	return ARRAY_LEN( weaponAttributeFields );
}

WeaponProxy::WeaponProxy( int weapon ) :
	weapon( weapon ),
	attributes( BG_Weapon( weapon ) ) {}

#define GET_FUNC( var, type ) \
static int Get##var( lua_State* L ) \
{ \
	WeaponProxy* proxy = LuaLib<WeaponProxy>::check( L, 1 ); \
	lua_push##type( L, proxy->attributes->var ); \
	return 1; \
}

#define GET_FUNC2( name, var, type ) \
static int Get##name( lua_State* L ) \
{ \
	WeaponProxy* proxy = LuaLib<WeaponProxy>::check( L, 1 ); \
	lua_push##type( L, var ); \
	return 1; \
}

GET_FUNC( price, integer )
GET_FUNC2( unlock_threshold, proxy->attributes->unlockThreshold, integer )
GET_FUNC2( name, proxy->attributes->humanName, string )
GET_FUNC( info, string )
GET_FUNC( slots, integer )
GET_FUNC2( ammo, proxy->attributes->maxAmmo, integer )
GET_FUNC2( clips, proxy->attributes->maxClips, integer )
GET_FUNC2( infinite_ammo, proxy->attributes->infiniteAmmo, boolean )
GET_FUNC2( energy, proxy->attributes->usesEnergy, boolean )
GET_FUNC2( repeat_rate1, proxy->attributes->repeatRate1, integer )
GET_FUNC2( repeat_rate2, proxy->attributes->repeatRate2, integer )
GET_FUNC2( repeat_rate3, proxy->attributes->repeatRate3, integer )
GET_FUNC2( reload_time, proxy->attributes->reloadTime, integer )
GET_FUNC2( alt_mode, proxy->attributes->hasAltMode, boolean )
GET_FUNC2( zoom, proxy->attributes->canZoom, boolean )
GET_FUNC( purchasable, boolean )
GET_FUNC2( long_ranged, proxy->attributes->longRanged, boolean )
GET_FUNC2( team, BG_TeamName( proxy->attributes->team ), string )

#define SET_INT(name, field_name) \
static int Set##name( lua_State* L ) \
{ \
	WeaponProxy* proxy = LuaLib<WeaponProxy>::check( L, 1 ); \
	return proxy ? SetAttributeInt( L, BG_ATTR_WEAPON, proxy->weapon - 1, "weapon", field_name ) : 0; \
}

#define SET_BOOL(name, field_name) \
static int Set##name( lua_State* L ) \
{ \
	WeaponProxy* proxy = LuaLib<WeaponProxy>::check( L, 1 ); \
	return proxy ? SetAttributeBool( L, BG_ATTR_WEAPON, proxy->weapon - 1, "weapon", field_name ) : 0; \
}

SET_INT(price, "price")
SET_INT(unlock_threshold, "unlock_threshold")
SET_INT(slots, "slots")
SET_INT(ammo, "ammo")
SET_INT(clips, "clips")
SET_INT(repeat_rate1, "repeat_rate1")
SET_INT(repeat_rate2, "repeat_rate2")
SET_INT(repeat_rate3, "repeat_rate3")
SET_INT(reload_time, "reload_time")
SET_BOOL(infinite_ammo, "infinite_ammo")
SET_BOOL(energy, "energy")
SET_BOOL(alt_mode, "alt_mode")
SET_BOOL(zoom, "zoom")
SET_BOOL(purchasable, "purchasable")
SET_BOOL(long_ranged, "long_ranged")

#undef SET_INT
#undef SET_BOOL

static int Methodreset( lua_State* L, WeaponProxy* proxy )
{
	const char* fieldName = luaL_checkstring( L, 1 );
	return ResetAttribute( L, BG_ATTR_WEAPON, proxy->weapon - 1, "weapon", fieldName );
}

template<> void ExtraInit<WeaponProxy>( lua_State* /*L*/, int /*metatable_index*/ ) {}

RegType<WeaponProxy> WeaponProxyMethods[] =
{
	{ "reset", Methodreset },
	{ nullptr, nullptr },
};

luaL_Reg WeaponProxyGetters[] =
{
	GETTER(price),
	GETTER(unlock_threshold),
	GETTER(name),
	GETTER(info),
	GETTER(slots),
	GETTER(ammo),
	GETTER(clips),
	GETTER(infinite_ammo),
	GETTER(energy),
	GETTER(repeat_rate1),
	GETTER(repeat_rate2),
	GETTER(repeat_rate3),
	GETTER(reload_time),
	GETTER(alt_mode),
	GETTER(zoom),
	GETTER(purchasable),
	GETTER(long_ranged),
	GETTER(team),

	{ nullptr, nullptr },
};

luaL_Reg WeaponProxySetters[] =
{
	{ "price", Setprice },
	{ "unlock_threshold", Setunlock_threshold },
	{ "slots", Setslots },
	{ "ammo", Setammo },
	{ "clips", Setclips },
	{ "infinite_ammo", Setinfinite_ammo },
	{ "energy", Setenergy },
	{ "repeat_rate1", Setrepeat_rate1 },
	{ "repeat_rate2", Setrepeat_rate2 },
	{ "repeat_rate3", Setrepeat_rate3 },
	{ "reload_time", Setreload_time },
	{ "alt_mode", Setalt_mode },
	{ "zoom", Setzoom },
	{ "purchasable", Setpurchasable },
	{ "long_ranged", Setlong_ranged },
	{ nullptr, nullptr },
};

LUACORETYPEDEFINE(WeaponProxy)

int Weapons::index( lua_State* L )
{
	const char *weaponName = luaL_checkstring( L, -1 );
	weapon_t weapon = BG_WeaponNumberByName( weaponName );
	if ( weapon > 0 && static_cast<size_t>( weapon ) - 1 < weapons.size() )
	{
		LuaLib<WeaponProxy>::push( L, &weapons[ weapon - 1 ] );
		return 1;
	}
	return 0;
}

int Weapons::reset( lua_State* L, Weapons* /*self*/ )
{
	const char* weaponName = luaL_checkstring( L, 1 );
	const char* fieldName = luaL_checkstring( L, 2 );
	int objectIndex = BG_FindAttributeObject( BG_ATTR_WEAPON, weaponName );
	int field = BG_FindAttributeField( BG_ATTR_WEAPON, fieldName );
	if ( objectIndex < 0 )
	{
		return luaL_error( L, "unknown weapon '%s'", weaponName );
	}
	if ( field < 0 )
	{
		return luaL_error( L, "unknown weapon field '%s'", fieldName );
	}

	return ResetAttribute( L, BG_ATTR_WEAPON, objectIndex, "weapon", fieldName );
}

int Weapons::reset_all( lua_State* L, Weapons* /*self*/ )
{
	return ResetAttributeFamily( L, BG_ATTR_WEAPON, "weapon" );
}

int Weapons::pairs( lua_State* L )
{
	return CreatePairsHelper( L, [](lua_State* L, size_t i ) {
		if ( i >= weapons.size() )
		{
			return 0;
		}
		lua_pushstring( L, weapons[ i ].attributes->name );
		LuaLib<WeaponProxy>::push( L, &weapons[ i ] );
		return 2;
	});
}

std::vector<WeaponProxy> Weapons::weapons;

template<> void ExtraInit<Weapons>( lua_State* L, int metatable_index )
{
	// overwrite index function
	lua_pushcfunction( L, Weapons::index );
	lua_setfield( L, metatable_index, "__index" );
	lua_pushcfunction( L, Weapons::pairs );
	lua_setfield( L, metatable_index, "__pairs" );

	for ( int i = WP_NONE + 1; i < WP_NUM_WEAPONS; ++i)
	{
		Weapons::weapons.push_back( WeaponProxy( i ) );
	}
}

RegType<Weapons> WeaponsMethods[] =
{
	{ "reset", Weapons::reset },
	{ "reset_all", Weapons::reset_all },
	{ nullptr, nullptr },
};

luaL_Reg WeaponsGetters[] =
{
	{ nullptr, nullptr },
};

luaL_Reg WeaponsSetters[] =
{
	{ nullptr, nullptr },
};

LUACORETYPEDEFINE(Weapons)

}  // namespace Lua
}  // namespace Shared
