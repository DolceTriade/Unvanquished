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

#include "sgame/lua/Missile.h"

#include "sgame/components/MissileComponent.h"
#include "sgame/lua/Entity.h"
#include "sgame/lua/EntityProxy.h"
#include "shared/lua/LuaLib.h"
#include "shared/lua/Utils.h"

using Shared::Lua::LuaLib;
using Shared::Lua::RegType;

namespace Lua {

Missile::Missile( EntityProxy* proxy )
	: proxy( proxy ), impactCallback( LUA_NOREF ), expireCallback( LUA_NOREF )
{}

int* Missile::CallbackRef( CallbackType type )
{
	switch ( type )
	{
		case IMPACT: return &impactCallback;
		case EXPIRE: return &expireCallback;
	}

	return nullptr;
}

void Missile::ClearCallbacks()
{
	if ( !proxy || !proxy->L ) return;

	for ( int* ref : { &impactCallback, &expireCallback } )
	{
		if ( *ref == LUA_NOREF ) continue;
		luaL_unref( proxy->L, LUA_REGISTRYINDEX, *ref );
		*ref = LUA_NOREF;
	}
}

bool HasMissileComponent( gentity_t* ent )
{
	return ent && ent->inuse && ent->entity && ent->entity->Get<MissileComponent>();
}

namespace {

#define GETTER( name ) \
	{ #name, Get##name }

#define SETTER( name ) \
	{ #name, Set##name }

static bool IsLiveMissile( Missile* missile )
{
	return missile && missile->proxy && missile->proxy->ent &&
	       missile->proxy->ent->generation == missile->proxy->generation &&
	       HasMissileComponent( missile->proxy->ent );
}

static const char* CallbackName( Missile::CallbackType type )
{
	switch ( type )
	{
		case Missile::IMPACT: return "impact";
		case Missile::EXPIRE: return "expire";
	}

	return "unknown";
}

static int GetCallback( lua_State* L, Missile::CallbackType type )
{
	Missile* missile = LuaLib<Missile>::check( L, 1 );
	if ( !IsLiveMissile( missile ) ) return 0;
	const int* ref = missile->CallbackRef( type );
	lua_pushboolean( L, ref && *ref != LUA_NOREF );
	return 1;
}

/// Missile current position. Array of floats starting at index 1.
// This uses the entity's current physics position rather than s.origin.
// @tfield array origin Read only.
// @within Missile
static int Getorigin( lua_State* L )
{
	Missile* missile = LuaLib<Missile>::check( L, 1 );
	if ( !IsLiveMissile( missile ) ) return 0;
	Shared::Lua::PushVec3( L, missile->proxy->ent->r.currentOrigin );
	return 1;
}

/// Missile type name.
// @tfield string type Read only.
// @within Missile
static int Gettype( lua_State* L )
{
	Missile* missile = LuaLib<Missile>::check( L, 1 );
	if ( !IsLiveMissile( missile ) ) return 0;
	lua_pushstring( L, missile->proxy->ent->entity->Get<MissileComponent>()->Attributes().name );
	return 1;
}

/// Missile parent entity.
// @tfield EntityProxy parent Read only.
// @within Missile
static int Getparent( lua_State* L )
{
	Missile* missile = LuaLib<Missile>::check( L, 1 );
	if ( !IsLiveMissile( missile ) ) return 0;

	gentity_t* parent = missile->proxy->ent->parent;
	if ( !parent || !parent->inuse )
	{
		return 0;
	}

	LuaLib<EntityProxy>::push( L, Entity::CreateProxy( parent, L ) );
	return 1;
}

static int SetCallback( lua_State* L, Missile::CallbackType type )
{
	Missile* missile = LuaLib<Missile>::check( L, 1 );
	if ( !IsLiveMissile( missile ) )
	{
		Log::Warn( "trying to modify a stale missile callback" );
		return 0;
	}

	int* ref = missile->CallbackRef( type );
	if ( !ref ) return 0;

	if ( *ref != LUA_NOREF )
	{
		luaL_unref( L, LUA_REGISTRYINDEX, *ref );
		*ref = LUA_NOREF;
	}

	if ( lua_isnil( L, 2 ) )
	{
		return 0;
	}

	if ( !lua_isfunction( L, 2 ) )
	{
		Log::Warn( "expected function argument for missile %s", CallbackName( type ) );
		return 0;
	}

	lua_pushvalue( L, 2 );
	*ref = luaL_ref( L, LUA_REGISTRYINDEX );
	return 0;
}

#define MISSILE_CALLBACK( name, type ) \
	static int Get##name( lua_State* L ) \
	{ \
		return GetCallback( L, Missile::type ); \
	} \
	\
	static int Set##name( lua_State* L ) \
	{ \
		return SetCallback( L, Missile::type ); \
	}

MISSILE_CALLBACK( impact, IMPACT )
MISSILE_CALLBACK( expire, EXPIRE )

static void DispatchMissileCallback( gentity_t* missileEnt, Missile::CallbackType type, gentity_t* hitEnt )
{
	if ( !missileEnt ) return;

	EntityProxy* proxy = Entity::proxies[ missileEnt->num() ];
	if ( !proxy || proxy->ent != missileEnt || proxy->generation != missileEnt->generation || !proxy->missile )
	{
		return;
	}

	Missile* missile = proxy->missile.get();
	int* ref = missile->CallbackRef( type );
	if ( ref && *ref != LUA_NOREF )
	{
		lua_rawgeti( proxy->L, LUA_REGISTRYINDEX, *ref );
		LuaLib<EntityProxy>::push( proxy->L, proxy );

		int numArgs = 1;
		if ( type == Missile::IMPACT )
		{
			if ( hitEnt && hitEnt->num() != ENTITYNUM_NONE && hitEnt->num() != ENTITYNUM_WORLD && hitEnt->inuse )
			{
				LuaLib<EntityProxy>::push( proxy->L, Entity::CreateProxy( hitEnt, proxy->L ) );
			}
			else
			{
				lua_pushnil( proxy->L );
			}
			numArgs = 2;
		}

		if ( lua_pcall( proxy->L, numArgs, 0, 0 ) != 0 )
		{
			Log::Warn( "Could not run lua missile %s callback: %s",
			           CallbackName( type ), lua_tostring( proxy->L, -1 ) );
			lua_pop( proxy->L, 1 );
		}
	}

	missile->ClearCallbacks();
}

}  // namespace

void ExecMissileImpactCallback( gentity_t* missile, gentity_t* hitEnt )
{
	DispatchMissileCallback( missile, Missile::IMPACT, hitEnt );
}

void ExecMissileExpireCallback( gentity_t* missile )
{
	DispatchMissileCallback( missile, Missile::EXPIRE, nullptr );
}

RegType<Missile> MissileMethods[] = {
	{ nullptr, nullptr },
};

luaL_Reg MissileGetters[] = {
	GETTER( origin ),
	GETTER( type ),
	GETTER( parent ),
	GETTER( impact ),
	GETTER( expire ),
	{ nullptr, nullptr },
};

luaL_Reg MissileSetters[] = {
	SETTER( impact ),
	SETTER( expire ),
	{ nullptr, nullptr },
};

}  // namespace Lua

namespace Shared {
namespace Lua {

LUACORETYPEDEFINE( ::Lua::Missile )

template <>
void ExtraInit<::Lua::Missile>( lua_State* /*L*/, int /*metatable_index*/ )
{}

}  // namespace Lua
}  // namespace Shared
