#include "cgame/rocket/lua/DataRequest.h"

#include <RmlUi/Lua/Interpreter.h>
#include "cgame/cg_local.h"

#include <cstdlib>
#include <string>
#include <unordered_map>

#include "common/Command.h"
#include "shared/lua/Json.h"

/// Request structured data from the server from cgame Lua.
// @module cgame

namespace
{
struct Callbacks
{
	int success = LUA_NOREF;
	int error = LUA_NOREF;
};

std::unordered_map<unsigned, Callbacks> pending;
unsigned nextId = 1;

/// Attach callbacks to a server data request.
// @function Then
// @tparam function onSuccess Called with the decoded result when the request succeeds.
// @tparam function onError Called with an error string when the request fails.
// @treturn table This request handle.
int PromiseThen( lua_State *L )
{
	lua_getfield( L, 1, "id" );
	unsigned id = static_cast<unsigned>( lua_tointeger( L, -1 ) );
	lua_pop( L, 1 );
	auto it = pending.find( id );
	if ( it == pending.end() )
	{
		return luaL_error( L, "unknown or expired sgame request" );
	}
	luaL_checktype( L, 2, LUA_TFUNCTION );
	luaL_checktype( L, 3, LUA_TFUNCTION );
	if ( it->second.success != LUA_NOREF )
	{
		luaL_unref( L, LUA_REGISTRYINDEX, it->second.success );
	}
	if ( it->second.error != LUA_NOREF )
	{
		luaL_unref( L, LUA_REGISTRYINDEX, it->second.error );
	}
	lua_pushvalue( L, 2 );
	it->second.success = luaL_ref( L, LUA_REGISTRYINDEX );
	lua_pushvalue( L, 3 );
	it->second.error = luaL_ref( L, LUA_REGISTRYINDEX );
	lua_pushvalue( L, 1 );
	return 1;
}

/// Request structured data from the server.
// @function Request
// @tparam string name Name of the registered sgame endpoint.
// @tparam table params JSON-compatible request parameters.
// @treturn table Request handle with a Then(onSuccess, onError) method.
// @usage cgame.Request('ping', { message = 'hello' }):Then(function(reply) print(reply.pong) end,
// function(err) print(err) end)
int Request( lua_State *L )
{
	const char *name = luaL_checkstring( L, 1 );
	if ( pending.size() >= 128 )
	{
		return luaL_error( L, "too many pending sgame requests" );
	}
	unsigned id;
	do
	{
		id = nextId++;
	}
	while ( !id || pending.find( id ) != pending.end() );
	int base = lua_gettop( L );
	std::string json;
	std::string jsonError;
	if ( lua_isnoneornil( L, 2 ) )
	{
		lua_newtable( L );
		bool encoded = Shared::Lua::EncodeJson( L, -1, json, jsonError );
		lua_settop( L, base );
		if ( !encoded )
		{
			return luaL_error( L, "request parameters are not JSON encodable: %s",
			                   jsonError.c_str() );
		}
	}
	else if ( !Shared::Lua::EncodeJson( L, 2, json, jsonError ) )
	{
		return luaL_error( L, "request parameters are not JSON encodable: %s", jsonError.c_str() );
	}
	std::string command = Str::Format( "lua_data_req %u %s %s\n", id, Cmd::Escape( name ).c_str(),
	                                   Cmd::Escape( json ).c_str() );
	if ( command.size() >= 1022 )
	{
		return luaL_error( L, "request exceeds reliable command size limit" );
	}
	pending.emplace( id, Callbacks{} );
	lua_newtable( L );
	lua_pushinteger( L, id );
	lua_setfield( L, -2, "id" );
	lua_pushcfunction( L, PromiseThen );
	lua_setfield( L, -2, "Then" );
	trap_SendClientCommand( command.c_str() );
	return 1;
}
}  // namespace

void CG_Rocket_RegisterDataRequest( lua_State *L )
{
	lua_newtable( L );
	lua_pushcfunction( L, Request );
	lua_setfield( L, -2, "Request" );
	lua_setglobal( L, "cgame" );
}

void CG_Rocket_HandleDataReply()
{
	char idBuffer[ 32 ], jsonBuffer[ MAX_STRING_CHARS ];
	Q_strncpyz( idBuffer, CG_Argv( 1 ), sizeof( idBuffer ) );
	Q_strncpyz( jsonBuffer, CG_Argv( 2 ), sizeof( jsonBuffer ) );
	char *end = nullptr;
	unsigned id = static_cast<unsigned>( std::strtoul( idBuffer, &end, 10 ) );
	auto it = pending.find( id );
	if ( !*idBuffer || !end || *end || it == pending.end() )
	{
		return;
	}
	lua_State *L = Rml::Lua::Interpreter::GetLuaState();
	if ( !L )
	{
		return;
	}
	int base = lua_gettop( L );
	bool ok = false;
	int callbackRef = 0;
	int valueIndex = 0;
	std::string jsonError;
	if ( Shared::Lua::DecodeJson( L, jsonBuffer, jsonError ) )
	{
		if ( lua_istable( L, -1 ) )
		{
			lua_getfield( L, -1, "ok" );
			ok = lua_toboolean( L, -1 );
			lua_pop( L, 1 );
			if ( ok )
			{
				lua_getfield( L, -1, "value" );
			}
			else
			{
				lua_getfield( L, -1, "error" );
			}
			lua_remove( L, -2 );
			valueIndex = lua_gettop( L );
		}
	}
	if ( !valueIndex )
	{
		lua_settop( L, base );
		lua_pushliteral( L, "invalid JSON response from server" );
		valueIndex = lua_gettop( L );
		ok = false;
	}
	Callbacks callbacks = it->second;
	callbackRef = ok ? callbacks.success : callbacks.error;
	pending.erase( it );
	if ( callbackRef == LUA_NOREF )
	{
		lua_settop( L, base );
		if ( callbacks.success != LUA_NOREF )
		{
			luaL_unref( L, LUA_REGISTRYINDEX, callbacks.success );
		}
		if ( callbacks.error != LUA_NOREF )
		{
			luaL_unref( L, LUA_REGISTRYINDEX, callbacks.error );
		}
		return;
	}
	lua_rawgeti( L, LUA_REGISTRYINDEX, callbackRef );
	lua_pushvalue( L, valueIndex );
	if ( lua_pcall( L, 1, 0, 0 ) != LUA_OK )
	{
		Log::Warn( "cgame data request callback failed: %s", lua_tostring( L, -1 ) );
	}
	lua_settop( L, base );
	luaL_unref( L, LUA_REGISTRYINDEX, callbacks.success );
	luaL_unref( L, LUA_REGISTRYINDEX, callbacks.error );
}

void CG_Rocket_ClearDataRequests()
{
	lua_State *L = Rml::Lua::Interpreter::GetLuaState();
	if ( L )
	{
		for ( const auto &request : pending )
		{
			if ( request.second.success != LUA_NOREF )
			{
				luaL_unref( L, LUA_REGISTRYINDEX, request.second.success );
			}
			if ( request.second.error != LUA_NOREF )
			{
				luaL_unref( L, LUA_REGISTRYINDEX, request.second.error );
			}
		}
	}
	pending.clear();
}
