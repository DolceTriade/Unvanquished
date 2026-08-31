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

#include "sgame/lua/Hooks.h"

#include "sgame/sg_local.h"

#include "sgame/lua/Entity.h"
#include "sgame/lua/Interpreter.h"
#include "shared/lua/LuaLib.h"

using Shared::Lua::LuaLib;
using Shared::Lua::RegType;

/// Register hooks to install callbacks on various game events.
// Multiple callbacks can be registered for each event.
/// @module hooks

namespace Lua {

using HookList = std::vector<int>;

static HookList chatHooks;
static HookList clientConnectHooks;
static HookList teamChangeHooks;
static HookList playerSpawnHooks;
static HookList gameEndHooks;
static HookList buildableSpawnedHooks;
static HookList missileSpawnedHooks;
static HookList shutdownHooks;

static int RegisterHook( lua_State* L, HookList& hooks )
{
	if ( lua_isfunction( L, 1 ) )
	{
		lua_pushvalue( L, 1 );
		int ref = luaL_ref( L, LUA_REGISTRYINDEX );
		hooks.push_back( ref );
		lua_pushinteger( L, ref );
		return 1;
	}
	return 0;
}

static bool UnregisterHook( HookList& hooks, lua_Integer token )
{
	lua_State* L = State();
	for ( int& ref : hooks )
	{
		if ( ref == token )
		{
			luaL_unref( L, LUA_REGISTRYINDEX, ref );
			ref = LUA_REFNIL;
			return true;
		}
	}
	return false;
}

static int UnregisterHook( lua_State* L, HookList& hooks )
{
	lua_Integer token = luaL_checkinteger( L, 1 );
	lua_pushboolean( L, UnregisterHook( hooks, token ) );
	return 1;
}

static void ClearHooks( HookList& hooks )
{
	lua_State* L = State();
	for ( int ref : hooks )
	{
		if ( ref != LUA_REFNIL )
		{
			luaL_unref( L, LUA_REGISTRYINDEX, ref );
		}
	}
	hooks.clear();
}

void ClearAllHooks()
{
	ClearHooks( chatHooks );
	ClearHooks( clientConnectHooks );
	ClearHooks( teamChangeHooks );
	ClearHooks( playerSpawnHooks );
	ClearHooks( gameEndHooks );
	ClearHooks( buildableSpawnedHooks );
	ClearHooks( missileSpawnedHooks );
	ClearHooks( shutdownHooks );
}

/// Install a callback that will be called for every chat message.
// The callback should be  function(EntityProxy, team, message).
// where team = 'alien', 'human', '&lt;team&gt;' (for all chat)
// and message is just the message including any quake3 colors.
// @function RegisterChatHook
// @tparam function callback function(EntityProxy, team, message)
// @treturn integer Opaque token for unregistering the hook.
int RegisterChatHook( lua_State* L )
{
	return RegisterHook( L, chatHooks );
}

/// Unregister a chat hook previously returned by RegisterChatHook.
// @function UnregisterChatHook
// @tparam integer token Opaque hook token returned by RegisterChatHook.
// @treturn boolean True if the hook was removed.
int UnregisterChatHook( lua_State* L )
{
	return UnregisterHook( L, chatHooks );
}

void ExecChatHooks( gentity_t* ent, team_t team, Str::StringRef message )
{
	// nullptr ent can be for console chats.
	if ( !ent ) return;
	lua_State* L = State();
	for ( int ref : chatHooks )
	{
		if ( ref == LUA_REFNIL ) continue;
		lua_rawgeti( L, LUA_REGISTRYINDEX, ref );
		EntityProxy* proxy = Entity::CreateProxy( ent, L );
		LuaLib<EntityProxy>::push( L, proxy );
		lua_pushstring( L, BG_TeamName( team ) );
		lua_pushstring( L, message.c_str() );
		if ( lua_pcall( L, 3, 0, 0 ) != 0 )
		{
			Log::Warn( "Could not run lua chat hook callback: %s", lua_tostring( L, -1 ) );
		}
	}
}

/// Install a callback that will be called for every client connect or disconnect.
// The callback should be  function(EntityProxy, connect).
// where connect = true for connect and false for disconnect.
// @function RegisterClientConnectHook
// @tparam function callback function(EntityProxy, connect)
// @treturn integer Opaque token for unregistering the hook.
int RegisterClientConnectHook( lua_State* L )
{
	return RegisterHook( L, clientConnectHooks );
}

/// Unregister a client connect hook previously returned by RegisterClientConnectHook.
// @function UnregisterClientConnectHook
// @tparam integer token Opaque hook token returned by RegisterClientConnectHook.
// @treturn boolean True if the hook was removed.
int UnregisterClientConnectHook( lua_State* L )
{
	return UnregisterHook( L, clientConnectHooks );
}

void ExecClientConnectHooks( gentity_t* ent, bool connect )
{
	// nullptr ent can be for console chats.
	if ( !ent ) return;
	lua_State* L = State();
	for ( int ref : clientConnectHooks )
	{
		if ( ref == LUA_REFNIL ) continue;
		lua_rawgeti( L, LUA_REGISTRYINDEX, ref );
		EntityProxy* proxy = Entity::CreateProxy( ent, L );
		LuaLib<EntityProxy>::push( L, proxy );
		lua_pushboolean( L, connect );
		if ( lua_pcall( L, 2, 0, 0 ) != 0 )
		{
			Log::Warn( "Could not run lua client connect hook callback: %s",
			           lua_tostring( L, -1 ) );
		}
	}
}

/// Install a callback that will be called for every time a client changes teams.
// The callback should be  function(EntityProxy, newTeam).
// where newTeam will be 'alien', 'human', 'spectator'.
// @function RegisterTeamChangeHook
// @tparam function callback function(EntityProxy, newTeam)
// @treturn integer Opaque token for unregistering the hook.
int RegisterTeamChangeHook( lua_State* L )
{
	return RegisterHook( L, teamChangeHooks );
}

/// Unregister a team change hook previously returned by RegisterTeamChangeHook.
// @function UnregisterTeamChangeHook
// @tparam integer token Opaque hook token returned by RegisterTeamChangeHook.
// @treturn boolean True if the hook was removed.
int UnregisterTeamChangeHook( lua_State* L )
{
	return UnregisterHook( L, teamChangeHooks );
}

void ExecTeamChangeHooks( gentity_t* ent, team_t team )
{
	// nullptr ent can be for console chats.
	if ( !ent ) return;
	lua_State* L = State();
	for ( int ref : teamChangeHooks )
	{
		if ( ref == LUA_REFNIL ) continue;
		lua_rawgeti( L, LUA_REGISTRYINDEX, ref );
		EntityProxy* proxy = Entity::CreateProxy( ent, L );
		LuaLib<EntityProxy>::push( L, proxy );
		lua_pushstring( L, BG_TeamName( team ) );
		if ( lua_pcall( L, 2, 0, 0 ) != 0 )
		{
			Log::Warn( "Could not run lua team change hook callback: %s",
			           lua_tostring( L, -1 ) );
		}
	}
}

/// Install a callback that will be called for every time a player changes classes.
// This includes initial spawning, but also evolving, de-evolving, buying a bsuit, etc.
// The callback should be  function(EntityProxy).
// @function RegisterPlayerSpawnHook
// @tparam function callback function(EntityProxy)
// @treturn integer Opaque token for unregistering the hook.
int RegisterPlayerSpawnHook( lua_State* L )
{
	return RegisterHook( L, playerSpawnHooks );
}

/// Unregister a player spawn hook previously returned by RegisterPlayerSpawnHook.
// @function UnregisterPlayerSpawnHook
// @tparam integer token Opaque hook token returned by RegisterPlayerSpawnHook.
// @treturn boolean True if the hook was removed.
int UnregisterPlayerSpawnHook( lua_State* L )
{
	return UnregisterHook( L, playerSpawnHooks );
}

void ExecPlayerSpawnHooks( gentity_t* ent )
{
	// nullptr ent can be for console chats.
	if ( !ent ) return;
	lua_State* L = State();
	for ( int ref : playerSpawnHooks )
	{
		if ( ref == LUA_REFNIL ) continue;
		lua_rawgeti( L, LUA_REGISTRYINDEX, ref );
		EntityProxy* proxy = Entity::CreateProxy( ent, L );
		LuaLib<EntityProxy>::push( L, proxy );
		if ( lua_pcall( L, 1, 0, 0 ) != 0 )
		{
			Log::Warn( "Could not run lua player spawn hook callback: %s",
			           lua_tostring( L, -1 ) );
		}
	}
}

/// Install a callback that will be called to check whether a game should end.
// The callback should be  function() and should return either 'human', 'alien', depending
// on whether the respective team has won the game. If no team has won, the function should
// return false.
// @function RegisterGameEndHook
// @tparam function callback function()
// @treturn integer Opaque token for unregistering the hook.
int RegisterGameEndHook( lua_State* L )
{
	return RegisterHook( L, gameEndHooks );
}

/// Unregister a game end hook previously returned by RegisterGameEndHook.
// @function UnregisterGameEndHook
// @tparam integer token Opaque hook token returned by RegisterGameEndHook.
// @treturn boolean True if the hook was removed.
int UnregisterGameEndHook( lua_State* L )
{
	return UnregisterHook( L, gameEndHooks );
}

team_t ExecGameEndHooks()
{
	lua_State* L = State();
	for ( int ref : gameEndHooks )
	{
		if ( ref == LUA_REFNIL ) continue;
		lua_rawgeti( L, LUA_REGISTRYINDEX, ref );
		if ( lua_pcall( L, 0, 1, 0 ) != 0 )
		{
			Log::Warn( "Could not run lua game end hook callback: %s",
			           lua_tostring( L, -1 ) );
		}
		if ( lua_toboolean( L, -1 ) )
		{
			const char* teamName = luaL_checkstring( L, -1 );
			team_t team = BG_PlayableTeamFromString( teamName );
			if ( team != TEAM_NONE )
			{
				lua_pop( L, 1 );
				return team;
			}
			lua_pop( L, 1 );
		}
		else
		{
			lua_pop( L, 1 );
		}
	}
	return TEAM_NONE;
}

/// Install a callback that will be called for every time a buildable is spawned.
// The callback should be function(EntityProxy).
// @function RegisterBuildableSpawnedHook
// @tparam function callback function(EntityProxy)
// @treturn integer Opaque token for unregistering the hook.
int RegisterBuildableSpawnedHook( lua_State* L )
{
	return RegisterHook( L, buildableSpawnedHooks );
}

/// Unregister a buildable spawned hook previously returned by RegisterBuildableSpawnedHook.
// @function UnregisterBuildableSpawnedHook
// @tparam integer token Opaque hook token returned by RegisterBuildableSpawnedHook.
// @treturn boolean True if the hook was removed.
int UnregisterBuildableSpawnedHook( lua_State* L )
{
	return UnregisterHook( L, buildableSpawnedHooks );
}

void ExecBuildableSpawnedHooks( gentity_t* ent )
{
	// nullptr ent can be for console chats.
	if ( !ent ) return;
	lua_State* L = State();
	for ( int ref : buildableSpawnedHooks )
	{
		if ( ref == LUA_REFNIL ) continue;
		lua_rawgeti( L, LUA_REGISTRYINDEX, ref );
		EntityProxy* proxy = Entity::CreateProxy( ent, L );
		LuaLib<EntityProxy>::push( L, proxy );
		if ( lua_pcall( L, 1, 0, 0 ) != 0 )
		{
			Log::Warn( "Could not run lua buildable spawned hook callback: %s",
			           lua_tostring( L, -1 ) );
		}
	}
}

/// Install a callback that will be called for every time a missile is spawned.
// The callback should be function(EntityProxy).
// @function RegisterMissileSpawnedHook
// @tparam function callback function(EntityProxy)
// @treturn integer Opaque token for unregistering the hook.
int RegisterMissileSpawnedHook( lua_State* L )
{
	return RegisterHook( L, missileSpawnedHooks );
}

/// Unregister a missile spawned hook previously returned by RegisterMissileSpawnedHook.
// @function UnregisterMissileSpawnedHook
// @tparam integer token Opaque hook token returned by RegisterMissileSpawnedHook.
// @treturn boolean True if the hook was removed.
int UnregisterMissileSpawnedHook( lua_State* L )
{
	return UnregisterHook( L, missileSpawnedHooks );
}

void ExecMissileSpawnedHooks( gentity_t* ent )
{
	// nullptr ent can be for console chats.
	if ( !ent ) return;
	lua_State* L = State();
	for ( int ref : missileSpawnedHooks )
	{
		if ( ref == LUA_REFNIL ) continue;
		lua_rawgeti( L, LUA_REGISTRYINDEX, ref );
		EntityProxy* proxy = Entity::CreateProxy( ent, L );
		LuaLib<EntityProxy>::push( L, proxy );
		if ( lua_pcall( L, 1, 0, 0 ) != 0 )
		{
			Log::Warn( "Could not run lua missile spawned hook callback: %s",
			           lua_tostring( L, -1 ) );
		}
	}
}

/// Install a callback that will be called when the game is about to shutdown.
// The callback should be function().
// @function RegisterShutdownHook
// @tparam function callback function()
// @treturn integer Opaque token for unregistering the hook.
int RegisterShutdownHook( lua_State* L )
{
	return RegisterHook( L, shutdownHooks );
}

/// Unregister a shutdown hook previously returned by RegisterShutdownHook.
// @function UnregisterShutdownHook
// @tparam integer token Opaque hook token returned by RegisterShutdownHook.
// @treturn boolean True if the hook was removed.
int UnregisterShutdownHook( lua_State* L )
{
	return UnregisterHook( L, shutdownHooks );
}

void ExecShutdownHooks()
{
	lua_State* L = State();
	for ( int ref : shutdownHooks )
	{
		if ( ref == LUA_REFNIL ) continue;
		lua_rawgeti( L, LUA_REGISTRYINDEX, ref );
		if ( lua_pcall( L, 0, 0, 0 ) != 0 )
		{
			Log::Warn( "Could not run lua shutdown hook callback: %s",
			           lua_tostring( L, -1 ) );
		}
	}
}


RegType<Hooks> HooksMethods[] = {
	{ nullptr, nullptr },
};

luaL_Reg HooksGetters[] = {
	{ nullptr, nullptr },
};

luaL_Reg HooksSetters[] = {
	{ nullptr, nullptr },
};

}  // namespace Lua

