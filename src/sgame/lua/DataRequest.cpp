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
#include <cstdlib>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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
// Endpoint name -> Lua registry reference for the endpoint function.
std::unordered_map<std::string, int> endpoints;

constexpr size_t MAX_COMMAND_SIZE = 800;
constexpr unsigned MAX_RESPONSE_CHUNKS = 10;
// Bound both the serialized response and its retained transfer buffer.
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

bool AddEstimatedSize( size_t &size, size_t amount )
{
	if ( amount > MAX_RESPONSE_BYTES - size ) return false;
	size += amount;
	return true;
}

bool EstimateJsonStringSize( const char *text, size_t length, size_t &size )
{
	if ( !AddEstimatedSize( size, 2 ) ) return false; // Quotes.
	for ( size_t i = 0; i < length; i++ )
	{
		unsigned char c = static_cast<unsigned char>( text[ i ] );
		size_t escaped = c < 0x20 ? 6 : ( c == '"' || c == '\\' || c == '/' ? 2 : 1 );
		if ( !AddEstimatedSize( size, escaped ) ) return false;
	}
	return true;
}

bool EstimateJsonValueSize( lua_State *L, int index, size_t &size, unsigned depth,
                            std::unordered_set<const void *> &ancestors )
{
	if ( depth > 100 ) return false;
	index = lua_absindex( L, index );
	switch ( lua_type( L, index ) )
	{
		case LUA_TNIL:
			return AddEstimatedSize( size, 4 );
		case LUA_TBOOLEAN:
			return AddEstimatedSize( size, lua_toboolean( L, index ) ? 4 : 5 );
		case LUA_TNUMBER:
			return AddEstimatedSize( size, 32 ); // Conservative bound for cjson's number format.
		case LUA_TSTRING:
		{
			size_t length = 0;
			const char *text = lua_tolstring( L, index, &length );
			return EstimateJsonStringSize( text, length, size );
		}
		case LUA_TTABLE:
		{
			const void *identity = lua_topointer( L, index );
			if ( ancestors.count( identity ) ) return false;
			ancestors.insert( identity );
			size_t before = size;
			bool ok = AddEstimatedSize( size, 2 ); // Braces; arrays are conservatively sized as objects.
			bool first = true;
			lua_pushnil( L );
			while ( ok && lua_next( L, index ) )
			{
				if ( !first ) ok = AddEstimatedSize( size, 1 );
				first = false;
				if ( ok )
				{
					if ( lua_type( L, -2 ) == LUA_TSTRING )
					{
						size_t length = 0;
						const char *text = lua_tolstring( L, -2, &length );
						ok = EstimateJsonStringSize( text, length, size );
					}
					else if ( lua_type( L, -2 ) == LUA_TNUMBER )
					{
						ok = AddEstimatedSize( size, 40 ); // Quoted numeric object key.
					}
					else
					{
						ok = false;
					}
				}
				if ( ok ) ok = AddEstimatedSize( size, 1 ); // Colon.
				if ( ok ) ok = EstimateJsonValueSize( L, -1, size, depth + 1, ancestors );
				lua_pop( L, 1 );
			}
			if ( !ok )
			{
				// Balance lua_next's key when the estimate hit its limit mid-iteration.
				if ( lua_gettop( L ) > index ) lua_pop( L, 1 );
				size = before;
			}
			ancestors.erase( identity );
			return ok;
		}
		default:
			return false;
	}
}

bool FitsResponseLimit( lua_State *L, int index )
{
	size_t estimate = 0;
	std::unordered_set<const void *> ancestors;
	return EstimateJsonValueSize( L, index, estimate, 0, ancestors );
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
	if ( command.size() < 1022 )
	{
		trap_SendServerCommand( clientNum, command.c_str() );
	}
}

