/*
===========================================================================

Unvanquished GPL Source Code
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of the Unvanquished GPL Source Code (Unvanquished Source Code).

Unvanquished is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Unvanquished is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Unvanquished; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

===========================================================================
*/

#include "common/Common.h"
#include "lua/BotBehavior.h"
#include "sg_bot_parse.h"
#include "sg_bot_trace.h"
#include "sg_bot_util.h"
#include "Entities.h"

namespace
{

Cvar::Modified<Cvar::Cvar<bool>> g_bot_transitionTrace(
	"g_bot_transitionTrace", "write transition-only bot execution traces to JSONL while enabled",
	Cvar::TEMPORARY, false);

struct BotTransitionTraceFile
{
	fileHandle_t handle = 0;
};

BotTransitionTraceFile g_botTransitionTraceFile;

const char *TraceBackendName( botTraceBackend_t backend )
{
	switch ( backend )
	{
		case botTraceBackend_t::BT: return "bt";
		case botTraceBackend_t::LUA: return "lua";
		case botTraceBackend_t::NONE: return "none";
	}

	return "none";
}

const char *TraceKindName( botTraceStateKind_t kind )
{
	switch ( kind )
	{
		case botTraceStateKind_t::RUNNING: return "running";
		case botTraceStateKind_t::ROOT_SUCCESS: return "root_success";
		case botTraceStateKind_t::ROOT_FAILURE: return "root_failure";
		case botTraceStateKind_t::NONE: return "none";
	}

	return "none";
}

const char *TraceStatusName( AINodeStatus_t status )
{
	switch ( status )
	{
		case STATUS_RUNNING: return "RUNNING";
		case STATUS_SUCCESS: return "SUCCESS";
		case STATUS_FAILURE: return "FAILURE";
	}

	return "UNKNOWN";
}

bool TraceDescriptorsEqual( const botTraceDescriptor_t& lhs, const botTraceDescriptor_t& rhs )
{
	return lhs.backend == rhs.backend &&
	       lhs.stateKind == rhs.stateKind &&
	       lhs.sourceLine == rhs.sourceLine &&
	       !Q_stricmp( lhs.behavior, rhs.behavior ) &&
	       !Q_stricmp( lhs.actionName, rhs.actionName ) &&
	       !Q_stricmp( lhs.sourceName, rhs.sourceName );
}

void ClearTraceDescriptor( botTraceDescriptor_t& descriptor )
{
	descriptor = {};
}

std::string JsonEscape( Str::StringRef input )
{
	std::string escaped;
	escaped.reserve( input.size() + 8 );

	for ( unsigned char ch : input )
	{
		switch ( ch )
		{
			case '\\': escaped += "\\\\"; break;
			case '"': escaped += "\\\""; break;
			case '\b': escaped += "\\b"; break;
			case '\f': escaped += "\\f"; break;
			case '\n': escaped += "\\n"; break;
			case '\r': escaped += "\\r"; break;
			case '\t': escaped += "\\t"; break;
			default:
				if ( ch < 0x20 )
				{
					escaped += Str::Format( "\\u%04x", ch );
				}
				else
				{
					escaped.push_back( static_cast<char>( ch ) );
				}
				break;
		}
	}

	return escaped;
}

std::string TraceDescriptorJson( const botTraceDescriptor_t& descriptor )
{
	return Str::Format(
		"{\"kind\":\"%s\",\"action\":\"%s\",\"source\":\"%s\",\"line\":%d}",
		TraceKindName( descriptor.stateKind ),
		JsonEscape( descriptor.actionName ).c_str(),
		JsonEscape( descriptor.sourceName ).c_str(),
		descriptor.sourceLine );
}

botTraceDescriptor_t BuildBTTraceDescriptor( gentity_t *self, AINodeStatus_t status )
{
	botTraceDescriptor_t descriptor = {};
	descriptor.backend = botTraceBackend_t::BT;
	Q_strncpyz( descriptor.behavior, self->botMind->behaviorTree->name, sizeof( descriptor.behavior ) );

	switch ( status )
	{
		case STATUS_RUNNING:
		{
			descriptor.stateKind = botTraceStateKind_t::RUNNING;
			if ( self->botMind->runningNodes.empty() )
			{
				break;
			}

			ASSERT_EQ( self->botMind->runningNodes[ 0 ]->type, AINode_t::ACTION_NODE );
			const AIActionNode_t *actionNode =
				reinterpret_cast<const AIActionNode_t *>( self->botMind->runningNodes[ 0 ] );
			const char *tree = self->botMind->behaviorTree->name;
			for ( const AIGenericNode_t *node : self->botMind->runningNodes )
			{
				if ( node->type == AINode_t::BEHAVIOR_NODE )
				{
					tree = reinterpret_cast<const AIBehaviorTree_t *>( node )->name;
					break;
				}
			}

			Q_strncpyz( descriptor.actionName, actionNode->name, sizeof( descriptor.actionName ) );
			Q_strncpyz( descriptor.sourceName, tree, sizeof( descriptor.sourceName ) );
			descriptor.sourceLine = actionNode->lineNum;
			break;
		}

		case STATUS_SUCCESS:
			descriptor.stateKind = botTraceStateKind_t::ROOT_SUCCESS;
			break;

		case STATUS_FAILURE:
			descriptor.stateKind = botTraceStateKind_t::ROOT_FAILURE;
			break;
	}

	return descriptor;
}

botTraceDescriptor_t BuildLuaTraceDescriptor( gentity_t *self, AINodeStatus_t status )
{
	botTraceDescriptor_t descriptor = {};
	descriptor.backend = botTraceBackend_t::LUA;
	Q_strncpyz( descriptor.behavior, self->botMind->behaviorTree->name, sizeof( descriptor.behavior ) );

	switch ( status )
	{
		case STATUS_RUNNING:
		{
			descriptor.stateKind = botTraceStateKind_t::RUNNING;
			const Lua::BotActionTraceInfo *traceInfo = Lua::GetBotActionTraceInfo( *self->botMind );
			Q_strncpyz( descriptor.actionName, traceInfo->actionName ? traceInfo->actionName : "",
			            sizeof( descriptor.actionName ) );
			Q_strncpyz( descriptor.sourceName, traceInfo->sourceName ? traceInfo->sourceName : "",
			            sizeof( descriptor.sourceName ) );
			descriptor.sourceLine = traceInfo->sourceLine;
			break;
		}

		case STATUS_SUCCESS:
			descriptor.stateKind = botTraceStateKind_t::ROOT_SUCCESS;
			break;

		case STATUS_FAILURE:
			descriptor.stateKind = botTraceStateKind_t::ROOT_FAILURE;
			break;
	}

	return descriptor;
}

void CloseTransitionTraceFile()
{
	if ( g_botTransitionTraceFile.handle )
	{
		trap_FS_FCloseFile( g_botTransitionTraceFile.handle );
		g_botTransitionTraceFile.handle = 0;
	}
}

void OpenTransitionTraceFile()
{
	if ( g_botTransitionTraceFile.handle )
	{
		return;
	}

	qtime_t qt;
	std::string mapname = Cvar::GetValue( "mapname" );
	Com_GMTime( &qt );

	std::string logfile = Str::Format(
		"bot-trace/%s-%04i%02i%02i-%02i%02i%02i.jsonl",
		mapname,
		1900 + qt.tm_year, qt.tm_mon + 1, qt.tm_mday,
		qt.tm_hour, qt.tm_min, qt.tm_sec );

	trap_FS_FOpenFile( logfile.c_str(), &g_botTransitionTraceFile.handle, fsMode_t::FS_APPEND );
	if ( !g_botTransitionTraceFile.handle )
	{
		Log::Warn( "Couldn't open bot transition trace logfile: %s", logfile );
		return;
	}

	Log::Notice( "Bot transition tracing enabled: %s", logfile );
}

void SyncTransitionTraceState()
{
	if ( g_bot_transitionTrace.GetModifiedValue() )
	{
		if ( g_bot_transitionTrace.Get() )
		{
			OpenTransitionTraceFile();
		}
		else
		{
			CloseTransitionTraceFile();
		}
	}
	else if ( g_bot_transitionTrace.Get() && !g_botTransitionTraceFile.handle )
	{
		OpenTransitionTraceFile();
	}
}

void WriteTransitionTraceEvent( gentity_t *self, const char *eventName,
                                const botTraceDescriptor_t *previous,
                                const botTraceDescriptor_t& current,
                                AINodeStatus_t status )
{
	if ( !g_botTransitionTraceFile.handle )
	{
		return;
	}

	std::string previousJson = previous ? TraceDescriptorJson( *previous ) : "null";
	std::string line = Str::Format(
		"{\"time\":%d,\"map\":\"%s\",\"botClientNum\":%d,\"botName\":\"%s\",\"team\":\"%s\","
		"\"backend\":\"%s\",\"behavior\":\"%s\",\"event\":\"%s\",\"previous\":%s,"
		"\"current\":%s,\"status\":\"%s\"}\n",
		level.time,
		JsonEscape( Cvar::GetValue( "mapname" ) ).c_str(),
		self->num(),
		JsonEscape( self->client->pers.netname ).c_str(),
		JsonEscape( BG_TeamName( G_Team( self ) ) ).c_str(),
		TraceBackendName( current.backend ),
		JsonEscape( current.behavior ).c_str(),
		eventName,
		previousJson.c_str(),
		TraceDescriptorJson( current ).c_str(),
		TraceStatusName( status ) );

	trap_FS_Write( line.data(), line.size(), g_botTransitionTraceFile.handle );
}

}  // namespace

