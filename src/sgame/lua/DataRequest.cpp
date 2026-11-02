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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/Command.h"
#include "sgame/lua/Entity.h"
#include "sgame/lua/EntityProxy.h"
#include "sgame/lua/Interpreter.h"
#include "shared/lua/Json.h"
#include "shared/lua/LuaLib.h"
#include "shared/lua/Push.h"

namespace Lua
{
namespace
{
// Endpoint name -> Lua registry reference for the endpoint function.
std::unordered_map<std::string, int> endpoints;

constexpr size_t MAX_COMMAND_SIZE = 800;
constexpr size_t MAX_SERVER_COMMAND_SIZE = 1022;
constexpr unsigned MAX_RESPONSE_CHUNKS = 10;
// Make sure we don't take up too much space for any single command.
constexpr size_t MAX_RESPONSE_BYTES = MAX_COMMAND_SIZE * MAX_RESPONSE_CHUNKS;
// Allows the stress harness's three concurrent large transfers with headroom.
constexpr unsigned MAX_OUTSTANDING_REQUESTS_PER_CLIENT = 4;
constexpr int STREAM_TIMEOUT_MSEC = 60000;
constexpr int CHUNK_INTERVAL_MSEC = 200;

struct StreamState
{
	std::string response;
	size_t offset = 0;
	unsigned nextSequence = 0;
	int waitingSequence = -1;
	bool pendingAck = false;
};

// Stream key is (client number, request id).
using StreamKey = std::pair<int, unsigned>;
// Stream key -> buffered response and flow-control state for that transfer.
std::map<StreamKey, StreamState> streams;
// (client number, request id) -> last time the server is waiting for completion.
std::map<StreamKey, int> outstandingRequests;
// Client number -> timestamps of accepted new data requests, in milliseconds.
std::unordered_map<int, std::deque<int>> requestTimes;
// Client number -> time the last response chunk was sent, in milliseconds.
std::unordered_map<int, int> lastChunkTime;

struct PushRecord
{
	uint64_t id = 0;
	char kind = Shared::Lua::Push::SKIP;
	std::string payload;
	unsigned sequence = 0;
	int sentAt = 0;
	unsigned retries = 0;
	bool exhaustionReported = false;
};

// Client number -> ordered, unacknowledged push records.
std::unordered_map<int, std::vector<PushRecord>> outstandingPushes;
// Client number -> highest sequence acknowledged by that client.
std::unordered_map<int, unsigned> pushAck;
// Client number -> highest sequence assigned in that client's stream.
std::unordered_map<int, unsigned> pushSequence;
// Client numbers whose cgame has completed the push readiness handshake.
std::unordered_set<int> pushReady;
// Client GUID -> IDs of persistent pushes already queued for that client.
std::unordered_map<std::string, std::unordered_set<uint64_t>> persistentRecipients;
// Group pushes are retained so newly joining clients and team changes can be
// evaluated against the current target. Per-client queues remain the transport.
enum class PushTargetGroup
{
	NONE,
	ALL,
	HUMANS,
	ALIENS
};

struct PersistentPush
{
	uint64_t id = 0;
	char kind;
	std::string payload;
	PushTargetGroup target = PushTargetGroup::NONE;
};
std::vector<PersistentPush> persistentPushes;

PushTargetGroup ParsePushTargetGroup( const char *target )
{
	if ( !Q_stricmp( target, "all" ) )
	{
		return PushTargetGroup::ALL;
	}
	if ( !Q_stricmp( target, "humans" ) )
	{
		return PushTargetGroup::HUMANS;
	}
	if ( !Q_stricmp( target, "aliens" ) )
	{
		return PushTargetGroup::ALIENS;
	}
	return PushTargetGroup::NONE;
}

std::string PushRecipientKey( int clientNum )
{
	if ( clientNum < 0 || clientNum >= level.maxclients || level.clients[ clientNum ].pers.isBot )
	{
		return "";
	}
	const std::string &guid = level.clients[ clientNum ].pers.guid;
	return guid;
}

void MarkPersistentRecipients( uint64_t id, PushTargetGroup target )
{
	if ( target == PushTargetGroup::NONE )
	{
		return;
	}
	for ( int clientNum = 0; clientNum < level.maxclients; clientNum++ )
	{
		const gclient_t &client = level.clients[ clientNum ];
		if ( client.pers.connected != CON_CONNECTED || client.pers.isBot )
		{
			continue;
		}
		std::string key = PushRecipientKey( clientNum );
		if ( key.empty() )
		{
			continue;
		}
		bool matches = target == PushTargetGroup::ALL ||
		               ( target == PushTargetGroup::HUMANS && client.pers.team == TEAM_HUMANS ) ||
		               ( target == PushTargetGroup::ALIENS && client.pers.team == TEAM_ALIENS );
		if ( matches )
		{
			persistentRecipients[ key ].insert( id );
		}
	}
}

constexpr size_t MAX_PUSH_RECORDS = 128;
constexpr size_t MAX_PUSH_BYTES = MAX_PUSH_RECORDS * ( MAX_SERVER_COMMAND_SIZE - 1 );
constexpr unsigned MAX_PUSH_RETRIES = 5;
constexpr int PUSH_RETRY_MSEC = 3000;
constexpr uint64_t MAX_SAFE_LUA_INTEGER = 9007199254740991ULL;
uint64_t nextPushId = 1;

uint64_t AllocatePushId()
{
	if ( nextPushId > MAX_SAFE_LUA_INTEGER )
	{
		return 0;
	}
	return nextPushId++;
}

bool IsValidPushKind( char kind )
{
	return kind == Shared::Lua::Push::HANDLER || kind == Shared::Lua::Push::LUA ||
	       kind == Shared::Lua::Push::SKIP;
}

void SendPushRecord( int clientNum, PushRecord &record )
{
	if ( !IsValidPushKind( record.kind ) )
	{
		Log::Warn( "Refusing to send Lua push %llu with invalid kind %d",
		           static_cast<unsigned long long>( record.id ), static_cast<int>( record.kind ) );
		return;
	}
	std::string command =
		Str::Format( "lua_push %u %s %c %s", record.sequence,
		             std::to_string( record.id ).c_str(), record.kind,
		             Cmd::Escape( record.payload ).c_str() );
	if ( command.size() < MAX_SERVER_COMMAND_SIZE )
	{
		trap_SendServerCommand( clientNum, command.c_str() );
		record.sentAt = level.time;
	}
	else
	{
		Log::Warn( "Lua push command for client %d is %zu bytes; maximum is %zu: %s", clientNum,
		           command.size(), MAX_SERVER_COMMAND_SIZE - 1, command.c_str() );
	}
}

bool AddPush( int clientNum, uint64_t id, char kind, const std::string &payload )
{
	if ( !IsValidPushKind( kind ) )
	{
		Log::Warn( "Refusing to queue Lua push %llu with invalid kind %d",
		           static_cast<unsigned long long>( id ), static_cast<int>( kind ) );
		return false;
	}
	if ( clientNum < 0 || clientNum >= level.maxclients ||
	     level.clients[ clientNum ].pers.connected != CON_CONNECTED ||
	     level.clients[ clientNum ].pers.isBot )
	{
		return false;
	}
	// Use UINT_MAX as the sequence size as a
	// conservative max possible length of command.
	std::string command =
		Str::Format( "lua_push %u %s %c %s", UINT32_MAX, std::to_string( id ).c_str(), kind,
	                 Cmd::Escape( payload ).c_str() );
	if ( command.size() >= MAX_SERVER_COMMAND_SIZE )
	{
		Log::Warn( "Lua push command for client %d is %zu bytes; maximum is %zu: %s", clientNum,
		           command.size(), MAX_SERVER_COMMAND_SIZE - 1, command.c_str() );
		return false;
	}
	auto &history = outstandingPushes[ clientNum ];
	for ( auto &record : history )
	{
		if ( record.id == id )
		{
			record.kind = kind;
			record.payload = payload;
			record.retries = 0;
			record.sentAt = 0;
			return true;
		}
	}
	size_t bytes = payload.size();
	for ( const auto &record : history )
	{
		bytes += record.payload.size();
	}
	if ( history.size() >= MAX_PUSH_RECORDS || bytes > MAX_PUSH_BYTES )
	{
		return false;
	}
	PushRecord record;
	record.id = id;
	record.kind = kind;
	record.payload = payload;
	record.sequence = ++pushSequence[ clientNum ];
	history.push_back( std::move( record ) );
	if ( pushReady.count( clientNum ) && history.size() == 1 )
	{
		SendPushRecord( clientNum, history.back() );
	}
	return true;
}

bool PushTarget( lua_State *L, int targetIndex, uint64_t id, char kind,
                 const std::string &payload, PushTargetGroup &group )
{
	std::vector<int> clients;
	group = PushTargetGroup::NONE;
	// Symbolic target that needs to be resolved.
	if ( lua_type( L, targetIndex ) == LUA_TSTRING )
	{
		group = ParsePushTargetGroup( lua_tostring( L, targetIndex ) );
		if ( group == PushTargetGroup::NONE )
		{
			return false;
		}
		for ( int i = 0; i < level.maxclients; i++ )
		{
			const auto &client = level.clients[ i ];
			bool matches = group == PushTargetGroup::ALL ||
			               ( group == PushTargetGroup::HUMANS && client.pers.team == TEAM_HUMANS ) ||
			               ( group == PushTargetGroup::ALIENS && client.pers.team == TEAM_ALIENS );
			if ( client.pers.connected == CON_CONNECTED && !client.pers.isBot && matches )
			{
				clients.push_back( i );
			}
		}
	}
	// Direct client slot number.
	else if ( lua_type( L, targetIndex ) == LUA_TNUMBER )
	{
		int target = static_cast<int>( lua_tointeger( L, targetIndex ) );
		if ( target == -1 )
		{
			for ( int i = 0; i < level.maxclients; i++ )
			{
				if ( level.clients[ i ].pers.connected == CON_CONNECTED &&
				     !level.clients[ i ].pers.isBot )
				{
					clients.push_back( i );
				}
			}
		}
		else
		{
			clients.push_back( target );
		}
	}
	// List of clients.
	else if ( lua_istable( L, targetIndex ) )
	{
		lua_pushnil( L );
		while ( lua_next( L, targetIndex ) )
		{
			if ( lua_isnumber( L, -1 ) )
			{
				clients.push_back( static_cast<int>( lua_tointeger( L, -1 ) ) );
			}
			else
			{
				lua_pop( L, 2 );
				return false;
			}
			lua_pop( L, 1 );
		}
	}
	else
	{
		Log::Warn( "Unknown target type: %d: %s", lua_type( L, targetIndex ), payload );
		return false;
	}
	for ( int client : clients )
	{
		if ( client < 0 || client >= level.maxclients ||
		     level.clients[ client ].pers.connected != CON_CONNECTED ||
		     level.clients[ client ].pers.isBot )
		{
			return false;
		}
	}
	std::sort( clients.begin(), clients.end() );
	clients.erase( std::unique( clients.begin(), clients.end() ), clients.end() );
	// Use UINT_MAX as the sequence size as a
	// conservative max possible length of command.
	std::string command =
		Str::Format( "lua_push %u %s %c %s", UINT32_MAX, std::to_string( id ).c_str(), kind,
	                 Cmd::Escape( payload ).c_str() );
	if ( command.size() >= MAX_SERVER_COMMAND_SIZE )
	{
		return false;
	}
	for ( int client : clients )
	{
		const auto &history = outstandingPushes[ client ];
		size_t bytes = payload.size();
		for ( const auto &record : history )
		{
			bytes += record.payload.size();
		}
		if ( history.size() >= MAX_PUSH_RECORDS || bytes > MAX_PUSH_BYTES ||
		     pushSequence[ client ] == UINT32_MAX )
		{
			return false;
		}
	}
	for ( int client : clients )
	{
		AddPush( client, id, kind, payload );
	}
	return true;
}

bool EncodePushPayload( lua_State *L, const char *nameField, const char *name, int dataIndex,
                        std::string &payload, std::string &error )
{
	int base = lua_gettop( L );
	lua_newtable( L );
	lua_pushstring( L, name );
	lua_setfield( L, -2, nameField );
	if ( dataIndex == 0 )
	{
		lua_newtable( L );
	}
	else
	{
		lua_pushvalue( L, dataIndex );
	}
	lua_setfield( L, -2, "data" );
	bool ok = Shared::Lua::EncodeJson( L, -1, payload, error );
	lua_settop( L, base );
	return ok;
}

unsigned OutstandingRequestsForClient( int clientNum )
{
	unsigned count = 0;
	auto it = outstandingRequests.lower_bound( StreamKey( clientNum, 0 ) );
	while ( it != outstandingRequests.end() && it->first.first == clientNum )
	{
		count++;
		++it;
	}
	return count;
}

std::string ReplyCommand( unsigned long id, const std::string &encoded )
{
	return Str::Format( "lua_data_reply %lu %s", id, Cmd::Escape( encoded ).c_str() );
}

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

void SendReply( int clientNum, unsigned long id, const std::string &encoded )
{
	std::string command = ReplyCommand( id, encoded );
	if ( command.size() < MAX_SERVER_COMMAND_SIZE )
	{
		trap_SendServerCommand( clientNum, command.c_str() );
	}
}

void SendTransportError( int clientNum, unsigned long id, const char *error )
{
	lua_State *L = State();
	if ( !L )
	{
		return;
	}
	int base = lua_gettop( L );
	std::string encoded, jsonError;
	PushErrorEnvelope( L, error );
	if ( Shared::Lua::EncodeJson( L, -1, encoded, jsonError ) )
	{
		SendReply( clientNum, id, encoded );
	}
	lua_settop( L, base );
}

std::string ChunkCommand( unsigned id, unsigned sequence, const std::string &part, bool done,
                          bool truncated )
{
	return Str::Format( "lua_data_chunk %u %u %s %d %d", id, sequence, Cmd::Escape( part ).c_str(),
	                    done ? 1 : 0, truncated ? 1 : 0 );
}

bool SendNextChunk( const StreamKey &key, StreamState &stream )
{
	const size_t remaining = stream.response.size() - stream.offset;
	size_t low = 1;
	size_t high = std::min( remaining, MAX_COMMAND_SIZE );
	size_t best = 0;
	while ( low <= high )
	{
		size_t middle = low + ( high - low ) / 2;
		bool atCap = stream.nextSequence + 1 >= MAX_RESPONSE_CHUNKS;
		bool done = middle == remaining || atCap;
		bool truncated = atCap && middle < remaining;
		std::string command =
			ChunkCommand( key.second, stream.nextSequence,
		                  stream.response.substr( stream.offset, middle ), done, truncated );
		if ( command.size() <= MAX_COMMAND_SIZE )
		{
			best = middle;
			low = middle + 1;
		}
		else
		{
			high = middle - 1;
		}
	}
	if ( !best )
	{
		SendTransportError( key.first, key.second, "response could not be split safely" );
		return false;
	}

	bool atCap = stream.nextSequence + 1 >= MAX_RESPONSE_CHUNKS;
	bool done = best == remaining || atCap;
	bool truncated = atCap && best < remaining;
	std::string command =
		ChunkCommand( key.second, stream.nextSequence,
	                  stream.response.substr( stream.offset, best ), done, truncated );
	trap_SendServerCommand( key.first, command.c_str() );
	stream.offset += best;
	outstandingRequests[ key ] = level.time;
	lastChunkTime[ key.first ] = level.time;
	if ( truncated )
	{
		Log::Warn( "Lua data response for client %d (request %u) truncated after %u chunks",
		           key.first, key.second, MAX_RESPONSE_CHUNKS );
		return false;
	}
	if ( done )
	{
		return false;
	}
	stream.waitingSequence = static_cast<int>( stream.nextSequence );
	stream.nextSequence++;
	stream.pendingAck = false;
	return true;
}

bool AllowRequest( int clientNum )
{
	std::deque<int> &times = requestTimes[ clientNum ];
	while ( !times.empty() && level.time - times.front() >= 60000 )
	{
		times.pop_front();
	}
	unsigned recentSecond = 0;
	for ( int time : times )
	{
		if ( level.time - time < 1000 )
		{
			recentSecond++;
		}
	}
	if ( recentSecond >= 5 || times.size() >= 20 )
	{
		return false;
	}
	times.push_back( level.time );
	return true;
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

int PushCommand( lua_State *L )
{
	int target = lua_absindex( L, 1 );
	const char *handler = luaL_checkstring( L, 2 );
	uint64_t id = AllocatePushId();
	if ( !id )
	{
		return luaL_error( L, "push command ID space exhausted" );
	}
	std::string payload, error;
	bool ok =
		EncodePushPayload( L, "handler", handler, lua_isnoneornil( L, 3 ) ? 0 : 3, payload, error );
	if ( !ok )
	{
		return luaL_error( L, "push command data is not JSON encodable: %s", error.c_str() );
	}
	PushTargetGroup group;
	if ( !PushTarget( L, target, id, Shared::Lua::Push::HANDLER, payload, group ) )
	{
		return luaL_error( L, "invalid target or push command limit exceeded" );
	}
	if ( group != PushTargetGroup::NONE )
	{
		persistentPushes.push_back( { id, Shared::Lua::Push::HANDLER, payload, group } );
		MarkPersistentRecipients( id, group );
	}
	lua_pushnumber( L, static_cast<lua_Number>( id ) );
	return 1;
}

int PushLua( lua_State *L )
{
	int target = lua_absindex( L, 1 );
	const char *source = luaL_checkstring( L, 2 );
	uint64_t id = AllocatePushId();
	if ( !id )
	{
		return luaL_error( L, "push command ID space exhausted" );
	}
	std::string data, error;
	bool ok =
		EncodePushPayload( L, "source", source, lua_isnoneornil( L, 3 ) ? 0 : 3, data, error );
	if ( !ok )
	{
		return luaL_error( L, "push Lua data is not JSON encodable: %s", error.c_str() );
	}
	PushTargetGroup group;
	if ( !PushTarget( L, target, id, Shared::Lua::Push::LUA, data, group ) )
	{
		return luaL_error( L, "invalid target or push command limit exceeded" );
	}
	if ( group != PushTargetGroup::NONE )
	{
		persistentPushes.push_back( { id, Shared::Lua::Push::LUA, data, group } );
		MarkPersistentRecipients( id, group );
	}
	lua_pushnumber( L, static_cast<lua_Number>( id ) );
	return 1;
}

int RetractCommand( lua_State *L )
{
	int target = lua_absindex( L, 1 );
	lua_Number numericId = luaL_checknumber( L, 2 );
	if ( !std::isfinite( numericId ) || numericId < 1 ||
	     numericId > static_cast<lua_Number>( MAX_SAFE_LUA_INTEGER ) ||
	     std::floor( numericId ) != numericId )
	{
		return luaL_error( L, "invalid push command ID" );
	}
	uint64_t id = static_cast<uint64_t>( numericId );
	bool wasPersistent = std::any_of( persistentPushes.begin(), persistentPushes.end(),
	                                  [ &id ]( const PersistentPush &push )
	                                  { return push.id == id; } );
	persistentPushes.erase( std::remove_if( persistentPushes.begin(), persistentPushes.end(),
	                                      [ &id ]( const PersistentPush &push )
	                                      { return push.id == id; } ),
	                       persistentPushes.end() );
	if ( wasPersistent )
	{
		for ( auto &recipient : persistentRecipients )
		{
			recipient.second.erase( id );
		}
		for ( auto &client : outstandingPushes )
		{
			for ( PushRecord &record : client.second )
			{
				if ( record.id == id )
				{
					record.kind = Shared::Lua::Push::SKIP;
					record.payload = "{}";
					record.sentAt = 0;
					record.retries = 0;
					record.exhaustionReported = false;
				}
			}
		}
		return 0;
	}
	std::vector<int> clients;
	if ( lua_type( L, target ) == LUA_TNUMBER && lua_tointeger( L, target ) == -1 )
	{
		for ( const auto &entry : outstandingPushes )
		{
			clients.push_back( entry.first );
		}
	}
	else if ( lua_istable( L, target ) )
	{
		lua_pushnil( L );
		while ( lua_next( L, target ) )
		{
			clients.push_back( static_cast<int>( luaL_checkinteger( L, -1 ) ) );
			lua_pop( L, 1 );
		}
	}
	else
	{
		clients.push_back( static_cast<int>( luaL_checkinteger( L, target ) ) );
	}
	for ( int client : clients )
	{
		auto it = outstandingPushes.find( client );
		if ( it == outstandingPushes.end() )
		{
			continue;
		}
		// Keep the position in the stream: the original may already be in flight.
		for ( auto &record : it->second )
		{
			if ( record.id == id )
			{
				record.kind = Shared::Lua::Push::SKIP;
				record.payload = "{}";
				record.sentAt = 0;
				record.retries = 0;
				record.exhaustionReported = false;
			}
		}
	}
	return 0;
}

bool HandlePushAck( int clientNum )
{
	const Cmd::Args &args = trap_Args();
	if ( args.Argc() != 2 )
	{
		return true;
	}
	char *end = nullptr;
	unsigned long value = std::strtoul( args.Argv( 1 ).c_str(), &end, 10 );
	if ( !end || *end )
	{
		return true;
	}
	unsigned ack = static_cast<unsigned>( value );
	auto &history = outstandingPushes[ clientNum ];
	if ( ( history.empty() && ack == pushAck[ clientNum ] ) ||
	     ( ack <= pushSequence[ clientNum ] ) )
	{
		pushAck[ clientNum ] = std::max( pushAck[ clientNum ], ack );
	}
	history.erase( std::remove_if( history.begin(), history.end(), [ & ]( const PushRecord &r )
	                               { return r.sequence <= pushAck[ clientNum ]; } ),
	               history.end() );
	return true;
}

bool HandlePushNack( int clientNum )
{
	const Cmd::Args &args = trap_Args();
	if ( args.Argc() != 2 )
	{
		return true;
	}
	char *end = nullptr;
	unsigned long rawSequence = std::strtoul( args.Argv( 1 ).c_str(), &end, 10 );
	if ( !end || *end || rawSequence > UINT32_MAX )
	{
		return true;
	}
	auto found = outstandingPushes.find( clientNum );
	if ( found == outstandingPushes.end() )
	{
		return true;
	}
	unsigned sequence = static_cast<unsigned>( rawSequence );
	auto &history = found->second;
	auto record = std::find_if( history.begin(), history.end(), [ sequence ]( const PushRecord &push )
	{
		return push.sequence == sequence;
	} );
	if ( record == history.end() )
	{
		return true;
	}
	Log::Warn( "Lua push: discarding NACKed command %llu for client %d",
	           static_cast<unsigned long long>( record->id ), clientNum );
	history.erase( record );
	pushAck[ clientNum ] = sequence;
	for ( PushRecord &next : history )
	{
		if ( next.sequence > sequence )
		{
			next.sentAt = 0;
			SendPushRecord( clientNum, next );
			break;
		}
	}
	return true;
}

void InitializePushHistory( int clientNum )
{
	if ( outstandingPushes.find( clientNum ) != outstandingPushes.end() )
	{
		return;
	}
	outstandingPushes[ clientNum ];
}

bool HandlePushResume( int clientNum )
{
	if ( !pushReady.count( clientNum ) )
	{
		return true;
	}
	InitializePushHistory( clientNum );
	auto &history = outstandingPushes[ clientNum ];
	for ( auto &r : history )
	{
		if ( r.sequence > pushAck[ clientNum ] )
		{
			r.sentAt = 0;
			SendPushRecord( clientNum, r );
			break;
		}
	}
	return true;
}

bool HandlePushReset( int clientNum )
{
	std::string recipientKey = PushRecipientKey( clientNum );
	if ( recipientKey.empty() )
	{
		return true;
	}
	persistentRecipients.erase( recipientKey );
	QueuePersistentPushes( clientNum );
	auto &history = outstandingPushes[ clientNum ];
	pushAck[ clientNum ] = 0;
	pushSequence[ clientNum ] = 0;
	for ( auto &record : history )
	{
		record.sequence = ++pushSequence[ clientNum ];
		record.sentAt = 0;
		record.retries = 0;
		record.exhaustionReported = false;
	}
	pushReady.insert( clientNum );
	if ( !history.empty() )
	{
		SendPushRecord( clientNum, history.front() );
	}
	return true;
}

void QueuePersistentPushes( int clientNum )
{
	if ( clientNum < 0 || clientNum >= level.maxclients ||
	     level.clients[ clientNum ].pers.connected != CON_CONNECTED ||
	     level.clients[ clientNum ].pers.isBot )
	{
		return;
	}
	std::string recipientKey = PushRecipientKey( clientNum );
	if ( recipientKey.empty() )
	{
		return;
	}
	int team = level.clients[ clientNum ].pers.team;
	auto &history = outstandingPushes[ clientNum ];
	history.erase( std::remove_if( history.begin(), history.end(), [ team ]( const PushRecord &record )
	{
		auto push = std::find_if( persistentPushes.begin(), persistentPushes.end(),
		                         [ &record ]( const PersistentPush &candidate )
		                         { return candidate.id == record.id; } );
		if ( push == persistentPushes.end() )
		{
			return false;
		}
		return ( push->target == PushTargetGroup::HUMANS && team != TEAM_HUMANS ) ||
		       ( push->target == PushTargetGroup::ALIENS && team != TEAM_ALIENS );
	} ), history.end() );
	for ( const PersistentPush &push : persistentPushes )
	{
		bool matches = push.target == PushTargetGroup::ALL ||
		               ( push.target == PushTargetGroup::HUMANS && team == TEAM_HUMANS ) ||
		               ( push.target == PushTargetGroup::ALIENS && team == TEAM_ALIENS );
		auto &delivered = persistentRecipients[ recipientKey ];
		if ( matches && !delivered.count( push.id ) )
		{
			if ( AddPush( clientNum, push.id, push.kind, push.payload ) )
			{
				delivered.insert( push.id );
			}
		}
	}
	std::stable_sort( history.begin(), history.end(), []( const PushRecord &left,
	                                                      const PushRecord &right )
	{
		return left.id < right.id;
	} );
	pushSequence[ clientNum ] = 0;
	for ( PushRecord &record : history )
	{
		record.sequence = ++pushSequence[ clientNum ];
	}
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
	if ( !AllowRequest( clientNum ) )
	{
		SendTransportError( clientNum, id, "data request rate limit exceeded" );
		return true;
	}
	if ( OutstandingRequestsForClient( clientNum ) >= MAX_OUTSTANDING_REQUESTS_PER_CLIENT )
	{
		SendTransportError( clientNum, id, "too many outstanding data requests" );
		return true;
	}
	auto endpoint = endpoints.find( name );
	lua_State *L = State();
	if ( !L )
	{
		return true;
	}
	StreamKey key( clientNum, static_cast<unsigned>( id ) );
	if ( outstandingRequests.count( key ) )
	{
		return true;
	}
	// Small replies and active streams are both outstanding until cgame confirms completion.
	outstandingRequests.emplace( key, level.time );
	int base = lua_gettop( L );
	bool success = false;
	bool responseTooLarge = false;
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
			if ( success && encoded.size() > MAX_RESPONSE_BYTES )
			{
				success = false;
				responseTooLarge = true;
			}
		}
	}
	lua_settop( L, base );
	if ( !success )
	{
		if ( responseTooLarge )
		{
			Log::Warn( "Lua data response for client %d (request %lu) exceeded the %zu-byte limit",
			           clientNum, id, MAX_RESPONSE_BYTES );
		}
		PushErrorEnvelope( L, responseTooLarge ? "response exceeds transport size limit"
		                      : endpoint == endpoints.end() ? "unknown endpoint"
		                                                    : "invalid request or endpoint error" );
		Shared::Lua::EncodeJson( L, -1, encoded, jsonError );
		lua_settop( L, base );
	}
	std::string command = ReplyCommand( id, encoded );
	if ( command.size() <= MAX_COMMAND_SIZE )
	{
		trap_SendServerCommand( clientNum, command.c_str() );
	}
	else
	{
		auto previous = streams.find( key );
		if ( previous != streams.end() )
		{
			streams.erase( previous );
		}
		StreamState stream;
		stream.response = encoded;
		auto inserted = streams.emplace( key, std::move( stream ) );
		if ( !SendNextChunk( key, inserted.first->second ) )
		{
			streams.erase( inserted.first );
		}
	}
	return true;
}

bool HandleDataRequestAck( int clientNum )
{
	const Cmd::Args &args = trap_Args();
	if ( args.Argc() != 3 )
	{
		return true;
	}
	const std::string &sequenceArg = args.Argv( 2 );
	if ( sequenceArg.empty() )
	{
		return true;
	}
	char *idEnd = nullptr;
	char *sequenceEnd = nullptr;
	unsigned long id = std::strtoul( args.Argv( 1 ).c_str(), &idEnd, 10 );
	long sequence = std::strtol( sequenceArg.c_str(), &sequenceEnd, 10 );
	if ( !idEnd || *idEnd || !sequenceEnd || *sequenceEnd || sequence < -1 )
	{
		return true;
	}
	StreamKey key( clientNum, static_cast<unsigned>( id ) );
	auto outstanding = outstandingRequests.find( key );
	if ( outstanding == outstandingRequests.end() )
	{
		return true;
	}
	if ( sequence == -1 )
	{
		outstandingRequests.erase( outstanding );
		streams.erase( key );
		return true;
	}
	auto it = streams.find( key );
	if ( it == streams.end() || it->second.waitingSequence != sequence )
	{
		return true;
	}
	outstanding->second = level.time;
	int last = lastChunkTime[ clientNum ];
	if ( level.time - last < CHUNK_INTERVAL_MSEC )
	{
		it->second.pendingAck = true;
		return true;
	}
	if ( !SendNextChunk( it->first, it->second ) )
	{
		streams.erase( it );
	}
	return true;
}

void UpdateDataRequestStreams()
{
	for ( int clientNum = 0; clientNum < level.maxclients; clientNum++ )
	{
		if ( level.clients[ clientNum ].pers.connected == CON_CONNECTED &&
		     !level.clients[ clientNum ].pers.isBot )
		{
			InitializePushHistory( clientNum );
		}
	}
	for ( auto &client : outstandingPushes )
	{
		int clientNum = client.first;
		if ( clientNum < 0 || clientNum >= level.maxclients ||
		     level.clients[ clientNum ].pers.connected != CON_CONNECTED )
		{
			continue;
		}
		for ( auto &record : client.second )
		{
			if ( !pushReady.count( clientNum ) )
			{
				break;
			}
			if ( record.sequence <= pushAck[ clientNum ] )
			{
				continue;
			}
			if ( !record.sentAt || level.time - record.sentAt >= PUSH_RETRY_MSEC )
			{
				if ( record.retries >= MAX_PUSH_RETRIES )
				{
					if ( !record.exhaustionReported )
					{
						Log::Warn( "Lua push stream for client %d blocked at command %llu", clientNum,
						           static_cast<unsigned long long>( record.id ) );
						record.exhaustionReported = true;
					}
					break;
				}
				if ( record.sentAt )
				{
					record.retries++;
				}
				SendPushRecord( clientNum, record );
			}
			break;
		}
	}
	for ( auto it = requestTimes.begin(); it != requestTimes.end(); )
	{
		int clientNum = it->first;
		if ( clientNum < 0 || clientNum >= level.maxclients ||
		     level.clients[ clientNum ].pers.connected != CON_CONNECTED )
		{
			lastChunkTime.erase( clientNum );
			it = requestTimes.erase( it );
		}
		else
		{
			++it;
		}
	}
	for ( auto it = outstandingRequests.begin(); it != outstandingRequests.end(); )
	{
		int clientNum = it->first.first;
		if ( level.time - it->second >= STREAM_TIMEOUT_MSEC || clientNum < 0 ||
		     clientNum >= level.maxclients ||
		     level.clients[ clientNum ].pers.connected != CON_CONNECTED )
		{
			streams.erase( it->first );
			it = outstandingRequests.erase( it );
		}
		else
		{
			++it;
		}
	}
	for ( auto it = streams.begin(); it != streams.end(); )
	{
		int clientNum = it->first.first;
		if ( outstandingRequests.find( it->first ) == outstandingRequests.end() || clientNum < 0 ||
		     clientNum >= level.maxclients ||
		     level.clients[ clientNum ].pers.connected != CON_CONNECTED )
		{
			it = streams.erase( it );
			continue;
		}
		if ( it->second.pendingAck &&
		     level.time - lastChunkTime[ clientNum ] >= CHUNK_INTERVAL_MSEC )
		{
			if ( !SendNextChunk( it->first, it->second ) )
			{
				it = streams.erase( it );
				continue;
			}
		}
		++it;
	}
}

void ClearDataEndpoints()
{
	streams.clear();
	outstandingRequests.clear();
	requestTimes.clear();
	lastChunkTime.clear();
	if ( State() )
	{
		for ( const auto &endpoint : endpoints )
		{
			luaL_unref( State(), LUA_REGISTRYINDEX, endpoint.second );
		}
	}
	endpoints.clear();
}

void ClearPushCommands()
{
	for ( int clientNum = 0; clientNum < level.maxclients; clientNum++ )
	{
		if ( level.clients[ clientNum ].pers.connected == CON_CONNECTED &&
		     !level.clients[ clientNum ].pers.isBot )
		{
			trap_SendServerCommand( clientNum, "lua_push_reset" );
		}
	}
	outstandingPushes.clear();
	pushSequence.clear();
	pushAck.clear();
	pushReady.clear();
	persistentRecipients.clear();
	nextPushId = 1;
}

void ResetPushClient( int clientNum )
{
	if ( clientNum < 0 || clientNum >= level.maxclients )
	{
		return;
	}
	auto existing = outstandingPushes.find( clientNum );
	if ( existing != outstandingPushes.end() )
	{
		for ( PushRecord &record : existing->second )
		{
			record.sentAt = 0;
			record.retries = 0;
			record.exhaustionReported = false;
		}
	}
	pushAck.erase( clientNum );
	pushSequence.erase( clientNum );
	pushReady.erase( clientNum );
	trap_SendServerCommand( clientNum, "lua_push_reset" );
}

void ClearPushClient( int clientNum )
{
	outstandingPushes.erase( clientNum );
	pushAck.erase( clientNum );
	pushSequence.erase( clientNum );
	pushReady.erase( clientNum );
}

void DisconnectPushClient( int clientNum )
{
	if ( clientNum < 0 || clientNum >= level.maxclients )
	{
		return;
	}
	const std::string guid = level.clients[ clientNum ].pers.guid;
	if ( !guid.empty() )
	{
		persistentRecipients.erase( guid );
	}
	ClearPushClient( clientNum );
}
}  // namespace Lua