void SendTransportError( int clientNum, unsigned long id, const char *error )
{
	lua_State *L = State();
	if ( !L ) return;
	int base = lua_gettop( L );
	std::string encoded, jsonError;
	PushErrorEnvelope( L, error );
	if ( Shared::Lua::EncodeJson( L, -1, encoded, jsonError ) )
	{
		SendReply( clientNum, id, encoded );
	}
	lua_settop( L, base );
}

std::string ChunkCommand( unsigned id, unsigned sequence, const std::string &part,
                          bool done, bool truncated )
{
	return Str::Format( "lua_data_chunk %u %u %s %d %d", id, sequence,
	                    Cmd::Escape( part ).c_str(), done ? 1 : 0, truncated ? 1 : 0 );
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
		std::string command = ChunkCommand( key.second, stream.nextSequence,
		                                    stream.response.substr( stream.offset, middle ),
		                                    done, truncated );
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
	std::string command = ChunkCommand( key.second, stream.nextSequence,
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
	if ( done ) return false;
	stream.waitingSequence = static_cast<int>( stream.nextSequence );
	stream.nextSequence++;
	stream.pendingAck = false;
	return true;
}

bool AllowRequest( int clientNum )
{
	std::deque<int> &times = requestTimes[ clientNum ];
	while ( !times.empty() && level.time - times.front() >= 60000 ) times.pop_front();
	unsigned recentSecond = 0;
	for ( int time : times )
	{
		if ( level.time - time < 1000 ) recentSecond++;
	}
	if ( recentSecond >= 5 || times.size() >= 20 ) return false;
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
	if ( outstandingRequests.count( key ) ) return true;
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
			if ( FitsResponseLimit( L, -1 ) )
			{
				success = Shared::Lua::EncodeJson( L, -1, encoded, jsonError );
				if ( success && encoded.size() > MAX_RESPONSE_BYTES )
				{
					success = false;
					responseTooLarge = true;
				}
			}
			else
			{
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
		if ( previous != streams.end() ) streams.erase( previous );
		StreamState stream;
		stream.response = encoded;
		auto inserted = streams.emplace( key, std::move( stream ) );
		if ( !SendNextChunk( key, inserted.first->second ) ) streams.erase( inserted.first );
	}
	return true;
}

bool HandleDataRequestAck( int clientNum )
{
	const Cmd::Args &args = trap_Args();
	if ( args.Argc() != 3 ) return true;
	const std::string &sequenceArg = args.Argv( 2 );
	if ( sequenceArg.empty() ) return true;
	char *idEnd = nullptr;
	char *sequenceEnd = nullptr;
	unsigned long id = std::strtoul( args.Argv( 1 ).c_str(), &idEnd, 10 );
	long sequence = std::strtol( sequenceArg.c_str(), &sequenceEnd, 10 );
	if ( !idEnd || *idEnd || !sequenceEnd || *sequenceEnd || sequence < -1 ) return true;
	StreamKey key( clientNum, static_cast<unsigned>( id ) );
	auto outstanding = outstandingRequests.find( key );
	if ( outstanding == outstandingRequests.end() ) return true;
	if ( sequence == -1 )
	{
		outstandingRequests.erase( outstanding );
		streams.erase( key );
		return true;
	}
	auto it = streams.find( key );
	if ( it == streams.end() || it->second.waitingSequence != sequence ) return true;
	outstanding->second = level.time;
	int last = lastChunkTime[ clientNum ];
	if ( level.time - last < CHUNK_INTERVAL_MSEC )
	{
		it->second.pendingAck = true;
		return true;
	}
	if ( !SendNextChunk( it->first, it->second ) ) streams.erase( it );
	return true;
}

void UpdateDataRequestStreams()
{
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
		if ( level.time - it->second >= STREAM_TIMEOUT_MSEC ||
		     clientNum < 0 || clientNum >= level.maxclients ||
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
		if ( outstandingRequests.find( it->first ) == outstandingRequests.end() ||
		     clientNum < 0 || clientNum >= level.maxclients ||
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
}  // namespace Lua
