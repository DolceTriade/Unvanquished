/*
===========================================================================

Unvanquished GPL Source Code
Copyright (C) 2026 Unvanquished Developers

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
#include "sgame/lua/Workflow.h"

#include "sgame/lua/Entity.h"
#include "sgame/lua/Interpreter.h"
#include "shared/lua/LuaLib.h"

using Shared::Lua::LuaLib;
using Shared::Lua::RegType;

static Log::Logger logger( "sgame.lua.workflow" );
static Cvar::Range<Cvar::Cvar<int>> msecPerFrame(
	"g_lua_workflow_msecPerFrame",
	"time budget per frame for single-threaded workflow signal pumping.", Cvar::NONE, 20, 1, 1500 );

/// Run, signal, and wait on workflows.
// These are an ergonomic way to write multiframe functions in Lua.
/// @module workflow
namespace Lua
{

struct Signal
{
	std::string signal;
	int payload;
};

struct Signals
{
	Signals() : signals(), length( 0 ) {}

	void Fire( Signal signal );
	bool IsSignaled( Str::StringRef signal ) const;
	Signal PopSignal( Str::StringRef signal );
	Signal PeekSignal( Str::StringRef signal ) const;
	size_t Backlog() const;
	void Clear( lua_State *L );

	std::unordered_map<std::string, std::queue<Signal>> signals;
	size_t length;
};

void Signals::Fire( Signal signal )
{
	auto &q = this->signals[ signal.signal ];
	signal.signal.clear();
	q.emplace( std::move( signal ) );
	this->length++;
}

bool Signals::IsSignaled( Str::StringRef signal ) const
{
	auto it = this->signals.find( signal );
	return it != this->signals.end() && !it->second.empty();
}

Signal Signals::PopSignal( Str::StringRef signal )
{
	auto it = this->signals.find( signal );
	auto sig = std::move( it->second.front() );
	it->second.pop();
	sig.signal = signal;
	this->length--;
	return sig;
}

Signal Signals::PeekSignal( Str::StringRef signal ) const
{
	auto it = this->signals.find( signal );
	auto sig = it->second.front();
	sig.signal = signal;
	return sig;
}

size_t Signals::Backlog() const
{
	return this->length;
}

void Signals::Clear( lua_State *L )
{
	for ( auto &q : this->signals )
	{
		while ( !q.second.empty() )
		{
			auto sig = q.second.front();
			q.second.pop();
			if ( sig.payload == LUA_REFNIL ) continue;
			luaL_unref( L, LUA_REGISTRYINDEX, sig.payload );
		}
	}
	this->signals.clear();
}

Signals signals;

enum WaitFor
{
	NONE = BIT( 1 ),
	TIME = BIT( 2 ),
	SIGNAL = BIT( 3 ),
};

struct WaitCondition
{
	WaitCondition() : waitFor( WaitFor::NONE ) {}

	~WaitCondition() {}

	bool IsReady( int mask = 0 ) const;
	void Clear();

	WaitFor waitFor;

	int time;
	std::string signal;
};

bool WaitCondition::IsReady( int mask ) const
{
	if ( mask && !( mask & this->waitFor ) ) return false;
	switch ( this->waitFor )
	{
		case WaitFor::NONE:
			return true;
		case WaitFor::TIME:
			return this->time <= level.time;
		case WaitFor::SIGNAL:
			return signals.IsSignaled( this->signal );
	}
	Sys::Drop( "unknown wait condition: %d", this->waitFor );
	return false;
}

void WaitCondition::Clear()
{
	switch ( this->waitFor )
	{
		case WaitFor::TIME:
			this->time = 0;
			break;
		case WaitFor::SIGNAL:
			this->signal.clear();
			break;
		default:
			break;
	}
	this->waitFor = WaitFor::NONE;
}

static int idCounter = 0;

struct workflowInstance
{
	workflowInstance( int id, int thread_ref ) : id( id ), thread_ref( thread_ref ) {}

	int id;
	// ref in the registry to the coroutine.
	int thread_ref;

	WaitCondition wait;
};

std::vector<workflowInstance> workflows;

void ParseYield( lua_State *L, workflowInstance *wf, int nres )
{
	if ( nres == 0 )
	{
		wf->wait.waitFor = WaitFor::NONE;
		return;
	}
	if ( nres != 1 || !lua_istable( L, -1 ) )
	{
		logger.Warn( "Unknown yield argument... not waiting" );
		wf->wait.waitFor = WaitFor::NONE;
		lua_pop( L, nres );
		return;
	}

	lua_getfield( L, -1, "kind" );
	const char *kind = lua_tostring( L, -1 );
	lua_pop( L, 1 );
	if ( !kind || !*kind )
	{
		logger.Warn( "Empty signal name... not waiting" );
		wf->wait.waitFor = WaitFor::NONE;
		lua_pop( L, nres );
		return;
	}

	if ( strcmp( kind, "wait_time" ) == 0 )
	{
		lua_getfield( L, -1, "delay" );
		int delayMs = luaL_checkinteger( L, -1 );
		lua_pop( L, 1 );

		wf->wait.waitFor = WaitFor::TIME;
		wf->wait.time = level.time + delayMs;
	}
	else if ( strcmp( kind, "wait_signal" ) == 0 )
	{
		lua_getfield( L, -1, "signal" );
		const char *name = lua_tostring( L, -1 );
		lua_pop( L, 1 );
		if ( !name || !*name )
		{
			logger.Warn( "Empty signal name... not waiting" );
			wf->wait.waitFor = WaitFor::NONE;
			lua_pop( L, nres );
			return;
		}
		wf->wait.waitFor = WaitFor::SIGNAL;
		wf->wait.signal = name;
	}
	else
	{
		logger.Warn( "Unknown kind '%s'... not waiting", kind );
		wf->wait.waitFor = WaitFor::NONE;
	}

	lua_pop( L, nres );
}

void cleanupThread( lua_State *L, lua_State *co, int thread_ref )
{
	int res = lua_closethread( co, L );
	if ( res != LUA_OK )
	{
		logger.Warn( "error closing lua thread: %d", res );
	}
	luaL_unref( L, LUA_REGISTRYINDEX, thread_ref );
}

size_t populateSignalArgs( lua_State *L, const workflowInstance &wf )
{
	if ( wf.wait.waitFor != WaitFor::SIGNAL ) return 0;
	auto sig = signals.PeekSignal( wf.wait.signal );
	if ( sig.payload == LUA_REFNIL ) return 0;

	lua_rawgeti( L, LUA_REGISTRYINDEX, sig.payload );
	int table_idx = lua_gettop( L );

	size_t nargs = lua_rawlen( L, table_idx );

	for ( size_t i = 1; i <= nargs; i++ )
	{
		lua_rawgeti( L, table_idx, i );
	}

	lua_remove( L, table_idx );

	return nargs;
}

void Workflow::Frame( lua_State *L )
{
	std::list<int> cleanup;
	int stopTime = Sys::Milliseconds() + msecPerFrame.Get();
	bool ranThread = true;
	bool first = true;
	// Only run workflows for an allocated frame budget.
	// Abort if:
	// - We exceed our frame budget.
	// - There is no more signal backlog
	// - All coroutines are blocked and we didn't run any coroutines during a particular iteration.
	while ( first || ( stopTime > Sys::Milliseconds() && signals.Backlog() > 0 && ranThread ) )
	{
		ranThread = false;
		std::unordered_set<std::string> processedSignals;
		for ( auto &wf : workflows )
		{
			// On the first iteration, run all the coroutines. Subsequently, only run
			// coroutines blocked on signals to ensure we burn down any accumulated backlog.
			// Also, ensure we don't run any "completed" coroutines.
			if ( !wf.wait.IsReady( first ? 0 : WaitFor::SIGNAL ) || wf.thread_ref == LUA_REFNIL )
			{
				continue;
			}
			ranThread = true;
			lua_rawgeti( L, LUA_REGISTRYINDEX, wf.thread_ref );

			lua_State *co = lua_tothread( L, -1 );

			int nres = 0;
			int nargs = populateSignalArgs( co, wf );
			if ( wf.wait.waitFor == WaitFor::SIGNAL )
			{
				processedSignals.insert( wf.wait.signal );
			}
			wf.wait.Clear();
			int res = lua_resume( co, L, nargs, &nres );
			switch ( res )
			{
				case LUA_YIELD:
					// will pop nres.
					ParseYield( co, &wf, nres );
					break;
				case LUA_OK:
					// Cleanup
					lua_pop( co, nres );
					cleanupThread( L, co, wf.thread_ref );
					wf.thread_ref = LUA_REFNIL;
					cleanup.push_back( wf.id );
					break;
				default:
					// Error
					const char *msg = lua_tostring( co, -1 );
					logger.Warn( "Lua Workflow Error (%d): %s\n", res, msg );
					lua_pop( co, 1 );
					cleanupThread( L, co, wf.thread_ref );
					wf.thread_ref = LUA_REFNIL;
					cleanup.push_back( wf.id );
					break;
			}
			lua_pop( L, 1 );
		}
		first = false;
		// Cleanup processed signals.
		for ( const auto &signal : processedSignals )
		{
			auto sig = signals.PopSignal( signal );
			if ( sig.payload == LUA_REFNIL ) continue;
			luaL_unref( L, LUA_REGISTRYINDEX, sig.payload );
		}
	}

	if ( cleanup.empty() ) return;

	for ( auto it = workflows.begin(); it != workflows.end() && !cleanup.empty(); )
	{
		if ( it->id != cleanup.front() )
		{
			it++;
			continue;
		}
		it = workflows.erase( it );
		cleanup.pop_front();
	}
	if ( !cleanup.empty() )
	{
		Sys::Drop( "Could not clean up all the things" );
	}
}

void ClearWorkflows( lua_State *L )
{
	for ( auto &wf : workflows )
	{
		lua_rawgeti( L, LUA_REGISTRYINDEX, wf.thread_ref );

		lua_State *co = lua_tothread( L, -1 );

		cleanupThread( L, co, wf.thread_ref );
		lua_pop( L, 1 );
	}

	workflows.clear();

	signals.Clear( L );
}

/// Run a workflow.
// The workflow function is scheduled to start on the next server frame.
// Use the workflow helper functions such as `wait_ms` and `wait_signal` instead of calling
// `coroutine.yield` directly.
// @function run
// @tparam function callback Workflow function to execute.
// @usage sgame.workflow.run(function() sgame.workflow.wait_ms(10); print('hi') end) -- Print 'hi'
//        after waiting at least 10 milliseconds.
// @see workflow
int WorkflowRun( lua_State *L )
{
	luaL_checktype( L, 1, LUA_TFUNCTION );

	lua_State *co = lua_newthread( L );

	// Pin the thread:
	// luaL_ref takes the object at the top of the stack (the thread),
	// stores it in the Registry, and returns a unique integer key.
	int thread_ref = luaL_ref( L, LUA_REGISTRYINDEX );

	// Move function to co
	lua_pushvalue( L, 1 );
	lua_xmove( L, co, 1 );

	workflows.emplace_back( idCounter++, thread_ref );

	return 0;
}

/// Send a signal to workflows waiting for an event.
// Additional arguments are queued as payload and returned by `wait_signal`.
// @function signal
// @tparam string signal The name of the signal to send.
// @usage sgame.workflow.signal("door_opened", ent.number) -- Wake workflows waiting on
// "door_opened".
// @see wait_signal
int WorkflowSignal( lua_State *L )
{
	int nargs = lua_gettop( L );
	Signal signal;
	signal.signal = luaL_checkstring( L, 1 );
	signal.payload = LUA_REFNIL;
	if ( nargs > 1 )
	{
		// Create a table with enough capacity for arguments.
		lua_createtable( L, nargs - 1, 0 );

		// Stack currently: [arg1, arg2, ..., argN, table]
		// We want to move arg2...argN into the table (arg1 is the signal name).
		for ( int i = 2; i <= nargs; i++ )
		{
			lua_pushvalue( L, i );
			// Pops the copy, sets it in table
			lua_rawseti( L, -2, i - 1 );
		}

		// Now the table is at the top of the stack.
		// Store the table in the registry and get a reference.
		int registry_ref = luaL_ref( L, LUA_REGISTRYINDEX );

		// Store registry_ref in your C++ Signal object
		signal.payload = registry_ref;
	}
	signals.Fire( std::move( signal ) );
	return 0;
}

/// Wait for at least a given number of milliseconds.
// Must be called from inside a workflow started with `run`.
// @function wait_ms
// @tparam integer delay Number of milliseconds to wait.
// @usage sgame.workflow.wait_ms(5000) -- Wait at least 5 seconds before continuing.
// @see run
int WorkflowWaitMs( lua_State *L )
{
	int delayMs = luaL_checkinteger( L, 1 );

	lua_createtable( L, 0, 2 );
	lua_pushstring( L, "wait_time" );
	lua_setfield( L, -2, "kind" );

	lua_pushinteger( L, delayMs );
	lua_setfield( L, -2, "delay" );

	return lua_yield( L, 1 );
}

/// Wait for the next queued signal with the given name.
// Must be called from inside a workflow started with `run`.
// Returns the signal payload values, if any.
// @function wait_signal
// @tparam string signal The name of the signal to wait for.
// @treturn ... Signal payload values passed to `signal`.
// @usage local entNum = sgame.workflow.wait_signal("door_opened") -- Wait for a door_opened signal.
// @see signal
int WorkflowWaitSignal( lua_State *L )
{
	int nargs = lua_gettop( L );
	const char *signal = nullptr;
	signal = luaL_checkstring( L, 1 );

	lua_createtable( L, 0, 2 );
	lua_pushstring( L, "wait_signal" );
	lua_setfield( L, -2, "kind" );

	lua_pushstring( L, signal );
	lua_setfield( L, -2, "signal" );

	return lua_yield( L, 1 );
}

RegType<Workflow> WorkflowMethods[] = {
	{ nullptr, nullptr },
};

luaL_Reg WorkflowGetters[] = {
	{ nullptr, nullptr },
};

luaL_Reg WorkflowSetters[] = {
	{ nullptr, nullptr },
};

}  // namespace Lua

namespace Shared
{
namespace Lua
{
LUACORETYPEDEFINE( ::Lua::Workflow )

template <>
void ExtraInit<::Lua::Workflow>( lua_State *L, int metatable_index )
{
	::Lua::ClearWorkflows( L );

	lua_pushcfunction( L, ::Lua::WorkflowRun );
	lua_setfield( L, metatable_index - 1, "run" );
	lua_pushcfunction( L, ::Lua::WorkflowSignal );
	lua_setfield( L, metatable_index - 1, "signal" );
	lua_pushcfunction( L, ::Lua::WorkflowWaitMs );
	lua_setfield( L, metatable_index - 1, "wait_ms" );
	lua_pushcfunction( L, ::Lua::WorkflowWaitSignal );
	lua_setfield( L, metatable_index - 1, "wait_signal" );
}
}  // namespace Lua
}  // namespace Shared
