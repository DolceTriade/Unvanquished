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
#include "sgame/sg_bot_util.h"
#include "shared/lua/LuaLib.h"
#include "shared/lua/Utils.h"
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
};

struct BotBehaviorState
{
	AILuaNode_t *ownerNode = nullptr;
	AIBotActionWrapper actions[ 2 ];
	int activeAction = 0;
};

struct BotContext
{
	gentity_t *self;
	AILuaNode_t *node;
	BotBehaviorState *actionState;
};

namespace
{

using Shared::Lua::LuaLib;
using Shared::Lua::RegType;

static bool botContextRegistered = false;

static const char *StatusName( AINodeStatus_t status )
{
	switch ( status )
	{
		case STATUS_FAILURE: return "FAILURE";
		case STATUS_SUCCESS: return "SUCCESS";
		case STATUS_RUNNING: return "RUNNING";
	}

	return "UNKNOWN";
}

static void TraceLuaAction( BotContext *ctx, lua_State *L, const char *name, AINodeStatus_t status )
{
	if ( !ctx || !ctx->self || G_BotTraceClient() != ctx->self->num() )
	{
		return;
	}

	lua_Debug ar = {};
	const char *source = "[C]";
	int line = 0;

	if ( lua_getstack( L, 1, &ar ) && lua_getinfo( L, "Sl", &ar ) )
	{
		if ( ar.source && ar.source[ 0 ] == '@' )
		{
			source = ar.source + 1;
		}
		else if ( ar.short_src[ 0 ] )
		{
			source = ar.short_src;
		}
		line = ar.currentline;
	}

	Log::defaultLogger.WithoutSuppression().Notice(
		"lua bot %d %s:%d %s -> %s",
		ctx->self->num(), source, line, name, StatusName( status ) );
}

static bool IsLiveEntityProxy( EntityProxy *proxy )
{
	return proxy && proxy->ent && proxy->ent->inuse && proxy->ent->generation == proxy->generation;
}

static EntityProxy *CheckEntityProxyArg( lua_State *L, int index )
{
	auto **proxy = static_cast<EntityProxy **>(
		luaL_checkudata( L, index, Shared::Lua::GetTClassName<EntityProxy>() ) );
	if ( !proxy || !IsLiveEntityProxy( *proxy ) )
	{
		luaL_argerror( L, index, "expected a live EntityProxy" );
	}

	return *proxy;
}

static glm::vec3 CheckPositionArg( lua_State *L, int index )
{
	glm::vec3 result;
	if ( lua_istable( L, index ) )
	{
		for ( int i = 0; i < 3; ++i )
		{
			lua_rawgeti( L, index, i + 1 );
			if ( !lua_isnumber( L, -1 ) )
			{
				lua_pop( L, 1 );
				luaL_argerror( L, index, "expected a vec3 array or three numeric coordinates" );
			}
			result[ i ] = lua_tonumber( L, -1 );
			lua_pop( L, 1 );
		}
		return result;
	}

	if ( lua_isnumber( L, index ) &&
	     lua_isnumber( L, index + 1 ) &&
	     lua_isnumber( L, index + 2 ) )
	{
		for ( int i = 0; i < 3; ++i )
		{
			result[ i ] = lua_tonumber( L, index + i );
		}
		return result;
	}

	luaL_argerror( L, index, "expected a vec3 array or three numeric coordinates" );
	return {};
}

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

static BotBehaviorState& GetBotBehaviorState( botMemory_t& memory, AILuaNode_t *node )
{
	if ( !memory.luaBehaviorState )
	{
		memory.luaBehaviorState = new BotBehaviorState();
	}

	BotBehaviorState& state = *memory.luaBehaviorState;
	if ( state.ownerNode != node )
	{
		for ( AIBotActionWrapper& wrapper : state.actions )
		{
			DestroyActionWrapper( wrapper );
		}

		state = {};
		state.ownerNode = node;
	}

	return state;
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

static bool ParseMoveDir( const char *name, int& out )
{
	if ( !Q_stricmp( name, "forward" ) )
	{
		out = MOVE_FORWARD;
		return true;
	}
	if ( !Q_stricmp( name, "backward" ) || !Q_stricmp( name, "back" ) )
	{
		out = MOVE_BACKWARD;
		return true;
	}
	if ( !Q_stricmp( name, "left" ) )
	{
		out = MOVE_LEFT;
		return true;
	}
	if ( !Q_stricmp( name, "right" ) )
	{
		out = MOVE_RIGHT;
		return true;
	}
	return false;
}

static bool ParseAIEntityName( const char *name, int& out )
{
	if ( !Q_stricmp( name, "goal" ) )
	{
		out = E_GOAL;
		return true;
	}
	if ( !Q_stricmp( name, "enemy" ) )
	{
		out = E_ENEMY;
		return true;
	}
	if ( !Q_stricmp( name, "damaged_building" ) || !Q_stricmp( name, "damagedbuilding" ) )
	{
		out = E_DAMAGEDBUILDING;
		return true;
	}
	if ( !Q_stricmp( name, "friendly_building" ) || !Q_stricmp( name, "friendlybuilding" ) )
	{
		out = E_FRIENDLYBUILDING;
		return true;
	}
	if ( !Q_stricmp( name, "enemy_building" ) || !Q_stricmp( name, "enemybuilding" ) )
	{
		out = E_ENEMYBUILDING;
		return true;
	}
	if ( !Q_stricmp( name, "self" ) )
	{
		out = E_SELF;
		return true;
	}
	if ( !Q_stricmp( name, "userpos" ) || !Q_stricmp( name, "user_pos" ) )
	{
		out = E_USERPOS;
		return true;
	}

	if ( !Q_stricmp( name, "eggpod" ) || !Q_stricmp( name, "spawn" ) || !Q_stricmp( name, "a_spawn" ) )
	{
		out = E_A_SPAWN;
		return true;
	}
	if ( !Q_stricmp( name, "overmind" ) )
	{
		out = E_A_OVERMIND;
		return true;
	}
	if ( !Q_stricmp( name, "booster" ) )
	{
		out = E_A_BOOSTER;
		return true;
	}
	if ( !Q_stricmp( name, "telenode" ) || !Q_stricmp( name, "h_spawn" ) )
	{
		out = E_H_SPAWN;
		return true;
	}
	if ( !Q_stricmp( name, "arm" ) || !Q_stricmp( name, "armoury" ) || !Q_stricmp( name, "armory" ) )
	{
		out = E_H_ARMOURY;
		return true;
	}
	if ( !Q_stricmp( name, "medistat" ) )
	{
		out = E_H_MEDISTAT;
		return true;
	}
	if ( !Q_stricmp( name, "drill" ) )
	{
		out = E_H_DRILL;
		return true;
	}
	if ( !Q_stricmp( name, "reactor" ) )
	{
		out = E_H_REACTOR;
		return true;
	}

	return false;
}

static bool BoxLuaActionValue( lua_State *L, const char *actionName, int index, AIValue_t& out )
{
	if ( lua_type( L, index ) == LUA_TSTRING )
	{
		const char *name = lua_tostring( L, index );

		if ( !Q_stricmp( actionName, "evolveTo" ) )
		{
			const classAttributes_t *clazz = BG_ClassByName( name );
			if ( !clazz || clazz->number == PCL_NONE )
			{
				return false;
			}
			out = AIBoxInt( clazz->number );
			return true;
		}

		if ( !Q_stricmp( actionName, "buyPrimary" ) ||
		     ( !Q_stricmp( actionName, "buy" ) && index == 1 ) )
		{
			const weaponAttributes_t *weapon = BG_WeaponByName( name );
			if ( !weapon || weapon->number == WP_NONE )
			{
				return false;
			}
			out = AIBoxInt( weapon->number );
			return true;
		}

		if ( !Q_stricmp( actionName, "buy" ) && index > 1 )
		{
			const upgradeAttributes_t *upgrade = BG_UpgradeByName( name );
			if ( !upgrade || upgrade->number == UP_NONE )
			{
				return false;
			}
			out = AIBoxInt( upgrade->number );
			return true;
		}

		if ( !Q_stricmp( actionName, "activateUpgrade" ) ||
		     !Q_stricmp( actionName, "deactivateUpgrade" ) )
		{
			const upgradeAttributes_t *upgrade = BG_UpgradeByName( name );
			if ( !upgrade || upgrade->number == UP_NONE )
			{
				return false;
			}
			out = AIBoxInt( upgrade->number );
			return true;
		}

		if ( !Q_stricmp( actionName, "moveInDir" ) )
		{
			int dir = 0;
			if ( !ParseMoveDir( name, dir ) )
			{
				return false;
			}
			out = AIBoxInt( dir );
			return true;
		}

		if ( !Q_stricmp( actionName, "roamInRadius" ) ||
		     !Q_stricmp( actionName, "moveTo" ) ||
		     !Q_stricmp( actionName, "changeGoal" ) ||
		     !Q_stricmp( actionName, "follow" ) )
		{
			int entity = 0;
			if ( !ParseAIEntityName( name, entity ) )
			{
				return false;
			}
			out = AIBoxInt( entity );
			return true;
		}
	}

	return BoxLuaValue( L, index, out );
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
		if ( !BoxLuaActionValue( L, name, i + 1, params[ i ] ) )
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

	AIActionNode_t *actionNode = nullptr;

	for ( int i = 0; i < 2; ++i )
	{
		AIBotActionWrapper& wrapper = ctx->actionState->actions[ i ];
		if ( wrapper.used && ActionMatches( wrapper.action, run, params.data(), argCount ) )
		{
			ctx->actionState->activeAction = i;
			actionNode = &wrapper.action;
			break;
		}
	}

	if ( actionNode )
	{
		for ( AIValue_t value : params )
		{
			AIDestroyValue( value );
		}
	}
	else
	{
		int replaceIndex = 1 - ctx->actionState->activeAction;
		AIGenericNode_t *currentNode = ctx->self->botMind->currentNode;

		for ( int i = 0; i < 2; ++i )
		{
			if ( reinterpret_cast<AIGenericNode_t *>( &ctx->actionState->actions[ i ].action ) != currentNode )
			{
				replaceIndex = i;
				break;
			}
		}

		AIBotActionWrapper& replacement = ctx->actionState->actions[ replaceIndex ];
		DestroyActionWrapper( replacement );

		replacement.used = true;
		replacement.action.type = ACTION_NODE;
		replacement.action.run = run;
		replacement.action.nparams = argCount;
		replacement.action.lineNum = 0;
		replacement.action.name = name;
		if ( argCount > 0 )
		{
			replacement.action.params =
				static_cast<AIValue_t *>( BG_Alloc( sizeof( AIValue_t ) * argCount ) );
			for ( int i = 0; i < argCount; ++i )
			{
				replacement.action.params[ i ] = CloneValue( params[ i ] );
			}
		}
		else
		{
			replacement.action.params = nullptr;
		}

		for ( AIValue_t value : params )
		{
			AIDestroyValue( value );
		}

		ctx->actionState->activeAction = replaceIndex;
		actionNode = &replacement.action;
	}

	AINodeStatus_t status =
		BotEvaluateNode( ctx->self, reinterpret_cast<AIGenericNode_t *>( actionNode ) );
	TraceLuaAction( ctx, L, name, status );
	return status;
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

static int MethodcanEvolveTo( lua_State *L, BotContext *ctx )
{
	if ( lua_gettop( L ) != 1 )
	{
		Log::Warn( "lua query 'canEvolveTo' expected exactly 1 argument" );
		lua_pushboolean( L, false );
		return 1;
	}

	int selection = 0;
	if ( lua_isinteger( L, 1 ) )
	{
		selection = lua_tointeger( L, 1 );
	}
	else if ( lua_isstring( L, 1 ) )
	{
		const char *name = lua_tostring( L, 1 );
		const classAttributes_t *clazz = BG_ClassByName( name );
		if ( !clazz || clazz->number == PCL_NONE )
		{
			lua_pushboolean( L, false );
			return 1;
		}
		selection = clazz->number;
	}
	else
	{
		lua_pushboolean( L, false );
		return 1;
	}

	class_t c = static_cast<class_t>( selection );
	lua_pushboolean( L, BotIsClassAvailable( c ) &&
		G_AlienEvolve( ctx->self, c, false, /* dryRun = */ true ) );
	return 1;
}

static int MethoddistanceToEntity( lua_State *L, BotContext *ctx )
{
	EntityProxy *proxy = CheckEntityProxyArg( L, 1 );
	lua_pushnumber( L, G_Distance( ctx->self, proxy->ent ) );
	return 1;
}

static int MethoddistanceToPosition( lua_State *L, BotContext *ctx )
{
	glm::vec3 position = CheckPositionArg( L, 1 );
	lua_pushnumber( L, Distance( ctx->self->s.origin, GLM4READ( position ) ) );
	return 1;
}

static int MethoddirectPathToEntity( lua_State *L, BotContext *ctx )
{
	EntityProxy *proxy = CheckEntityProxyArg( L, 1 );
	botTarget_t target;
	target = proxy->ent;
	lua_pushboolean( L, BotPathIsWalkable( ctx->self, target ) );
	return 1;
}

static int MethoddirectPathToPosition( lua_State *L, BotContext *ctx )
{
	glm::vec3 position = CheckPositionArg( L, 1 );
	botTarget_t target;
	target = position;
	lua_pushboolean( L, BotPathIsWalkable( ctx->self, target ) );
	return 1;
}

static int MethodisVisibleEntity( lua_State *L, BotContext *ctx )
{
	EntityProxy *proxy = CheckEntityProxyArg( L, 1 );
	botTarget_t target;
	target = proxy->ent;

	bool visible = BotTargetIsVisible( ctx->self, target, MASK_OPAQUE );
	if ( visible && BotEntityIsValidTarget( proxy->ent ) )
	{
		ctx->self->botMind->enemyLastSeen = level.time;
	}

	lua_pushboolean( L, visible );
	return 1;
}

static int MethodinAttackRangeEntity( lua_State *L, BotContext *ctx )
{
	EntityProxy *proxy = CheckEntityProxyArg( L, 1 );
	botTarget_t target;
	target = proxy->ent;
	lua_pushboolean( L, BotTargetInAttackRange( ctx->self, target ) );
	return 1;
}

#define GET_CTX_FUNC( var, func )                            \
	static int GetCtx##var( lua_State *L )                   \
	{                                                        \
		BotContext *ctx = LuaLib<BotContext>::check( L, 1 ); \
		if ( !ctx || !ctx->self )                            \
		{                                                    \
			Log::Warn( "trying to access stale bot context!" ); \
			return 0;                                        \
		}                                                    \
		func;                                                \
		return 1;                                            \
	}

GET_CTX_FUNC( baseRushScore, lua_pushnumber( L, BotGetBaseRushScore( ctx->self ) ) )

#undef GET_CTX_FUNC

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
	{ "canEvolveTo", MethodcanEvolveTo },
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
	{ "distanceToEntity", MethoddistanceToEntity },
	{ "distanceToPosition", MethoddistanceToPosition },
	{ "directPathToEntity", MethoddirectPathToEntity },
	{ "directPathToPosition", MethoddirectPathToPosition },
	{ "isVisibleEntity", MethodisVisibleEntity },
	{ "inAttackRangeEntity", MethodinAttackRangeEntity },
	{ nullptr, nullptr },
};

luaL_Reg BotContextGetters[] = {
	{ "baseRushScore", GetCtxbaseRushScore },
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

	BotContext context = { self, root, &GetBotBehaviorState( *self->botMind, root ) };
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
	luaL_unref( State(), LUA_REGISTRYINDEX, luaNode->ref );
	BG_Free( luaNode );
}

void ResetBotBehaviorState( botMemory_t& memory )
{
	if ( !memory.luaBehaviorState )
	{
		return;
	}

	for ( AIBotActionWrapper& wrapper : memory.luaBehaviorState->actions )
	{
		DestroyActionWrapper( wrapper );
	}

	delete memory.luaBehaviorState;
	memory.luaBehaviorState = nullptr;
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
