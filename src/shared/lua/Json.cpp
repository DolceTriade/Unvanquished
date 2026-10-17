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
#include "shared/lua/Json.h"

#include "common/Common.h"
#include "shared/bg_lua.h"

extern "C" int luaopen_cjson( lua_State *L );
extern "C" int luaopen_cjson_safe( lua_State *L );

namespace Shared
{
namespace Lua
{

namespace
{
void PushSafeMethod( lua_State *L, const char *name )
{
	lua_getglobal( L, "package" );
	lua_getfield( L, -1, "loaded" );
	lua_getfield( L, -1, "cjson.safe" );
	lua_getfield( L, -1, name );
	lua_remove( L, -2 );
	lua_remove( L, -2 );
	lua_remove( L, -2 );
	if ( !lua_isfunction( L, -1 ) )
	{
		Sys::Drop( "cjson.safe.%s is unavailable", name );
	}
}
}  // namespace

void RegisterJson( lua_State *L )
{
	luaL_requiref( L, "cjson", luaopen_cjson, 1 );
	lua_pop( L, 1 );
	luaL_requiref( L, "cjson.safe", luaopen_cjson_safe, 1 );
	lua_pop( L, 1 );
}

bool EncodeJson( lua_State *L, int valueIndex, std::string &json, std::string &error )
{
	const int base = lua_gettop( L );
	valueIndex = lua_absindex( L, valueIndex );
	PushSafeMethod( L, "encode" );
	lua_pushvalue( L, valueIndex );
	if ( lua_pcall( L, 1, 2, 0 ) != LUA_OK )
	{
		const char *message = lua_tostring( L, -1 );
		error = message ? message : "JSON encoding failed";
		lua_settop( L, base );
		return false;
	}
	if ( !lua_isstring( L, -2 ) )
	{
		const char *message = lua_tostring( L, -1 );
		error = message ? message : "JSON encoding failed";
		lua_settop( L, base );
		return false;
	}
	size_t length = 0;
	const char *encoded = lua_tolstring( L, -2, &length );
	json.assign( encoded, length );
	lua_settop( L, base );
	return true;
}

bool DecodeJson( lua_State *L, const char *json, std::string &error )
{
	const int base = lua_gettop( L );
	PushSafeMethod( L, "decode" );
	lua_pushstring( L, json );
	if ( lua_pcall( L, 1, 2, 0 ) != LUA_OK )
	{
		const char *message = lua_tostring( L, -1 );
		error = message ? message : "JSON decoding failed";
		lua_settop( L, base );
		return false;
	}
	if ( lua_isnil( L, base + 1 ) )
	{
		const char *message = lua_tostring( L, base + 2 );
		error = message ? message : "JSON decoding failed";
		lua_settop( L, base );
		return false;
	}
	lua_settop( L, base + 1 );
	return true;
}
}  // namespace Lua
}  // namespace Shared
