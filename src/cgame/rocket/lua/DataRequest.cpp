#include "cgame/rocket/lua/DataRequest.h"

#include "cgame/rocket/rocket.h"
#include <RmlUi/Lua/Interpreter.h>
#include "cgame/cg_local.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "common/Command.h"
#include "shared/lua/Json.h"
#include "shared/lua/Push.h"

/// Request structured data from the server from cgame Lua.
// @module cgame

namespace
{
struct Callbacks
{
	int success = LUA_NOREF;
	int error = LUA_NOREF;
	std::string response;
	unsigned nextSequence = 0;
	bool assembling = false;
};

std::unordered_map<unsigned, Callbacks> pending;
unsigned nextId = 1;
std::unordered_map<std::string, int> pushHandlers;
std::unordered_set<std::string> appliedPushIds;
unsigned pushSequence = 0;

int RegisterPushHandler( lua_State *L )
{
	const char *name = luaL_checkstring( L, 1 );
	luaL_checktype( L, 2, LUA_TFUNCTION );
	auto it = pushHandlers.find( name );
	if ( it != pushHandlers.end() )
	{
		luaL_unref( L, LUA_REGISTRYINDEX, it->second );
	}
	lua_pushvalue( L, 2 );
	pushHandlers[ name ] = luaL_ref( L, LUA_REGISTRYINDEX );
	return 0;
}

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

void CG_Rocket_RegisterPushCommands( lua_State *L )
{
	lua_getglobal( L, "cgame" );
	if ( !lua_istable( L, -1 ) )
	{
		lua_pop( L, 1 );
		lua_newtable( L );
	}
	lua_pushcfunction( L, RegisterPushHandler );
	lua_setfield( L, -2, "RegisterPushHandler" );
	lua_setglobal( L, "cgame" );
}

void CG_Rocket_PushCommandsReady()
{
	trap_SendClientCommand( "lua_push_reset\n" );
}

void CG_Rocket_HandlePushCommand()
{
	char seqBuffer[ 32 ], idBuffer[ 128 ], kindBuffer[ 8 ], payloadBuffer[ MAX_STRING_CHARS ];
	Q_strncpyz( seqBuffer, CG_Argv( 1 ), sizeof( seqBuffer ) );
	Q_strncpyz( idBuffer, CG_Argv( 2 ), sizeof( idBuffer ) );
	Q_strncpyz( kindBuffer, CG_Argv( 3 ), sizeof( kindBuffer ) );
	Q_strncpyz( payloadBuffer, CG_Argv( 4 ), sizeof( payloadBuffer ) );
	char *end = nullptr;
	unsigned seq = static_cast<unsigned>( std::strtoul( seqBuffer, &end, 10 ) );
	if ( !*seqBuffer || !end || *end || !*idBuffer )
	{
		return;
	}
	std::string id( idBuffer );
	if ( seq <= pushSequence && appliedPushIds.count( id ) )
	{
		trap_SendClientCommand( Str::Format( "lua_push_ack %u\n", pushSequence ).c_str() );
		return;
	}
	if ( seq != pushSequence + 1 )
	{
		trap_SendClientCommand( "lua_push_resume\n" );
		return;
	}
	lua_State *L = Rml::Lua::Interpreter::GetLuaState();
	if ( !L )
	{
		return;
	}
	std::string payload( payloadBuffer );
	int base = lua_gettop( L );
	std::string jsonError;
	bool ok = Shared::Lua::DecodeJson( L, payload.c_str(), jsonError );
	int payloadIndex = 0;
	if ( ok && lua_istable( L, -1 ) )
	{
		payloadIndex = lua_absindex( L, -1 );
	}
	else
	{
		ok = false;
	}
	std::string name;
	if ( ok && kindBuffer[ 0 ] == Shared::Lua::Push::SKIP )
	{
		// A retracted command still consumes its original stream position.
	}
	else if ( ok && kindBuffer[ 0 ] == Shared::Lua::Push::HANDLER )
	{
		lua_getfield( L, payloadIndex, "handler" );
		if ( lua_type( L, -1 ) == LUA_TSTRING )
		{
			name = lua_tostring( L, -1 );
		}
		else
		{
			ok = false;
		}
		lua_pop( L, 1 );
		auto handler = pushHandlers.find( name );
		if ( handler == pushHandlers.end() )
		{
			ok = false;
		}
		else
		{
			lua_getfield( L, payloadIndex, "data" );
			lua_rawgeti( L, LUA_REGISTRYINDEX, handler->second );
			lua_insert( L, -2 );
			ok = lua_pcall( L, 1, 0, 0 ) == LUA_OK;
		}
	}
	else if ( ok && kindBuffer[ 0 ] == Shared::Lua::Push::LUA )
	{
		lua_getfield( L, payloadIndex, "source" );
		if ( lua_type( L, -1 ) == LUA_TSTRING )
		{
			name = lua_tostring( L, -1 );
		}
		else
		{
			ok = false;
		}
		lua_pop( L, 1 );
		std::string wrapped = "local data = ...\n" + name;
		lua_getfield( L, payloadIndex, "data" );
		if ( ok && luaL_loadstring( L, wrapped.c_str() ) == LUA_OK )
		{
			lua_insert( L, -2 );
			ok = lua_pcall( L, 1, 0, 0 ) == LUA_OK;
		}
		else
		{
			ok = false;
		}
	}
	else
	{
		ok = false;
	}
	if ( !ok )
	{
		const char *error = lua_tostring( L, -1 );
		Log::Warn( "cgame push command %s at sequence %u rejected: %s", id.c_str(), seq,
		           error ? error : "invalid payload or unknown handler" );
		lua_settop( L, base );
		pushSequence = seq;
		appliedPushIds.insert( id );
		trap_SendClientCommand( Str::Format( "lua_push_nack %u\n", seq ).c_str() );
		return;
	}
	lua_settop( L, base );
	appliedPushIds.insert( id );
	pushSequence = seq;
	trap_SendClientCommand( Str::Format( "lua_push_ack %u\n", pushSequence ).c_str() );
}

void CG_Rocket_ClearPushCommands()
{
	lua_State *L = Rml::Lua::Interpreter::GetLuaState();
	if ( L )
	{
		for ( const auto &handler : pushHandlers )
		{
			luaL_unref( L, LUA_REGISTRYINDEX, handler.second );
		}
	}
	pushHandlers.clear();
}

void CG_Rocket_ResetPushCommands()
{
	pushSequence = 0;
	appliedPushIds.clear();
	// Client commands sent during CG_Init can be dropped before ClientBegin.
	// Re-send readiness after the server's ClientBegin reset reaches cgame.
	trap_SendClientCommand( "lua_push_reset\n" );
	Rocket_ReinitializeScoreboard();
}

namespace
{
void DeliverReply( unsigned id, const char *jsonBuffer )
{
	auto it = pending.find( id );
	if ( it == pending.end() )
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
}  // namespace

void CG_Rocket_HandleDataReply()
{
	char idBuffer[ 32 ], jsonBuffer[ MAX_STRING_CHARS ];
	Q_strncpyz( idBuffer, CG_Argv( 1 ), sizeof( idBuffer ) );
	Q_strncpyz( jsonBuffer, CG_Argv( 2 ), sizeof( jsonBuffer ) );
	char *end = nullptr;
	unsigned id = static_cast<unsigned>( std::strtoul( idBuffer, &end, 10 ) );
	if ( !*idBuffer || !end || *end )
	{
		return;
	}
	if ( pending.find( id ) == pending.end() )
	{
		return;
	}
	trap_SendClientCommand( Str::Format( "lua_data_ack %u -1\n", id ).c_str() );
	DeliverReply( id, jsonBuffer );
}

void CG_Rocket_HandleDataChunk()
{
	char idBuffer[ 32 ], sequenceBuffer[ 32 ], part[ MAX_STRING_CHARS ];
	char doneBuffer[ 8 ], truncatedBuffer[ 8 ];
	Q_strncpyz( idBuffer, CG_Argv( 1 ), sizeof( idBuffer ) );
	Q_strncpyz( sequenceBuffer, CG_Argv( 2 ), sizeof( sequenceBuffer ) );
	Q_strncpyz( part, CG_Argv( 3 ), sizeof( part ) );
	Q_strncpyz( doneBuffer, CG_Argv( 4 ), sizeof( doneBuffer ) );
	Q_strncpyz( truncatedBuffer, CG_Argv( 5 ), sizeof( truncatedBuffer ) );
	char *idEnd = nullptr, *sequenceEnd = nullptr;
	unsigned id = static_cast<unsigned>( std::strtoul( idBuffer, &idEnd, 10 ) );
	unsigned sequence = static_cast<unsigned>( std::strtoul( sequenceBuffer, &sequenceEnd, 10 ) );
	auto it = pending.find( id );
	if ( !*idBuffer || !idEnd || *idEnd || !sequenceEnd || *sequenceEnd || it == pending.end() ||
	     sequence != it->second.nextSequence )
	{
		return;
	}
	Callbacks &callbacks = it->second;
	callbacks.assembling = true;
	callbacks.response += part;
	callbacks.nextSequence++;
	if ( std::atoi( truncatedBuffer ) != 0 )
	{
		trap_SendClientCommand( Str::Format( "lua_data_ack %u -1\n", id ).c_str() );
		lua_State *L = Rml::Lua::Interpreter::GetLuaState();
		if ( !L )
		{
			return;
		}
		Callbacks finished = callbacks;
		pending.erase( it );
		int base = lua_gettop( L );
		if ( finished.error != LUA_NOREF )
		{
			lua_rawgeti( L, LUA_REGISTRYINDEX, finished.error );
			lua_pushliteral( L, "response truncated by server" );
			if ( lua_pcall( L, 1, 0, 0 ) != LUA_OK )
			{
				Log::Warn( "cgame data request callback failed: %s", lua_tostring( L, -1 ) );
			}
			lua_settop( L, base );
		}
		if ( finished.success != LUA_NOREF )
		{
			luaL_unref( L, LUA_REGISTRYINDEX, finished.success );
		}
		if ( finished.error != LUA_NOREF )
		{
			luaL_unref( L, LUA_REGISTRYINDEX, finished.error );
		}
		return;
	}
	if ( std::atoi( doneBuffer ) != 0 )
	{
		trap_SendClientCommand( Str::Format( "lua_data_ack %u -1\n", id ).c_str() );
		std::string response = std::move( callbacks.response );
		DeliverReply( id, response.c_str() );
		return;
	}
	trap_SendClientCommand( Str::Format( "lua_data_ack %u %u\n", id, sequence ).c_str() );
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
