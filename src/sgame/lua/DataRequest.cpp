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
#include "sgame/lua/DataRequest.h"

#include "sgame/sg_local.h"

#include <cstdlib>
#include <string>
#include <unordered_map>

#include "common/Command.h"
#include "sgame/lua/Entity.h"
#include "sgame/lua/EntityProxy.h"
#include "sgame/lua/Interpreter.h"
#include "shared/lua/Json.h"
#include "shared/lua/LuaLib.h"

namespace Lua
{
namespace
{
std::unordered_map<std::string, int> endpoints;

void PushSuccessEnvelope( lua_State *L )
{
	lua_newtable( L );
	lua_pushboolean( L, 1 );
	lua_setfield( L, -2, "ok" );
	lua_pushvalue( L, -2 );
	lua_setfield( L, -2, "value" );
	lua_remove( L, -2 );
}

void PushErrorEnvelope( lua_State *L, const char *error )
{
	lua_newtable( L );
	lua_pushboolean( L, 0 );
	lua_setfield( L, -2, "ok" );
	lua_pushstring( L, error );
	lua_setfield( L, -2, "error" );
}
}  // namespace

int RegisterDataEndpoint( lua_State *L )
{
	const char *name = luaL_checkstring( L, 1 );
	luaL_checktype( L, 2, LUA_TFUNCTION );
	lua_pushvalue( L, 2 );
	int ref = luaL_ref( L, LUA_REGISTRYINDEX );
	auto it = endpoints.find( name );
	if ( it != endpoints.end() )
	{
		luaL_unref( L, LUA_REGISTRYINDEX, it->second );
	}
	endpoints[ name ] = ref;
	return 0;
}

bool HandleDataRequest( int clientNum )
{
	const Cmd::Args &args = trap_Args();
	if ( args.Argc() != 4 )
	{
		return true;
	}
	const std::string &idArg = args.Argv( 1 );
	const std::string &name = args.Argv( 2 );
	const std::string &json = args.Argv( 3 );
	char *end = nullptr;
	unsigned long id = std::strtoul( idArg.c_str(), &end, 10 );
	if ( idArg.empty() || !end || *end )
	{
		return true;
	}
	auto endpoint = endpoints.find( name );
	lua_State *L = State();
	if ( !L )
	{
		return true;
	}
	int base = lua_gettop( L );
	bool success = false;
	std::string encoded;
	std::string jsonError;
	if ( endpoint != endpoints.end() && Shared::Lua::DecodeJson( L, json.c_str(), jsonError ) )
	{
		int params = lua_gettop( L );
		lua_rawgeti( L, LUA_REGISTRYINDEX, endpoint->second );
		Lua::EntityProxy *proxy = Lua::Entity::CreateProxy( g_entities + clientNum, L );
		Shared::Lua::LuaLib<Lua::EntityProxy>::push( L, proxy );
		lua_pushvalue( L, params );
		if ( lua_pcall( L, 2, 1, 0 ) == LUA_OK )
		{
			PushSuccessEnvelope( L );
			success = Shared::Lua::EncodeJson( L, -1, encoded, jsonError );
		}
	}
	lua_settop( L, base );
	if ( !success )
	{
		PushErrorEnvelope( L, endpoint == endpoints.end() ? "unknown endpoint"
		                                                  : "invalid request or endpoint error" );
		Shared::Lua::EncodeJson( L, -1, encoded, jsonError );
		lua_settop( L, base );
	}
	std::string command =
		Str::Format( "lua_data_reply %lu %s", id, Cmd::Escape( encoded ).c_str() );
	if ( command.size() >= 1022 )
	{
		lua_settop( L, base );
		PushErrorEnvelope( L, "response exceeds reliable command size limit" );
		Shared::Lua::EncodeJson( L, -1, encoded, jsonError );
		command = Str::Format( "lua_data_reply %lu %s", id, Cmd::Escape( encoded ).c_str() );
	}
	if ( command.size() < 1022 )
	{
		trap_SendServerCommand( clientNum, command.c_str() );
	}
	else
	{
		Log::Warn( "Unable to send Lua data error response to client %d", clientNum );
	}
	return true;
}

void ClearDataEndpoints()
{
	if ( State() )
	{
		for ( const auto &endpoint : endpoints )
		{
			luaL_unref( State(), LUA_REGISTRYINDEX, endpoint.second );
		}
	}
	endpoints.clear();
}
}  // namespace Lua