botTraceDescriptor_t G_BotBuildTraceDescriptor( gentity_t *self, AINodeStatus_t status )
{
	if ( self->botMind->behaviorTree->type == LUA_BEHAVIOR_NODE )
	{
		return BuildLuaTraceDescriptor( self, status );
	}

	return BuildBTTraceDescriptor( self, status );
}

void G_BotResetTraceCache( botMemory_t& memory )
{
	ClearTraceDescriptor( memory.previousTrace );
	ClearTraceDescriptor( memory.lastFailureTrace );
	memory.previousTraceInitialized = false;
	memory.lastFailureTraceInitialized = false;
}

void G_BotTraceTransition( gentity_t *self, AINodeStatus_t status )
{
	SyncTransitionTraceState();
	if ( !g_botTransitionTraceFile.handle || !self->botMind->behaviorTree )
	{
		return;
	}

	botTraceDescriptor_t current = G_BotBuildTraceDescriptor( self, status );
	botMemory_t& memory = *self->botMind;
	bool changed = !memory.previousTraceInitialized ||
	               !TraceDescriptorsEqual( memory.previousTrace, current );

	if ( changed )
	{
		WriteTransitionTraceEvent(
			self, "transition",
			memory.previousTraceInitialized ? &memory.previousTrace : nullptr,
			current, status );
	}

	bool duplicateFailure = memory.lastFailureTraceInitialized &&
	                        TraceDescriptorsEqual( memory.lastFailureTrace, current );
	if ( status == STATUS_FAILURE && !duplicateFailure )
	{
		WriteTransitionTraceEvent( self, "failure", nullptr, current, status );
		memory.lastFailureTrace = current;
		memory.lastFailureTraceInitialized = true;
	}
	else if ( status != STATUS_FAILURE )
	{
		memory.lastFailureTraceInitialized = false;
		ClearTraceDescriptor( memory.lastFailureTrace );
	}

	memory.previousTrace = current;
	memory.previousTraceInitialized = true;
}

void G_BotTransitionTraceShutdown()
{
	CloseTransitionTraceFile();
}