namespace Shared {
namespace Lua {
LUACORETYPEDEFINE( ::Lua::Hooks )

template <>
void ExtraInit<::Lua::Hooks>( lua_State* L, int metatable_index )
{
	::Lua::ClearAllHooks();
	lua_pushcfunction( L, ::Lua::RegisterChatHook );
	lua_setfield( L, metatable_index - 1, "RegisterChatHook" );
	lua_pushcfunction( L, ::Lua::UnregisterChatHook );
	lua_setfield( L, metatable_index - 1, "UnregisterChatHook" );
	lua_pushcfunction( L, ::Lua::RegisterClientConnectHook );
	lua_setfield( L, metatable_index - 1, "RegisterClientConnectHook" );
	lua_pushcfunction( L, ::Lua::UnregisterClientConnectHook );
	lua_setfield( L, metatable_index - 1, "UnregisterClientConnectHook" );
	lua_pushcfunction( L, ::Lua::RegisterTeamChangeHook );
	lua_setfield( L, metatable_index - 1, "RegisterTeamChangeHook" );
	lua_pushcfunction( L, ::Lua::UnregisterTeamChangeHook );
	lua_setfield( L, metatable_index - 1, "UnregisterTeamChangeHook" );
	lua_pushcfunction( L, ::Lua::RegisterPlayerSpawnHook );
	lua_setfield( L, metatable_index - 1, "RegisterPlayerSpawnHook" );
	lua_pushcfunction( L, ::Lua::UnregisterPlayerSpawnHook );
	lua_setfield( L, metatable_index - 1, "UnregisterPlayerSpawnHook" );
	lua_pushcfunction( L, ::Lua::RegisterGameEndHook );
	lua_setfield( L, metatable_index - 1, "RegisterGameEndHook" );
	lua_pushcfunction( L, ::Lua::UnregisterGameEndHook );
	lua_setfield( L, metatable_index - 1, "UnregisterGameEndHook" );
	lua_pushcfunction( L, ::Lua::RegisterBuildableSpawnedHook );
	lua_setfield( L, metatable_index - 1, "RegisterBuildableSpawnedHook" );
	lua_pushcfunction( L, ::Lua::UnregisterBuildableSpawnedHook );
	lua_setfield( L, metatable_index - 1, "UnregisterBuildableSpawnedHook" );
	lua_pushcfunction( L, ::Lua::RegisterMissileSpawnedHook );
	lua_setfield( L, metatable_index - 1, "RegisterMissileSpawnedHook" );
	lua_pushcfunction( L, ::Lua::UnregisterMissileSpawnedHook );
	lua_setfield( L, metatable_index - 1, "UnregisterMissileSpawnedHook" );
	lua_pushcfunction( L, ::Lua::RegisterShutdownHook );
	lua_setfield( L, metatable_index - 1, "RegisterShutdownHook" );
	lua_pushcfunction( L, ::Lua::UnregisterShutdownHook );
	lua_setfield( L, metatable_index - 1, "UnregisterShutdownHook" );
}
}  // namespace Lua
}  // namespace Shared
