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
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with the Unvanquished Source Code. If not, see <http://www.gnu.org/licenses/>.

In addition, the Unvanquished Source Code is also subject to certain additional terms.
You should have received a copy of these additional terms immediately following
the terms and conditions of the GNU General Public License which accompanied the
Unvanquished Source Code. If not, please request a copy in writing from id Software at
the address below.

If you have questions concerning this license or the applicable additional terms, you
may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville,
Maryland 20850 USA.

===========================================================================
*/

#include "sgame/lua/World.h"

#include "common/cm/cm_public.h"
#include "sgame/sg_local.h"
#include "sgame/sg_cm_world.h"
#include "sgame/sg_trapcalls.h"
#include "shared/lua/LuaLib.h"
#include "shared/lua/Utils.h"

using Shared::Lua::LuaLib;
using Shared::Lua::RegType;

/// Query map collision geometry.
// @module world

namespace Lua {

/// Return the bounds of the static BSP world.
// @function bounds
// @treturn array mins The lower world bounds.
// @treturn array maxs The upper world bounds.
// @within world
static int Bounds( lua_State* L )
{
	vec3_t mins, maxs;
	CM_ModelBounds( CM_InlineModel( 0 ), mins, maxs );
	Shared::Lua::PushVec3( L, mins );
	Shared::Lua::PushVec3( L, maxs );
	return 2;
}

/// Return contents at a point, including linked entities, mirroring trap_PointContents.
// @function point_contents
// @tparam array point Position to query.
// @tparam integer passEntityNum Entity to ignore.
// @treturn integer contents Contents bit mask.
// @within world
static int PointContents( lua_State* L )
{
	vec3_t point;
	if ( !Shared::Lua::CheckVec3( L, 1, point ) )
	{
		return luaL_error( L, "point_contents expects a position" );
	}
	lua_pushinteger( L, G_CM_PointContents( point, luaL_checkinteger( L, 2 ) ) );
	return 1;
}

static bool CheckOptionalVec3( lua_State* L, int index, vec3_t out )
{
	if ( lua_isnil( L, index ) )
	{
		VectorClear( out );
		return true;
	}

	return Shared::Lua::CheckVec3( L, index, out );
}

/// Trace a box or point through the world and linked entities.
// Inputs mirror trap_Trace. mins and maxs may be nil for a point trace.
// @function trace
// @tparam array start Trace start position.
// @tparam array|nil mins Hull mins relative to start, or nil for a point trace.
// @tparam array|nil maxs Hull maxs relative to start, or nil for a point trace.
// @tparam array end Trace end position.
// @tparam integer passEntityNum Entity to ignore.
// @tparam integer contentMask Contents to collide with.
// @tparam integer skipMask Contents to skip.
// @treturn table trace Trace fields: fraction, endpos, normal, surfaceFlags, contents,
//                       entityNum, startsolid, and allsolid. normal is nil when invalid.
// @within world
static int Trace( lua_State* L )
{
	vec3_t start, mins, maxs, end;
	if ( !Shared::Lua::CheckVec3( L, 1, start ) ||
	     !CheckOptionalVec3( L, 2, mins ) ||
	     !CheckOptionalVec3( L, 3, maxs ) ||
	     !Shared::Lua::CheckVec3( L, 4, end ) ||
	     !lua_isnumber( L, 5 ) || !lua_isnumber( L, 6 ) || !lua_isnumber( L, 7 ) )
	{
		Log::Warn( "invalid arguments to sgame.world.trace." );
		lua_pushnil( L );
		return 1;
	}

	trace_t trace;
	trap_Trace( &trace, start, mins, maxs, end,
	            luaL_checkinteger( L, 5 ), luaL_checkinteger( L, 6 ), luaL_checkinteger( L, 7 ) );

	lua_newtable( L );
	lua_pushnumber( L, trace.fraction );
	lua_setfield( L, -2, "fraction" );
	Shared::Lua::PushVec3( L, trace.endpos );
	lua_setfield( L, -2, "endpos" );
	if ( !trace.startsolid && !trace.allsolid && trace.fraction < 1.0f )
	{
		Shared::Lua::PushVec3( L, trace.plane.normal );
	}
	else
	{
		lua_pushnil( L );
	}
	lua_setfield( L, -2, "normal" );
	lua_pushinteger( L, trace.surfaceFlags );
	lua_setfield( L, -2, "surfaceFlags" );
	lua_pushinteger( L, trace.contents );
	lua_setfield( L, -2, "contents" );
	lua_pushinteger( L, trace.entityNum );
	lua_setfield( L, -2, "entityNum" );
	lua_pushboolean( L, trace.startsolid );
	lua_setfield( L, -2, "startsolid" );
	lua_pushboolean( L, trace.allsolid );
	lua_setfield( L, -2, "allsolid" );
	return 1;
}

RegType<World> WorldMethods[] = {
	{ nullptr, nullptr },
};

luaL_Reg WorldGetters[] = {
	{ nullptr, nullptr },
};

luaL_Reg WorldSetters[] = {
	{ nullptr, nullptr },
};

} // namespace Lua

namespace Shared {
namespace Lua {

LUACORETYPEDEFINE( ::Lua::World )

template <>
void ExtraInit<::Lua::World>( lua_State* L, int metatable_index )
{
	lua_pushcfunction( L, ::Lua::Bounds );
	lua_setfield( L, metatable_index - 1, "bounds" );
	lua_pushcfunction( L, ::Lua::Trace );
	lua_setfield( L, metatable_index - 1, "trace" );
	lua_pushcfunction( L, ::Lua::PointContents );
	lua_setfield( L, metatable_index - 1, "point_contents" );

	lua_pushinteger( L, ENTITYNUM_WORLD );
	lua_setfield( L, metatable_index - 1, "ENTITYNUM_WORLD" );
	lua_pushinteger( L, ENTITYNUM_NONE );
	lua_setfield( L, metatable_index - 1, "ENTITYNUM_NONE" );
	lua_pushinteger( L, MASK_SOLID );
	lua_setfield( L, metatable_index - 1, "MASK_SOLID" );
	lua_pushinteger( L, MASK_PLAYERSOLID );
	lua_setfield( L, metatable_index - 1, "MASK_PLAYERSOLID" );
	lua_pushinteger( L, MASK_DEADSOLID );
	lua_setfield( L, metatable_index - 1, "MASK_DEADSOLID" );
	lua_pushinteger( L, SURF_SKY );
	lua_setfield( L, metatable_index - 1, "SURF_SKY" );
	lua_pushinteger( L, SURF_NODRAW );
	lua_setfield( L, metatable_index - 1, "SURF_NODRAW" );
	lua_pushinteger( L, CONTENTS_SOLID );
	lua_setfield( L, metatable_index - 1, "CONTENTS_SOLID" );
	lua_pushinteger( L, CONTENTS_NODROP | CONTENTS_LAVA | CONTENTS_SLIME );
	lua_setfield( L, metatable_index - 1, "MASK_HAZARD" );
}

} // namespace Lua
} // namespace Shared
