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
#include "shared/lua/Gameplay.h"
#include "shared/lua/Utils.h"
#include "shared/bg_gameplay.h"

#ifdef BUILD_SGAME
#include "sgame/sg_local.h"
#endif

namespace Shared {
namespace Lua {

Gameplay gameplay;

namespace {

int PushGameplayValue( lua_State* L, size_t index )
{
	const gameplayVarInfo_t* var = BG_GameplayVar( index );
	if ( !var )
	{
		return luaL_error( L, "invalid gameplay registry index" );
	}

	if ( var->type == GAMEPLAY_INTEGER )
	{
		lua_pushinteger( L, *static_cast<int*>( var->storage ) );
	}
	else
	{
		lua_pushnumber( L, *static_cast<float*>( var->storage ) );
	}
	return 1;
}

int GameplayReset( lua_State* L )
{
	luaL_checktype( L, 1, LUA_TUSERDATA );
	const char* key = luaL_checkstring( L, 2 );
	int index = BG_FindGameplayVarByName( key );
	if ( index < 0 )
	{
		return luaL_error( L, "unknown gameplay variable '%s'", key );
	}

#ifdef BUILD_SGAME
	std::string error;
	if ( !BG_ResetGameplayValue( index, true, &error ) )
	{
		return luaL_error( L, "%s", error.c_str() );
	}
	BG_PublishGameplayConfig();
	return 0;
#else
	return luaL_error( L, "gameplay values are read-only on the client" );
#endif
}

int GameplayResetAll( lua_State* L )
{
	luaL_checktype( L, 1, LUA_TUSERDATA );

#ifdef BUILD_SGAME
	BG_ResetGameplayOverrides();
	BG_PublishGameplayConfig();
	return 0;
#else
	return luaL_error( L, "gameplay values are read-only on the client" );
#endif
}

int GameplayIndex( lua_State* L )
{
	luaL_checktype( L, 1, LUA_TUSERDATA );
	const char* key = luaL_checkstring( L, 2 );

	lua_getglobal( L, "Gameplay" );
	if ( lua_istable( L, -1 ) )
	{
		lua_pushvalue( L, 2 );
		lua_rawget( L, -2 );
		if ( !lua_isnil( L, -1 ) )
		{
			return 1;
		}
		lua_pop( L, 1 );
	}
	lua_pop( L, 1 );

	int index = BG_FindGameplayVarByName( key );
	if ( index < 0 )
	{
		return luaL_error( L, "unknown gameplay variable '%s'", key );
	}

	return PushGameplayValue( L, index );
}

int GameplayNewIndex( lua_State* L )
{
	luaL_checktype( L, 1, LUA_TUSERDATA );
	const char* key = luaL_checkstring( L, 2 );
	int index = BG_FindGameplayVarByName( key );
	if ( index < 0 )
	{
		return luaL_error( L, "unknown gameplay variable '%s'", key );
	}

#ifndef BUILD_SGAME
	return luaL_error( L, "gameplay values are read-only on the client" );
#else
	const gameplayVarInfo_t* var = BG_GameplayVar( index );
	std::string error;

	if ( var->type == GAMEPLAY_INTEGER )
	{
		if ( !lua_isinteger( L, 3 ) )
		{
			return luaL_error( L, "gameplay variable '%s' expects an integer", key );
		}

		if ( !BG_SetGameplayInt( index, lua_tointeger( L, 3 ), true, &error ) )
		{
			return luaL_error( L, "%s", error.c_str() );
		}
	}
	else
	{
		if ( !lua_isnumber( L, 3 ) )
		{
			return luaL_error( L, "gameplay variable '%s' expects a number", key );
		}

		if ( !BG_SetGameplayFloat( index, lua_tonumber( L, 3 ), true, &error ) )
		{
			return luaL_error( L, "%s", error.c_str() );
		}
	}

	BG_PublishGameplayConfig();
	return 0;
#endif
}

int GameplayPairs( lua_State* L )
{
	return CreatePairsHelper( L, []( lua_State* L, size_t& index )
	{
		if ( index >= BG_NumGameplayVars() )
		{
			return 0;
		}

		const gameplayVarInfo_t* var = BG_GameplayVar( index );
		lua_pushstring( L, var->name );
		PushGameplayValue( L, index );
		return 2;
	} );
}

}  // namespace

template<> void ExtraInit<Gameplay>( lua_State* L, int metatable_index )
{
	const int methods_index = metatable_index - 1;

	lua_pushcfunction( L, GameplayReset );
	lua_setfield( L, methods_index, "reset" );
	lua_pushcfunction( L, GameplayResetAll );
	lua_setfield( L, methods_index, "reset_all" );
	lua_pushcfunction( L, GameplayResetAll );
	lua_setfield( L, methods_index, "resetAll" );

	lua_pushcfunction( L, GameplayIndex );
	lua_setfield( L, metatable_index, "__index" );
	lua_pushcfunction( L, GameplayNewIndex );
	lua_setfield( L, metatable_index, "__newindex" );
	lua_pushcfunction( L, GameplayPairs );
	lua_setfield( L, metatable_index, "__pairs" );
}

RegType<Gameplay> GameplayMethods[] =
{
	{ nullptr, nullptr },
};

luaL_Reg GameplayGetters[] =
{
	{ nullptr, nullptr },
};

luaL_Reg GameplaySetters[] =
{
	{ nullptr, nullptr },
};

LUACORETYPEDEFINE(Gameplay)

} // namespace Lua
} // namespace Shared
