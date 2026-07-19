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
#include "sgame/lua/Bot.h"

#include <cmath>
#include <limits>

#include "sgame/lua/Entity.h"
#include "sgame/lua/EntityProxy.h"
#include "sgame/sg_bot_local.h"
#include "sgame/sg_local.h"

#include "sgame/sg_bot_ai.h"
#include "shared/lua/LuaLib.h"
#include "shared/lua/Utils.h"

using Shared::Lua::LuaLib;
using Shared::Lua::RegType;

/// Handle interactions with Bots.
// @module bot

namespace Lua {
namespace {
static bool botMindRegistered = false;

static int ReadOnlyNewIndex( lua_State *L )
{
	return luaL_error( L, "table is read-only" );
}

static void EnsureReadOnlyTableMeta( lua_State *L )
{
	if ( luaL_newmetatable( L, "LuaReadOnlyTable" ) )
	{
		lua_pushcfunction( L, ReadOnlyNewIndex );
		lua_setfield( L, -2, "__newindex" );
		lua_pushboolean( L, true );
		lua_setfield( L, -2, "__metatable" );
	}
	lua_pop( L, 1 );
}

static void SetReadOnlyTable( lua_State *L )
{
	EnsureReadOnlyTableMeta( L );
	luaL_getmetatable( L, "LuaReadOnlyTable" );
	lua_setmetatable( L, -2 );
}

static void PushOptionalVec3( lua_State *L, Util::optional<glm::vec3> const& vec )
{
	if ( !vec )
	{
		lua_pushnil( L );
		return;
	}

	Shared::Lua::PushVec3( L, GLM4READ( *vec ) );
	SetReadOnlyTable( L );
}

static void PushPositionComponents( lua_State *L, glm::vec3 const& vec )
{
	lua_pushnumber( L, vec.x );
	lua_setfield( L, -2, "positionX" );
	lua_pushnumber( L, vec.y );
	lua_setfield( L, -2, "positionY" );
	lua_pushnumber( L, vec.z );
	lua_setfield( L, -2, "positionZ" );
}

static const char *BotJetpackStateName( botJetpackState_t state )
{
	switch ( state )
	{
		case BOT_JETPACK_NONE: return "none";
		case BOT_JETPACK_NAVCON_WAITING: return "navcon_waiting";
		case BOT_JETPACK_NAVCON_FLYING: return "navcon_flying";
		case BOT_JETPACK_NAVCON_LANDING: return "navcon_landing";
	}

	return "none";
}

static bool PushTargetFields( lua_State *L, botTarget_t const& target )
{
	if ( target.targetsCoordinates() )
	{
		lua_pushstring( L, "coordinates" );
		lua_setfield( L, -2, "kind" );

		lua_pushnil( L );
		lua_setfield( L, -2, "entity" );

		glm::vec3 position = target.getPos();
		Shared::Lua::PushVec3( L, GLM4READ( position ) );
		SetReadOnlyTable( L );
		lua_setfield( L, -2, "position" );
		PushPositionComponents( L, position );

		lua_pushnil( L );
		lua_setfield( L, -2, "entityType" );
		lua_pushnil( L );
		lua_setfield( L, -2, "team" );
		lua_pushnil( L );
		lua_setfield( L, -2, "buildable" );
		return true;
	}

	if ( target.targetsValidEntity() )
	{
		const gentity_t *ent = target.getTargetedEntity();
		lua_pushstring( L, "entity" );
		lua_setfield( L, -2, "kind" );

		LuaLib<EntityProxy>::push( L, Entity::CreateProxy( const_cast<gentity_t *>( ent ), L ) );
		lua_setfield( L, -2, "entity" );

		lua_pushnil( L );
		lua_setfield( L, -2, "position" );
		lua_pushnil( L );
		lua_setfield( L, -2, "positionX" );
		lua_pushnil( L );
		lua_setfield( L, -2, "positionY" );
		lua_pushnil( L );
		lua_setfield( L, -2, "positionZ" );

		lua_pushstring( L, Com_EntityTypeName( static_cast<entityType_t>( ent->s.eType ) ) );
		lua_setfield( L, -2, "entityType" );

		lua_pushstring( L, BG_TeamName( G_Team( const_cast<gentity_t *>( ent ) ) ) );
		lua_setfield( L, -2, "team" );

		if ( ent->s.eType == entityType_t::ET_BUILDABLE )
		{
			lua_pushstring( L, BG_Buildable( ent->s.modelindex )->name );
		}
		else
		{
			lua_pushnil( L );
		}
		lua_setfield( L, -2, "buildable" );
		return true;
	}

	lua_pushstring( L, "empty" );
	lua_setfield( L, -2, "kind" );
	lua_pushnil( L );
	lua_setfield( L, -2, "entity" );
	lua_pushnil( L );
	lua_setfield( L, -2, "position" );
	lua_pushnil( L );
	lua_setfield( L, -2, "positionX" );
	lua_pushnil( L );
	lua_setfield( L, -2, "positionY" );
	lua_pushnil( L );
	lua_setfield( L, -2, "positionZ" );
	lua_pushnil( L );
	lua_setfield( L, -2, "entityType" );
	lua_pushnil( L );
	lua_setfield( L, -2, "team" );
	lua_pushnil( L );
	lua_setfield( L, -2, "buildable" );
	return false;
}

static void PushStructuredTarget( lua_State *L, botTarget_t const& target,
                                  Util::optional<float> distance )
{
	lua_newtable( L );
	bool hasTarget = PushTargetFields( L, target );

	if ( hasTarget && distance && std::isfinite( *distance ) &&
	     *distance < std::numeric_limits<float>::max() )
	{
		lua_pushnumber( L, *distance );
	}
	else
	{
		lua_pushnil( L );
	}
	lua_setfield( L, -2, "distance" );

	SetReadOnlyTable( L );
}

static void PushStructuredGoalAndDistance( lua_State *L, botGoalAndDistance_t const& goal )
{
	PushStructuredTarget( L, goal.goal, goal.distance );
}

static void PushClosestBuilding( lua_State *L, botMemory_t const* botMind, buildable_t buildable )
{
	PushStructuredGoalAndDistance( L, botMind->closestBuildings[ buildable ] );
}

static void EnsureBotMindRegistered( lua_State *L )
{
	if ( botMindRegistered )
	{
		return;
	}

	LuaLib<BotMind>::Register( L );
	botMindRegistered = true;
}

#define GET_FUNC( var, func )                                \
	static int Get##var( lua_State* L )                      \
	{                                                        \
		Bot* c = LuaLib<Bot>::check( L, 1 );                 \
		if ( !c || !c->ent || !c->ent->botMind )             \
		{                                                    \
			Log::Warn( "trying to access stale bot info!" ); \
			return 0;                                        \
		}                                                    \
		func;                                                \
		return 1;                                            \
	}

/// Bot skill level. From 1-9. Higher skill levels are better.
// @tfield integer skill Read/Write.
// @within Bot
GET_FUNC( skill, lua_pushinteger( L, c->ent->botMind->skillLevel ) )

/// Name of the current behavior tree.
// @tfield string behavior Read only.
// @within Bot
// @see set_behavior
static int Getbehavior( lua_State* L )
{
	Bot* c = LuaLib<Bot>::check( L, 1 );
	if ( !c || !c->ent || !c->ent->botMind )
	{
		Log::Warn( "trying to access stale bot info!" );
		return 0;
	}
	if ( !c->ent->botMind->behaviorTree )
	{
		return 0;
	}
	lua_pushstring( L, c->ent->botMind->behaviorTree->name );
	return 1;
}

/// Cached bot-specific state and perception.
// @tfield BotMind mind Read only.
// @within Bot
static int Getmind( lua_State *L )
{
	Bot *c = LuaLib<Bot>::check( L, 1 );
	if ( !c || !c->ent || !c->ent->botMind )
	{
		Log::Warn( "trying to access stale bot info!" );
		if ( c )
		{
			c->mind.reset();
		}
		return 0;
	}

	EnsureBotMindRegistered( L );

	if ( !c->mind || c->mind->ent != c->ent )
	{
		c->mind.reset( new BotMind( c->ent ) );
	}

	LuaLib<BotMind>::push( L, c->mind.get() );
	return 1;
}

/// Set a new behavior tree for the bots.
// @function set_behavior
// @tparam string behavior New behavior tree file.
// @within Bot
// @see behavior
static int MethodSetBehavior( lua_State* L, Bot* c )
{
	if ( !c || !c->ent || !c->ent->botMind )
	{
		Log::Warn( "trying to access stale bot info!" );
		return 0;
	}
	const char* behavior = luaL_checkstring( L, 1 );
	if ( !behavior )
	{
		Log::Warn( "empty behavior" );
		return 0;
	}
	G_BotChangeBehavior( c->ent->num(), behavior );
	return 0;
}

static int Setskill( lua_State* L )
{
	Bot* c = LuaLib<Bot>::check( L, 1 );
	if ( !c || !c->ent || !c->ent->botMind )
	{
		Log::Warn( "trying to access stale bot info!" );
		return 0;
	}
	int skill = luaL_checkinteger( L, 2 );
	G_BotSetSkill( c->ent->client->num(), skill );
	return 0;
}

#define GET_MIND_FUNC( var, func )                           \
	static int GetMind##var( lua_State *L )                  \
	{                                                        \
		BotMind *mind = LuaLib<BotMind>::check( L, 1 );      \
		if ( !mind || !mind->ent || !mind->ent->botMind )    \
		{                                                    \
			Log::Warn( "trying to access stale bot mind!" ); \
			return 0;                                        \
		}                                                    \
		func;                                                \
		return 1;                                            \
	}

GET_MIND_FUNC( userSpecifiedPosition,
               PushOptionalVec3( L, mind->ent->botMind->userSpecifiedPosition ) )
GET_MIND_FUNC( userSpecifiedClient,
               mind->ent->botMind->userSpecifiedClientNum
                   ? lua_pushinteger( L, *mind->ent->botMind->userSpecifiedClientNum )
                   : lua_pushnil( L ) )
GET_MIND_FUNC( spawnTime, lua_pushinteger( L, mind->ent->botMind->spawnTime ) )
GET_MIND_FUNC( stuckTime, lua_pushinteger( L, mind->ent->botMind->stuckTime ) )
GET_MIND_FUNC( stuckPosition,
               Shared::Lua::PushVec3( L, GLM4READ( mind->ent->botMind->stuckPosition ) );
               SetReadOnlyTable( L ) )
GET_MIND_FUNC( enemyLastSeen, lua_pushinteger( L, mind->ent->botMind->enemyLastSeen ) )
GET_MIND_FUNC( painTime, lua_pushinteger( L, mind->ent->botMind->painTime ) )
GET_MIND_FUNC( exhausted, lua_pushboolean( L, mind->ent->botMind->exhausted ) )
GET_MIND_FUNC( stuckTimer, lua_pushinteger( L, mind->ent->botMind->myTimer ) )
GET_MIND_FUNC( buildCooldownUntil, lua_pushinteger( L, mind->ent->botMind->buildCooldownUntil ) )
GET_MIND_FUNC( jetpackState,
               lua_pushstring( L, BotJetpackStateName( mind->ent->botMind->jetpackState ) ) )
GET_MIND_FUNC( goal, PushStructuredTarget( L, mind->ent->botMind->goal, {} ) )
GET_MIND_FUNC( bestEnemy, PushStructuredGoalAndDistance( L, mind->ent->botMind->bestEnemy ) )
GET_MIND_FUNC( closestDamagedBuilding,
               PushStructuredGoalAndDistance( L, mind->ent->botMind->closestDamagedBuilding ) )
GET_MIND_FUNC( closestBuildings,
		lua_newtable( L );
		for ( int i = 1; i < BA_NUM_BUILDABLES; ++i )
		{
			const buildableAttributes_t *buildable = BG_Buildable( i );
			if ( !buildable || !buildable->name[ 0 ] )
			{
				continue;
			}

			PushStructuredGoalAndDistance( L, mind->ent->botMind->closestBuildings[ i ] );
			lua_setfield( L, -2, buildable->name );
		}
		SetReadOnlyTable( L ) )

static int MethodclosestBuilding( lua_State *L, BotMind *mind )
{
	if ( !mind || !mind->ent || !mind->ent->botMind )
	{
		Log::Warn( "trying to access stale bot mind!" );
		return 0;
	}

	const char* name = luaL_checkstring( L, 1 );
	const buildableAttributes_t* buildable = BG_BuildableByName( name );
	if ( !buildable || buildable->number <= BA_NONE || buildable->number >= BA_NUM_BUILDABLES )
	{
		return 0;
	}

	PushClosestBuilding( L, mind->ent->botMind, static_cast<buildable_t>( buildable->number ) );
	return 1;
}

#undef GET_MIND_FUNC

static int SetMindstuckTimer( lua_State *L )
{
	BotMind *mind = LuaLib<BotMind>::check( L, 1 );
	if ( !mind || !mind->ent || !mind->ent->botMind )
	{
		Log::Warn( "trying to access stale bot mind!" );
		return 0;
	}

	mind->ent->botMind->myTimer = luaL_checkinteger( L, 2 );
	return 0;
}

}  // namespace

RegType<Bot> BotMethods[] = {
	{ "set_behavior", MethodSetBehavior },

	{ nullptr, nullptr },
};

#define GETTER( name )   \
	{                    \
		#name, Get##name \
	}

luaL_Reg BotGetters[] = {
	GETTER( skill ),
	GETTER( behavior ),
	GETTER( mind ),

	{ nullptr, nullptr },
};

#define SETTER( name )   \
	{                    \
		#name, Set##name \
	}

luaL_Reg BotSetters[] = {
	SETTER( skill ),

	{ nullptr, nullptr },
};

RegType<BotMind> BotMindMethods[] = {
	{ "closestBuilding", MethodclosestBuilding },
	{ nullptr, nullptr },
};

luaL_Reg BotMindGetters[] = {
	{ "userSpecifiedPosition", GetMinduserSpecifiedPosition },
	{ "userSpecifiedClient", GetMinduserSpecifiedClient },
	{ "spawnTime", GetMindspawnTime },
	{ "stuckTime", GetMindstuckTime },
	{ "stuckPosition", GetMindstuckPosition },
	{ "enemyLastSeen", GetMindenemyLastSeen },
	{ "painTime", GetMindpainTime },
	{ "exhausted", GetMindexhausted },
	{ "stuckTimer", GetMindstuckTimer },
	{ "buildCooldownUntil", GetMindbuildCooldownUntil },
	{ "jetpackState", GetMindjetpackState },
	{ "goal", GetMindgoal },
	{ "bestEnemy", GetMindbestEnemy },
	{ "closestDamagedBuilding", GetMindclosestDamagedBuilding },
	{ "closestBuildings", GetMindclosestBuildings },
	{ nullptr, nullptr },
};

luaL_Reg BotMindSetters[] = {
	{ "stuckTimer", SetMindstuckTimer },
	{ nullptr, nullptr },
};

}  // namespace Lua

namespace Shared {
namespace Lua {
LUACORETYPEDEFINE( ::Lua::Bot )
LUACORETYPEDEFINE( ::Lua::BotMind )

template <>
void ExtraInit<::Lua::Bot>( lua_State* L, int /*metatable_index*/ )
{
	::Lua::EnsureBotMindRegistered( L );
}

template <>
void ExtraInit<::Lua::BotMind>( lua_State* /*L*/, int /*metatable_index*/ )
{}
}  // namespace Lua
}  // namespace Shared
