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

#include "sgame/lua/Overload.h"

#include "sgame/sg_local.h"
#include "shared/lua/LuaLib.h"

using Shared::Lua::LuaLib;
using Shared::Lua::RegType;

/// Access Overload unlock and upgrade state.
/// @module overload
namespace {

team_t ParseTeamArg( lua_State* L, int index )
{
	const char* teamName = luaL_checkstring( L, index );
	std::string team = Str::ToLower( teamName );

	if ( team == "humans" )
	{
		return TEAM_HUMANS;
	}

	if ( team == "aliens" )
	{
		return TEAM_ALIENS;
	}

	Log::Warn( "Lua overload API received invalid team '%s'", teamName );
	return TEAM_NONE;
}

unlockableType_t ParseUnlockTypeArg( lua_State* L, int index )
{
	const char* typeName = luaL_checkstring( L, index );
	std::string type = Str::ToLower( typeName );

	if ( type == "weapon" )
	{
		return UNLT_WEAPON;
	}

	if ( type == "upgrade" )
	{
		return UNLT_UPGRADE;
	}

	if ( type == "buildable" )
	{
		return UNLT_BUILDABLE;
	}

	if ( type == "class" )
	{
		return UNLT_CLASS;
	}

	Log::Warn( "Lua overload API received invalid unlock type '%s'", typeName );
	return UNLT_NUM_UNLOCKABLETYPES;
}

/// Check whether a specific Overload unlock is owned.
// @function is_purchased
// @tparam string team Team name: humans or aliens.
// @tparam string type Unlock category: weapon, upgrade, buildable, or class.
// @tparam string thing Unlock token, for example bsuit or shotgun.
// @treturn boolean Whether the unlock is already purchased.
int OverloadIsPurchased( lua_State* L )
{
	team_t team = ParseTeamArg( L, 1 );
	unlockableType_t type = ParseUnlockTypeArg( L, 2 );
	const char* thing = luaL_checkstring( L, 3 );

	lua_pushboolean( L,
	                 team != TEAM_NONE &&
	                 type != UNLT_NUM_UNLOCKABLETYPES &&
	                 G_OverloadUnlockPurchasedByName( team, type, thing ) );
	return 1;
}

/// Force-complete a specific Overload unlock.
// @function force_unlock
// @tparam string team Team name: humans or aliens.
// @tparam string type Unlock category: weapon, upgrade, buildable, or class.
// @tparam string thing Unlock token, for example bsuit or shotgun.
int OverloadForceUnlock( lua_State* L )
{
	team_t team = ParseTeamArg( L, 1 );
	unlockableType_t type = ParseUnlockTypeArg( L, 2 );
	const char* thing = luaL_checkstring( L, 3 );

	if ( team != TEAM_NONE && type != UNLT_NUM_UNLOCKABLETYPES )
	{
		G_OverloadForceUnlockByName( team, type, thing );
	}

	return 0;
}

/// Get the current rank of an Overload upgrade.
// @function upgrade_level
// @tparam string team Team name: humans or aliens.
// @tparam string thing Upgrade target token, for example shotgun.
// @tparam string stat Upgrade stat token, for example damage or ammo.
// @treturn integer Current rank for the upgrade.
int OverloadUpgradeLevel( lua_State* L )
{
	team_t team = ParseTeamArg( L, 1 );
	const char* thing = luaL_checkstring( L, 2 );
	const char* stat = luaL_checkstring( L, 3 );

	lua_pushinteger( L, team == TEAM_NONE ? 0 : G_OverloadUpgradeLevel( team, thing, stat ) );
	return 1;
}

/// Force-advance an Overload upgrade by exactly one rank.
// @function force_upgrade
// @tparam string team Team name: humans or aliens.
// @tparam string thing Upgrade target token, for example shotgun.
// @tparam string stat Upgrade stat token, for example damage or ammo.
// @treturn integer Resulting rank for the upgrade.
int OverloadForceUpgrade( lua_State* L )
{
	team_t team = ParseTeamArg( L, 1 );
	const char* thing = luaL_checkstring( L, 2 );
	const char* stat = luaL_checkstring( L, 3 );

	lua_pushinteger( L, team == TEAM_NONE ? 0 : G_OverloadForceUpgrade( team, thing, stat ) );
	return 1;
}

}  // namespace

namespace Lua {

RegType<Overload> OverloadMethods[] = {
	{ nullptr, nullptr },
};

luaL_Reg OverloadGetters[] = {
	{ nullptr, nullptr },
};

luaL_Reg OverloadSetters[] = {
	{ nullptr, nullptr },
};

}  // namespace Lua

namespace Shared {
namespace Lua {

LUACORETYPEDEFINE( ::Lua::Overload )

template <>
void ExtraInit<::Lua::Overload>( lua_State* L, int metatable_index )
{
	lua_pushcfunction( L, ::OverloadIsPurchased );
	lua_setfield( L, metatable_index - 1, "is_purchased" );
	lua_pushcfunction( L, ::OverloadForceUnlock );
	lua_setfield( L, metatable_index - 1, "force_unlock" );
	lua_pushcfunction( L, ::OverloadUpgradeLevel );
	lua_setfield( L, metatable_index - 1, "upgrade_level" );
	lua_pushcfunction( L, ::OverloadForceUpgrade );
	lua_setfield( L, metatable_index - 1, "force_upgrade" );
}

}  // namespace Lua
}  // namespace Shared
