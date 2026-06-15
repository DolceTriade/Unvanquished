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
#include "sgame/lua/BotBehavior.h"

#include "sgame/lua/Interpreter.h"
#include "sgame/sg_bot_parse.h"

namespace Lua
{

struct AILuaNode_t
{
	// Must match AIGenericNode_t
	AINode_t type;
	AINodeRunner run;
	// Custom properties.
	int ref;
};

AINodeStatus_t runLuaBehavior( gentity_t *self, AIGenericNode_t *node )
{
	if ( node->type != LUA_BEHAVIOR_NODE )
	{
		Sys::Drop( "Wrong node type passed to %s, got %d", __func__, node->type );
	}

	AIBehaviorTree_t *bt = reinterpret_cast<AIBehaviorTree_t *>( node );
	if ( bt->root->type != LUA_ACTION_NODE )
	{
		Sys::Drop( "Lua behavior node has wrong child node: %d", bt->root->type );
	}

	lua_State *L = Lua::State();
	AILuaNode_t *root = reinterpret_cast<AILuaNode_t *>( bt->root );
	lua_rawgeti( L, LUA_REGISTRYINDEX, root->ref );
	if ( lua_pcall( L, 0, 1, 0 ) != LUA_OK )
	{
		Log::Warn( "Error running lua behavior '%s': %s", bt->name, lua_tostring( L, -1 ) );
		lua_pop( L, 1 );
		return STATUS_FAILURE;
	}

	AINodeStatus_t status = STATUS_SUCCESS;
	if ( lua_isnumber( L, -1 ) )
	{
		status = static_cast<AINodeStatus_t>( lua_tointeger( L, -1 ) );
	}
	lua_pop( L, 1 );

	return status;
}

AIGenericNode_t *luaNode( int funcRef )
{
	// Freed by FreeBehaviorTree
	AILuaNode_t *node = static_cast<AILuaNode_t *>( BG_Alloc( sizeof( AILuaNode_t ) ) );
	node->type = LUA_ACTION_NODE;
	node->ref = funcRef;
	return reinterpret_cast<AIGenericNode_t *>( node );
}

AIBehaviorTree_t *LoadLuaBehavior( Str::StringRef file )
{
	lua_State *L = Lua::State();

	if ( !Lua::LoadScript( file ) )
	{
		Log::Warn( "Error loading file '%s': %s", file, lua_tostring( L, -1 ) );
		return nullptr;
	}

	if ( lua_pcall( L, 0, 1, 0 ) != LUA_OK )
	{
		Log::Warn( "Error executing file '%s': %s", file, lua_tostring( L, -1 ) );
		lua_pop( L, 1 );
		return nullptr;
	}

	// Ensure that the file actually returned a function.
	if ( !lua_isfunction( L, -1 ) )
	{
		Log::Warn( "Error: Script in '%s' did not return a function", file );
		lua_pop( L, 1 );
		return nullptr;
	}

	int funcRef = luaL_ref( L, LUA_REGISTRYINDEX );

	// Freed by FreeBehaviorTree
	AIBehaviorTree_t *bt =
		static_cast<AIBehaviorTree_t *>( BG_Alloc( sizeof( AIBehaviorTree_t ) ) );
	Q_strncpyz( bt->name, file.c_str(), std::min( sizeof( bt->name ) - 1, file.size() ) );
	bt->type = LUA_BEHAVIOR_NODE;
	bt->root = luaNode( funcRef );
	bt->classSelectionTree = nullptr;
	bt->run = &runLuaBehavior;

	return bt;
}

void FreeLuaBehaviorTree( AIBehaviorTree_t *tree )
{
	if ( !tree )
	{
		return;
	}

	::FreeNode( tree->root );
	::FreeNode( tree->classSelectionTree );
	BG_Free( tree );
}

void FreeLuaActionNode( AIGenericNode_t *node )
{
	if ( !node )
	{
		return;
	}

	AILuaNode_t *luaNode = reinterpret_cast<AILuaNode_t *>( node );
	luaL_unref( State(), LUA_REGISTRYINDEX, luaNode->ref );
	BG_Free( luaNode );
}

}  // namespace Lua
