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
#include "sgame/lua/EntityProxy.h"

#include "sgame/lua/Missile.h"
#include "sgame/sg_local.h"

#include "sgame/Entities.h"
#include "sgame/lua/Entity.h"
#include "shared/lua/LuaLib.h"

using Shared::Lua::LuaLib;
using Shared::Lua::RegType;

/// Handle interactions with Entities.
// @module entityproxy

namespace Lua {

/// Access information and interact with in game entities. Wrapper class for gentity_t.
// @table EntityProxy
EntityProxy::EntityProxy( gentity_t* ent, lua_State* L ) : ent( ent ), generation( ent ? ent->generation : 0 ), L( L ) {}
EntityProxy::~EntityProxy() = default;

namespace {

static bool IsLiveEntity( EntityProxy* proxy )
{
	return proxy && proxy->ent && proxy->ent->inuse && proxy->ent->generation == proxy->generation;
}

#define GETTER( name )   \
	{                    \
		#name, ::Lua::Get##name \
	}
#define SETTER( name )   \
	{                    \
		#name, ::Lua::Set##name \
	}

#define GET_FUNC( var, type )                                    \
	static int Get##var( lua_State* L )                          \
	{                                                            \
		EntityProxy* proxy = LuaLib<EntityProxy>::check( L, 1 ); \
		if ( !IsLiveEntity( proxy ) ) return 0;                  \
		lua_push##type( L, proxy->ent->var );                    \
		return 1;                                                \
	}

#define GET_FUNC2( var, func )                                   \
	static int Get##var( lua_State* L )                          \
	{                                                            \
		EntityProxy* proxy = LuaLib<EntityProxy>::check( L, 1 ); \
		if ( !IsLiveEntity( proxy ) ) return 0;                  \
		func;                                                    \
		return 1;                                                \
	}

/// Entity origin. Array of floats starting at index 1.
// @tfield array origin Read/Write.
// @within EntityProxy
GET_FUNC2( origin, Shared::Lua::PushVec3( L, proxy->ent->s.origin ) )
/// Entity origin2. Array of floats starting at index 1.
// Can mean a lot of things depending on the context.
// @tfield array origin Read/Write.
// @within EntityProxy
GET_FUNC2( origin2, Shared::Lua::PushVec3( L, proxy->ent->s.origin2 ) )
/// Entity classname.
// @tfield string class_name Read only.
// @within EntityProxy
GET_FUNC2( class_name, lua_pushstring( L, proxy->ent->classname ) )
/// Entity ID. A manually unique ID specifically for this entity.
// @tfield string id Read/Write.
// @within EntityProxy
GET_FUNC2( id, lua_pushstring( L, proxy->ent->id ) )
/// Entity angles. Controls orientation of the entity. Array of floats starting at index 1.
// @tfield array angles Read/Write.
// @within EntityProxy
GET_FUNC2( angles, Shared::Lua::PushVec3( L, proxy->ent->s.angles ) )
/// The next level.time the entity will think.
// @tfield integer nextthink Read/Write.
// @see level.time
// @within EntityProxy
GET_FUNC2( nextthink, lua_pushinteger( L, proxy->ent->nextthink ) )
/// The mins for the entity AABB. Array of floats starting at index 1.
// @tfield array mins Read/Write.
// @within EntityProxy
GET_FUNC2( mins, Shared::Lua::PushVec3( L, proxy->ent->r.mins ) )
/// The maxs for the entity AABB. Array of floats starting at index 1.
// @tfield array maxs Read/Write.
// @within EntityProxy
GET_FUNC2( maxs, Shared::Lua::PushVec3( L, proxy->ent->r.maxs ) )
/// The entity number. Also the entity's index in g_entities.
// @tfield integer number Read only.
// @within EntityProxy
GET_FUNC2( number, lua_pushinteger( L, proxy->ent->num() ) )
/// The entity generation captured by this proxy.
// @tfield integer generation Read only.
// @within EntityProxy
static int Getgeneration( lua_State* L )
{
	EntityProxy* proxy = LuaLib<EntityProxy>::check( L, 1 );
	if ( !proxy || !proxy->ent ) return 0;
	lua_pushinteger( L, proxy->generation );
	return 1;
}

/// The entity team.
// @tfield string team Read only.
// @within EntityProxy
static int Getteam( lua_State* L )
{
	EntityProxy* proxy = LuaLib<EntityProxy>::check( L, 1 );
	if ( !IsLiveEntity( proxy ) ) return 0;
	team_t team = TEAM_NONE;

	switch ( proxy->ent->s.eType )
	{
		case entityType_t::ET_BUILDABLE:
			team = proxy->ent->buildableTeam;
			break;

		case entityType_t::ET_PLAYER:
		case entityType_t::ET_INVISIBLE:
			if ( proxy->ent->client )
			{
				team = static_cast<team_t>( proxy->ent->client->pers.team );
				break;
			}
			// fallthrough
		default:
			team = proxy->ent->mapEntity.conditions.team;
			break;
	}
	lua_pushstring( L, BG_TeamName( team ) );
	return 1;
}

/// Fields related to players. Will be nil if the entity is not a player.
// @tfield Client client Read/Write.
// @within EntityProxy
// @see client
// @usage if ent.client ~= nil then print(ent.client.name) end -- Print client name
static int Getclient(lua_State* L)
{
	EntityProxy* proxy = LuaLib<EntityProxy>::check( L, 1 );
	if (!IsLiveEntity( proxy ) || !proxy->ent->client) return 0;
	if (!proxy->client || proxy->client->ent != proxy->ent)
	{
		proxy->client.reset(new Client(proxy->ent));
	}
	LuaLib<Client>::push(L, proxy->client.get());
	return 1;
}

/// Fields related to bots. Will be nil if the entity is not a bot.
// @tfield Bot bot Read/Write.
// @within EntityProxy
// @see bot
static int Getbot(lua_State* L)
{
	EntityProxy* proxy = LuaLib<EntityProxy>::check( L, 1 );
	if (!IsLiveEntity( proxy ) || !proxy->ent->botMind)
	{
		proxy->bot.reset();
		return 0;
	}
	if (!proxy->bot || proxy->bot->ent != proxy->ent)
	{
		proxy->bot.reset(new Bot(proxy->ent));
	}
	LuaLib<Bot>::push(L, proxy->bot.get());
	return 1;
}

/// Fields related to buildables. Will be nil if the entity is not a buildable.
// @tfield Buildable buildable Read/Write.
// @within EntityProxy
// @see buildable
static int Getbuildable(lua_State* L)
{
	EntityProxy* proxy = LuaLib<EntityProxy>::check( L, 1 );
	if (!IsLiveEntity( proxy ) || proxy->ent->s.eType != entityType_t::ET_BUILDABLE)
	{
		proxy->buildable.reset();
		return 0;
	}
	if (!proxy->buildable || !proxy->buildable->ent || proxy->buildable->ent.get() != proxy->ent)
	{
		proxy->buildable.reset(new Buildable(proxy->ent));
	}
	LuaLib<Buildable>::push(L, proxy->buildable.get());
	return 1;
}

/// Fields related to missiles. Will be nil if the entity is not a missile.
// @tfield Missile missile Read/Write.
// @within EntityProxy
// @see missile
static int Getmissile( lua_State* L )
{
	EntityProxy* proxy = LuaLib<EntityProxy>::check( L, 1 );
	if ( !IsLiveEntity( proxy ) || !HasMissileComponent( proxy->ent ) )
	{
		proxy->missile.reset();
		return 0;
	}
	if ( !proxy->missile || proxy->missile->proxy != proxy )
	{
		proxy->missile.reset( new Missile( proxy ) );
	}
	LuaLib<Missile>::push( L, proxy->missile.get() );
	return 1;
}

/// Kill this entity instantly.
// @function kill
// @tparam string|integer|nil mod Optional means-of-death, e.g. "MOD_SLOWBLOB".
// @tparam EntityProxy|nil source Optional source entity to attribute the kill to.
// @usage ent:kill("MOD_SLOWBLOB", attacker)
// @within EntityProxy
static int Methodkill( lua_State* L, EntityProxy* proxy )
{
	if ( !IsLiveEntity( proxy ) )
	{
		Log::Warn( "trying to kill a stale entity!" );
		return 0;
	}

	meansOfDeath_t mod = MOD_SUICIDE;
	if ( lua_gettop( L ) >= 1 && !lua_isnil( L, 1 ) )
	{
		if ( lua_isinteger( L, 1 ) )
		{
			mod = static_cast<meansOfDeath_t>( lua_tointeger( L, 1 ) );
		}
		else if ( lua_isstring( L, 1 ) )
		{
			mod = BG_MeansOfDeathByName( lua_tostring( L, 1 ) );
		}
		else
		{
			Log::Warn( "EntityProxy.kill expected mod as string, integer, or nil." );
			return 0;
		}
	}

	gentity_t* source = nullptr;
	if ( lua_gettop( L ) >= 2 && !lua_isnil( L, 2 ) )
	{
		EntityProxy* sourceProxy = LuaLib<EntityProxy>::check( L, 2 );
		if ( !IsLiveEntity( sourceProxy ) )
		{
			Log::Warn( "EntityProxy.kill expected a live source entity." );
			return 0;
		}
		source = sourceProxy->ent;
	}

	Entities::Kill( proxy->ent, source, mod );
	return 0;
}

static int Setorigin( lua_State* L )
{
	if ( lua_istable( L, 2 ) )
	{
		EntityProxy* proxy = LuaLib<EntityProxy>::check( L, 1 );
		if ( !IsLiveEntity( proxy ) ) return 0;
		vec3_t origin;
		Shared::Lua::CheckVec3( L, 2, origin );
		VectorCopy( origin, proxy->ent->s.origin );
		VectorCopy( origin, proxy->ent->r.currentOrigin );
		trap_LinkEntity( proxy->ent );
	}
	return 0;
}

static int Setorigin2( lua_State* L )
{
	if ( lua_istable( L, 2 ) )
	{
		EntityProxy* proxy = LuaLib<EntityProxy>::check( L, 1 );
		if ( !IsLiveEntity( proxy ) ) return 0;
		vec3_t origin;
		Shared::Lua::CheckVec3( L, 2, origin );
		VectorCopy( origin, proxy->ent->s.origin2 );
	}
	return 0;
}

static int Setangles( lua_State* L )
{
	if ( lua_istable( L, 2 ) )
	{
		EntityProxy* proxy = LuaLib<EntityProxy>::check( L, 1 );
		if ( !IsLiveEntity( proxy ) ) return 0;
		vec3_t angles;
		Shared::Lua::CheckVec3( L, 2, angles );
		VectorCopy( angles, proxy->ent->s.angles );
		VectorCopy( angles, proxy->ent->r.currentAngles );
		trap_LinkEntity( proxy->ent );
	}
	return 0;
}

static int Setnextthink( lua_State* L )
{
	EntityProxy* proxy = LuaLib<EntityProxy>::check( L, 1 );
	if ( !IsLiveEntity( proxy ) ) return 0;
	int nextthink = luaL_checknumber( L, 2 );
	if ( nextthink > level.time )
	{
		proxy->ent->nextthink = nextthink;
	}
	return 0;
}

static int Setmins( lua_State* L )
{
	if ( lua_istable( L, 2 ) )
	{
		EntityProxy* proxy = LuaLib<EntityProxy>::check( L, 1 );
		if ( !IsLiveEntity( proxy ) ) return 0;
		vec3_t mins;
		Shared::Lua::CheckVec3( L, 2, mins );
		VectorCopy( mins, proxy->ent->r.mins );
		trap_LinkEntity( proxy->ent );
	}
	return 0;
}

static int Setmaxs( lua_State* L )
{
	if ( lua_istable( L, 2 ) )
	{
		EntityProxy* proxy = LuaLib<EntityProxy>::check( L, 1 );
		if ( !IsLiveEntity( proxy ) ) return 0;
		vec3_t maxs;
		Shared::Lua::CheckVec3( L, 2, maxs );
		VectorCopy( maxs, proxy->ent->r.maxs );
		trap_LinkEntity( proxy->ent );
	}
	return 0;
}

static int Setid( lua_State* L )
{
	if ( lua_istable( L, 2 ) )
	{
		EntityProxy* proxy = LuaLib<EntityProxy>::check( L, 1 );
		if ( !IsLiveEntity( proxy ) ) return 0;
		const char* luaid = luaL_checkstring( L, 1 );
		if ( proxy->ent->id )
		{
			BG_Free( proxy->ent->id );
		}
		proxy->ent->id = BG_strdup( luaid );
	}
	return 0;
}

template <typename T>
void Push( lua_State* /*L*/, T /*arg*/ )
{}

template <>
void Push<gentity_t*>( lua_State* L, gentity_t* ent )
{
	if ( !ent )
	{
		lua_pushnil( L );
		return;
	}
	EntityProxy* proxy = Entity::CreateProxy( ent, L );
	LuaLib<EntityProxy>::push( L, proxy );
}

template <>
void Push<int>( lua_State* L, int num )
{
	lua_pushinteger( L, num );
}

template <typename T>
void PushArgs( lua_State* L, T arg )
{
	Push( L, arg );
}

template <typename T, typename... Args>
void PushArgs( lua_State* L, T arg, Args... args )
{
	PushArgs( L, arg );
	PushArgs( L, args... );
}

#define ExecFunc( method, upper, def, numArgs, ... )                                     \
	static void Exec##method def                                                         \
	{                                                                                    \
		int entityNum = self->num();                                                     \
		EntityProxy* proxy = Entity::proxies[ entityNum ];                               \
		if ( !proxy )                                                                    \
		{                                                                                \
			Log::Warn( "Error " #method "-ing: No proxy for entity num %d", entityNum ); \
			return;                                                                      \
		}                                                                                \
		auto it = proxy->funcs.find( EntityProxy::upper );                               \
		if ( it == proxy->funcs.end() )                                                  \
		{                                                                                \
			Log::Warn( "Error " #method "-ing: No Lua callback for entity num %d", entityNum ); \
			return;                                                                      \
		}                                                                                \
		lua_rawgeti( proxy->L, LUA_REGISTRYINDEX, it->second.luaRef );                   \
		PushArgs( proxy->L, __VA_ARGS__ );                                               \
		if ( lua_pcall( proxy->L, numArgs, 1, 0 ) != 0 )                                 \
		{                                                                                \
			Log::Warn( "Could not run lua " #method " callback: %s",                     \
				lua_tostring( proxy->L, -1 ) );                                   \
		}                                                                                \
		if ( lua_toboolean( proxy->L, -1 ) && it->second.method ) it->second.method( __VA_ARGS__ );                       \
		lua_pop( proxy->L, 1 );                                                          \
	}                                                                                    \
	static int Set##method( lua_State* L )                                               \
	{                                                                                    \
		EntityProxy* proxy = LuaLib<EntityProxy>::check( L, 1 );                         \
		if ( !proxy ) return 0;                                                          \
		if ( !IsLiveEntity( proxy ) )                                                    \
		{                                                                                \
			Log::Warn( "trying to modify a stale entity callback" );                     \
			return 0;                                                                    \
		}                                                                                \
		int ref;                                                                         \
		/* if set to nil, clear old lua function */                                      \
		if ( lua_isnil( L, 2 ) )                                                         \
		{                                                                                \
			ref = -1;                                                                    \
		}                                                                                \
		else if ( lua_isfunction( L, 2 ) )                                                    \
		{                                                                                \
			ref = luaL_ref( L, LUA_REGISTRYINDEX );                                      \
		}                                                                                \
		else                                                                             \
		{                                                                                \
			Log::Warn( "expected function argument for " #method );                      \
			return 0;                                                                    \
		}                                                                                \
		auto it = proxy->funcs.find( EntityProxy::upper );                               \
		bool hadExistingFunc = it != proxy->funcs.end();                                 \
		if ( it == proxy->funcs.end() && ref != -1 )                                     \
		{                                                                                \
			EntityProxy::EntityFunction func = {};                                       \
			func.type = EntityProxy::upper;                                              \
			func.luaRef = ref;                                                           \
			func.method = proxy->ent->method;                                            \
			it = proxy->funcs.insert( { EntityProxy::upper, std::move( func ) } ).first; \
			proxy->ent->method = Exec##method;                                           \
		}                                                                                \
		if ( hadExistingFunc )                                                        \
		{                                                                            \
			luaL_unref( L, LUA_REGISTRYINDEX, it->second.luaRef );                   \
		}                                                                            \
		/* If set to nil, remove lua func all together */                            \
		if ( ref == -1 )                                                             \
		{                                                                            \
			if ( it == proxy->funcs.end() ) return 0;                                \
			proxy->ent->method = it->second.method;                                  \
			proxy->funcs.erase( it );                                                \
		}                                                                            \
		else                                                                         \
		{                                                                            \
			it->second.luaRef = ref;                                                 \
			if ( proxy->ent->method != Exec##method )                                \
			{                                                                        \
				it->second.method = proxy->ent->method;                              \
				proxy->ent->method = Exec##method;                                   \
			}                                                                        \
			if ( EntityProxy::upper == EntityProxy::USE ) proxy->ent->s.eFlags |= EF_USABLE; \
		}                                                                            \
		return 0;                                                                        \
	}                                                                                    \
	static int Get##method( lua_State* L )                                               \
	{                                                                                    \
		EntityProxy* proxy = LuaLib<EntityProxy>::check( L, 1 );                         \
		if ( !IsLiveEntity( proxy ) ) return 0;                                          \
		lua_pushboolean( L, proxy->ent->method != nullptr );                             \
		return 1;                                                                        \
	}

/// The Lua think function. Will be called every time the entity thinks.
// General notes about these Lua Entity functions:
// On read, returns true or nil if the function is set.
// On write, accepts a function.
// Set to nil to clear the Lua function.
// When the Lua callback runs, returning true will also run the original C++
// function. Returning false or nil suppresses the original function.
// @tfield function|bool think function(EntityProxy self)
// @tparam EntityProxy self The current entity.
// @within EntityProxy
ExecFunc( think, THINK, ( gentity_t * self ), 1, self )
/// The Lua reset function. Will be called every time the entity restets.
// @tfield function|bool reset function(EntityProxy self)
// @tparam EntityProxy self The current entity.
// @within EntityProxy
// @see think
ExecFunc( reset, RESET, ( gentity_t * self ), 1, self )
/// The Lua touch function. Will be called every time a collidable entity is touched.
// @tfield function|bool touch function(EntityProxy self, EntityProxy toucher)
// @tparam EntityProxy self The current entity.
// @tparam EntityProxy toucher The touching entity.
// @within EntityProxy
// @see think
ExecFunc(
	touch, TOUCH, ( gentity_t * self, gentity_t* other ), 2, self, other )
/// The Lua use function. Will be called every time a usable entity is used.
// @tfield function|bool use function(EntityProxy self, EntityProxy caller, EntityProxy activator)
// @tparam EntityProxy self The current entity.
// @tparam EntityProxy caller The calling entity (idk what this means.)
// @tparam EntityProxy activator The entity that uses this entity.
// @within EntityProxy
// @see think
ExecFunc( use, USE, ( gentity_t * self, gentity_t* other, gentity_t* act ), 3, self, other, act )
/// The Lua pain function. Will be called every time an entity takes damage.
// @tfield function|bool pain function(EntityProxy self, EntityProxy attacker, integer damage)
// @tparam EntityProxy self The current entity.
// @tparam EntityProxy attacker|nil The entity that initiated the damage.
// @tparam integer damage The amount of damage taken.
// @within EntityProxy
// @see think
ExecFunc(
	pain, PAIN, ( gentity_t * self, gentity_t* attacker, int damage ), 3, self, attacker, damage )
/// The Lua die function. Will be called every time an entity dies.
// @tfield function|bool die function(EntityProxy self, EntityProxy inflictor, EntityProxy attacker, integer mod)
// @tparam EntityProxy self The current entity.
// @tparam EntityProxy|nil inflictor The entity that killed the entity. idk when these are different.
// @tparam EntityProxy|nil attacker The entity that killed the entity.
// @tparam integer mod The kill cause number. Look at bg_public.h.
// @within EntityProxy
// @see think
ExecFunc(
    die, DIE, ( gentity_t * self, gentity_t* inflictor, gentity_t* attacker, int mod ), 4, self, inflictor, attacker, mod )

RegType<::Lua::EntityProxy> EntityProxyMethods[] = {
	{ "kill", Methodkill },
	{ nullptr, nullptr },
};

luaL_Reg EntityProxyGetters[] = {
	GETTER( origin ),
	GETTER( origin2 ),
	GETTER( id ),
	GETTER( class_name ),
	GETTER( angles ),
	GETTER( team ),
	GETTER( nextthink ),
	GETTER( mins ),
	GETTER( maxs ),
	GETTER( number ),
	GETTER( generation ),
	// Getters for functions just return bool if the function is set.
	GETTER( think ),
	GETTER( reset ),
	GETTER( touch ),
	GETTER( use ),
	GETTER( pain ),
	GETTER( die ),

	GETTER( client ),
	GETTER( bot ),
	GETTER( buildable ),
	GETTER( missile ),

	{ nullptr, nullptr }
};

luaL_Reg EntityProxySetters[] = {
	SETTER( origin ),
	SETTER( origin2 ),
	SETTER( angles ),
	SETTER( nextthink ),
	SETTER( mins ),
	SETTER( maxs ),
	// Setters for functions allow running a lua callback in addition to the
	// existing callback.
	SETTER( think ),
	SETTER( reset ),
	SETTER( touch ),
	SETTER( use ),
	SETTER( pain ),
	SETTER( die ),
	SETTER( id ),

	{ nullptr, nullptr }
};
}  // namespace
}  // namespace Lua

namespace Shared {
namespace Lua {

LUACORETYPEDEFINE( ::Lua::EntityProxy )

template <>
void ExtraInit<::Lua::EntityProxy>( lua_State* /*L*/, int /*metatable_index*/ )
{}

}  // namespace Lua
}  // namespace Shared
