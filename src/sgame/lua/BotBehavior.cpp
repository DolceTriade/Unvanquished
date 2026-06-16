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

#include "sgame/lua/Entity.h"
#include "sgame/lua/EntityProxy.h"
#include "sgame/lua/Interpreter.h"
#include "sgame/sg_bot_parse.h"
#include "shared/lua/LuaLib.h"
#include <vector>

namespace Lua
{
struct AIBotActionWrapper
{
	bool used;
	AIActionNode_t action;
};

struct AILuaNode_t
{
	// Must match AIGenericNode_t
	AINode_t type;
	AINodeRunner run;
	// Custom properties.
	int ref;
	AIBotActionWrapper actions[ 2 ];
	int activeAction;
};

struct BotContext
{
	gentity_t *self;
	AILuaNode_t *node;
};

namespace
{

using Shared::Lua::LuaLib;
using Shared::Lua::RegType;

static bool botContextRegistered = false;

static void DestroyActionWrapper( AIBotActionWrapper& wrapper )
{
	if ( !wrapper.used )
	{
		return;
	}

	for ( int i = 0; i < wrapper.action.nparams; ++i )
	{
		AIDestroyValue( wrapper.action.params[ i ] );
	}

	BG_Free( wrapper.action.params );
	wrapper = {};
}

static AIValue_t CloneValue( AIValue_t value )
{
	switch ( value.valType )
	{
		case VALUE_FLOAT:
			return AIBoxFloat( value.l.floatValue );
		case VALUE_INT:
			return AIBoxInt( value.l.intValue );
		case VALUE_STRING:
			return AIBoxString( value.l.stringValue );
	}

	return AIBoxInt( 0 );
}

static bool ValuesEqual( AIValue_t lhs, AIValue_t rhs )
{
	if ( lhs.valType != rhs.valType )
	{
		return false;
	}

	switch ( lhs.valType )
	{
		case VALUE_FLOAT:
			return lhs.l.floatValue == rhs.l.floatValue;
		case VALUE_INT:
			return lhs.l.intValue == rhs.l.intValue;
		case VALUE_STRING:
			return !Q_stricmp( lhs.l.stringValue, rhs.l.stringValue );
	}

	return false;
}

static bool ActionMatches( const AIActionNode_t& action, AINodeRunner run,
                           const AIValue_t *params, int nparams )
{
	if ( action.run != run || action.nparams != nparams )
	{
		return false;
	}

	for ( int i = 0; i < nparams; ++i )
	{
		if ( !ValuesEqual( action.params[ i ], params[ i ] ) )
		{
			return false;
		}
	}

	return true;
}

static bool BoxLuaValue( lua_State *L, int index, AIValue_t& out )
{
	switch ( lua_type( L, index ) )
	{
		case LUA_TBOOLEAN:
			out = AIBoxInt( lua_toboolean( L, index ) ? 1 : 0 );
			return true;
		case LUA_TNUMBER:
			if ( lua_isinteger( L, index ) )
			{
				out = AIBoxInt( lua_tointeger( L, index ) );
			}
			else
			{
				out = AIBoxFloat( lua_tonumber( L, index ) );
			}
			return true;
		case LUA_TSTRING:
			out = AIBoxString( const_cast<char *>( lua_tostring( L, index ) ) );
			return true;
		default:
			return false;
	}
}

static AINodeStatus_t ExecuteAction( BotContext *ctx, const char *name, AINodeRunner run,
                                     int minparams, int maxparams, lua_State *L )
{
	const int argCount = lua_gettop( L );
	if ( argCount < minparams || argCount > maxparams )
	{
		Log::Warn( "lua action '%s' expected %d to %d arguments but got %d",
		           name, minparams, maxparams, argCount );
		return STATUS_FAILURE;
	}

	std::vector<AIValue_t> params( argCount );
	for ( int i = 0; i < argCount; ++i )
	{
		if ( !BoxLuaValue( L, i + 1, params[ i ] ) )
		{
			Log::Warn( "lua action '%s' received unsupported argument type at index %d",
			           name, i + 1 );
			for ( int j = 0; j < i; ++j )
			{
				AIDestroyValue( params[ j ] );
			}
			return STATUS_FAILURE;
		}
	}

	AIBotActionWrapper& active = ctx->node->actions[ ctx->node->activeAction ];
	AIActionNode_t *actionNode = nullptr;

	if ( active.used && ActionMatches( active.action, run, params.data(), argCount ) )
	{
		actionNode = &active.action;
		for ( AIValue_t value : params )
		{
			AIDestroyValue( value );
		}
	}
	else
	{
		const int inactiveIndex = 1 - ctx->node->activeAction;
		AIBotActionWrapper& inactive = ctx->node->actions[ inactiveIndex ];
		DestroyActionWrapper( inactive );

		inactive.used = true;
		inactive.action.type = ACTION_NODE;
		inactive.action.run = run;
		inactive.action.nparams = argCount;
		inactive.action.lineNum = 0;
		inactive.action.name = name;
		if ( argCount > 0 )
		{
			inactive.action.params =
				static_cast<AIValue_t *>( BG_Alloc( sizeof( AIValue_t ) * argCount ) );
			for ( int i = 0; i < argCount; ++i )
			{
				inactive.action.params[ i ] = CloneValue( params[ i ] );
			}
		}
		else
		{
			inactive.action.params = nullptr;
		}

		for ( AIValue_t value : params )
		{
			AIDestroyValue( value );
		}

		ctx->node->activeAction = inactiveIndex;
		actionNode = &inactive.action;
	}

	return BotEvaluateNode( ctx->self, reinterpret_cast<AIGenericNode_t *>( actionNode ) );
}

static AINodeStatus_t ExecuteSpawnAs( BotContext *ctx, lua_State *L )
{
	if ( lua_gettop( L ) != 1 )
	{
		Log::Warn( "lua action 'spawnAs' expected exactly 1 argument" );
		return STATUS_FAILURE;
	}

	int selection = 0;
	if ( lua_isinteger( L, 1 ) )
	{
		selection = lua_tointeger( L, 1 );
	}
	else if ( lua_isstring( L, 1 ) )
	{
		const char *name = lua_tostring( L, 1 );
		switch ( G_Team( ctx->self ) )
		{
			case TEAM_ALIENS:
			{
				const classAttributes_t *clazz = BG_ClassByName( name );
				if ( !clazz || clazz->number == PCL_NONE )
				{
					Log::Warn( "lua action 'spawnAs' received invalid class '%s'", name );
					return STATUS_FAILURE;
				}
				selection = clazz->number;
				break;
			}
			case TEAM_HUMANS:
			{
				const weaponAttributes_t *weapon = BG_WeaponByName( name );
				if ( !weapon || weapon->number == WP_NONE )
				{
					Log::Warn( "lua action 'spawnAs' received invalid weapon '%s'", name );
					return STATUS_FAILURE;
				}
				selection = weapon->number;
				break;
			}
			case TEAM_NONE:
				return STATUS_FAILURE;
		}
	}
	else
	{
		Log::Warn( "lua action 'spawnAs' requires a string or integer argument" );
		return STATUS_FAILURE;
	}

	AISpawnNode_t spawn = {};
	spawn.type = SPAWN_NODE;
	spawn.run = BotSpawnNode;
	spawn.selection = selection;
	return BotSpawnNode( ctx->self, reinterpret_cast<AIGenericNode_t *>( &spawn ) );
}

#define DEFINE_BOT_ACTION_METHOD(name, run, minparams, maxparams) \
	static int Method##name( lua_State *L, BotContext *ctx ) \
	{ \
		lua_pushinteger( L, ExecuteAction( ctx, #name, run, minparams, maxparams, L ) ); \
		return 1; \
	}

DEFINE_BOT_ACTION_METHOD( activateUpgrade, BotActionActivateUpgrade, 1, 1 )
DEFINE_BOT_ACTION_METHOD( aimAtGoal, BotActionAimAtGoal, 0, 0 )
DEFINE_BOT_ACTION_METHOD( alternateStrafe, BotActionAlternateStrafe, 0, 0 )
DEFINE_BOT_ACTION_METHOD( blackboardNoteTransient, BotActionBlackboardNoteTransient, 1, 1 )
DEFINE_BOT_ACTION_METHOD( buildNowChosenBuildable, BotActionBuildNowChosenBuildable, 0, 0 )
DEFINE_BOT_ACTION_METHOD( buy, BotActionBuy, 1, 4 )
DEFINE_BOT_ACTION_METHOD( buyPrimary, BotActionBuyPrimary, 1, 1 )
DEFINE_BOT_ACTION_METHOD( changeBehavior, BotActionChangeBehavior, 1, 1 )
DEFINE_BOT_ACTION_METHOD( changeGoal, BotActionChangeGoal, 1, 3 )
DEFINE_BOT_ACTION_METHOD( classDodge, BotActionClassDodge, 0, 0 )
DEFINE_BOT_ACTION_METHOD( deactivateUpgrade, BotActionDeactivateUpgrade, 1, 1 )
DEFINE_BOT_ACTION_METHOD( equip, BotActionBuy, 0, 0 )
DEFINE_BOT_ACTION_METHOD( evolve, BotActionEvolve, 0, 0 )
DEFINE_BOT_ACTION_METHOD( evolveTo, BotActionEvolveTo, 1, 1 )
DEFINE_BOT_ACTION_METHOD( extinguishFire, BotActionExtinguishFire, 0, 0 )
DEFINE_BOT_ACTION_METHOD( fight, BotActionFight, 0, 0 )
DEFINE_BOT_ACTION_METHOD( fireWeapon, BotActionFireWeapon, 0, 0 )
DEFINE_BOT_ACTION_METHOD( flee, BotActionFlee, 0, 0 )
DEFINE_BOT_ACTION_METHOD( follow, BotActionFollow, 1, 1 )
DEFINE_BOT_ACTION_METHOD( gesture, BotActionGesture, 0, 0 )
DEFINE_BOT_ACTION_METHOD( heal, BotActionHeal, 0, 0 )
DEFINE_BOT_ACTION_METHOD( jump, BotActionJump, 0, 0 )
DEFINE_BOT_ACTION_METHOD( moveInDir, BotActionMoveInDir, 1, 2 )
DEFINE_BOT_ACTION_METHOD( moveTo, BotActionMoveTo, 1, 2 )
DEFINE_BOT_ACTION_METHOD( moveToGoal, BotActionMoveToGoal, 0, 0 )
DEFINE_BOT_ACTION_METHOD( reload, BotActionReload, 0, 0 )
DEFINE_BOT_ACTION_METHOD( repair, BotActionRepair, 0, 0 )
DEFINE_BOT_ACTION_METHOD( resetMyTimer, BotActionResetMyTimer, 0, 0 )
DEFINE_BOT_ACTION_METHOD( resetStuckTime, BotActionResetStuckTime, 0, 0 )
DEFINE_BOT_ACTION_METHOD( roam, BotActionRoam, 0, 0 )
DEFINE_BOT_ACTION_METHOD( roamInRadius, BotActionRoamInRadius, 2, 2 )
DEFINE_BOT_ACTION_METHOD( rush, BotActionRush, 0, 0 )
DEFINE_BOT_ACTION_METHOD( say, BotActionSay, 1, 2 )
static int MethodspawnAs( lua_State *L, BotContext *ctx )
{
	lua_pushinteger( L, ExecuteSpawnAs( ctx, L ) );
	return 1;
}
DEFINE_BOT_ACTION_METHOD( stayHere, BotActionStayHere, 1, 1 )
DEFINE_BOT_ACTION_METHOD( strafeDodge, BotActionStrafeDodge, 0, 0 )
DEFINE_BOT_ACTION_METHOD( suicide, BotActionSuicide, 0, 0 )
DEFINE_BOT_ACTION_METHOD( teleport, BotActionTeleport, 3, 3 )

#undef DEFINE_BOT_ACTION_METHOD

RegType<BotContext> BotContextMethods[] =
{
	{ "activateUpgrade", MethodactivateUpgrade },
	{ "aimAtGoal", MethodaimAtGoal },
	{ "alternateStrafe", MethodalternateStrafe },
	{ "blackboardNoteTransient", MethodblackboardNoteTransient },
	{ "buildNowChosenBuildable", MethodbuildNowChosenBuildable },
	{ "buy", Methodbuy },
	{ "buyPrimary", MethodbuyPrimary },
	{ "changeBehavior", MethodchangeBehavior },
	{ "changeGoal", MethodchangeGoal },
	{ "classDodge", MethodclassDodge },
	{ "deactivateUpgrade", MethoddeactivateUpgrade },
	{ "equip", Methodequip },
	{ "evolve", Methodevolve },
	{ "evolveTo", MethodevolveTo },
	{ "extinguishFire", MethodextinguishFire },
	{ "fight", Methodfight },
	{ "fireWeapon", MethodfireWeapon },
	{ "flee", Methodflee },
	{ "follow", Methodfollow },
	{ "gesture", Methodgesture },
	{ "heal", Methodheal },
	{ "jump", Methodjump },
	{ "moveInDir", MethodmoveInDir },
	{ "moveTo", MethodmoveTo },
	{ "moveToGoal", MethodmoveToGoal },
	{ "reload", Methodreload },
	{ "repair", Methodrepair },
	{ "resetMyTimer", MethodresetMyTimer },
	{ "resetStuckTime", MethodresetStuckTime },
	{ "roam", Methodroam },
	{ "roamInRadius", MethodroamInRadius },
	{ "rush", Methodrush },
	{ "say", Methodsay },
	{ "spawnAs", MethodspawnAs },
	{ "stayHere", MethodstayHere },
	{ "strafeDodge", MethodstrafeDodge },
	{ "suicide", Methodsuicide },
	{ "teleport", Methodteleport },
	{ nullptr, nullptr },
};

luaL_Reg BotContextGetters[] = {
	{ nullptr, nullptr },
};

luaL_Reg BotContextSetters[] = {
	{ nullptr, nullptr },
};

static void EnsureBotContextRegistered( lua_State *L )
{
	if ( botContextRegistered )
	{
		return;
	}

	LuaLib<BotContext>::Register( L );
	botContextRegistered = true;
}

}  // namespace

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
	EnsureBotContextRegistered( L );

	BotContext context = { self, root };
	lua_rawgeti( L, LUA_REGISTRYINDEX, root->ref );
	LuaLib<EntityProxy>::push( L, Entity::CreateProxy( self, L ) );
	LuaLib<BotContext>::push( L, &context );
	if ( lua_pcall( L, 2, 1, 0 ) != LUA_OK )
	{
		Log::Warn( "Error running lua behavior '%s': %s", bt->name, lua_tostring( L, -1 ) );
		lua_pop( L, 1 );
		return STATUS_FAILURE;
	}

	if ( !lua_isnumber( L, -1 ) )
	{
		Log::Warn( "Lua behavior '%s' must return a numeric status", bt->name );
		lua_pop( L, 1 );
		return STATUS_FAILURE;
	}
	AINodeStatus_t status = static_cast<AINodeStatus_t>( lua_tointeger( L, -1 ) );
	lua_pop( L, 1 );

	return status;
}

AIGenericNode_t *luaNode( int funcRef )
{
	// Freed by FreeBehaviorTree
	AILuaNode_t *node = static_cast<AILuaNode_t *>( BG_Alloc( sizeof( AILuaNode_t ) ) );
	node->type = LUA_ACTION_NODE;
	node->run = nullptr;
	node->ref = funcRef;
	node->actions[ 0 ] = {};
	node->actions[ 1 ] = {};
	node->activeAction = 0;
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
	bt->classSelectionTree = reinterpret_cast<AIGenericNode_t*>( bt );
	bt->run = &runLuaBehavior;

	return bt;
}

void FreeLuaBehaviorTree( AIBehaviorTree_t *tree )
{
	if ( !tree )
	{
		return;
	}

	if ( tree->classSelectionTree )
	{
		tree->classSelectionTree = nullptr;
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
	DestroyActionWrapper( luaNode->actions[ 0 ] );
	DestroyActionWrapper( luaNode->actions[ 1 ] );
	luaL_unref( State(), LUA_REGISTRYINDEX, luaNode->ref );
	BG_Free( luaNode );
}

}  // namespace Lua

namespace Shared {
namespace Lua {

LUACORETYPEDEFINE( ::Lua::BotContext )

template <>
void ExtraInit<::Lua::BotContext>( lua_State* /*L*/, int /*metatable_index*/ )
{}

}  // namespace Lua
}  // namespace Shared
