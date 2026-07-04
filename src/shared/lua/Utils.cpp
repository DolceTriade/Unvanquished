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
#include "shared/lua/Utils.h"

#include "shared/bg_attributes.h"
#include "shared/bg_public.h"

namespace Shared {
namespace Lua {

void Report( lua_State* L, Str::StringRef place )
{
	Log::Warn( place );

	while ( true )
	{
		lua_gettop( L );
		const char* msg = lua_tostring( L, -1 );
		if (msg)
		{
			Log::Warn( msg );
			lua_pop( L, 1 );
			continue;
		}
		break;
	}
}

void PushVec3(lua_State* L, const vec3_t vec)
{
	lua_newtable(L);
	for (int i = 0; i < 3; ++i)
	{
		lua_pushnumber(L, vec[i]);
		lua_rawseti(L, -2, i + 1); // lua arrays start at 1
	}
}

bool CheckVec3(lua_State* L, int pos, vec3_t vec)
{
	if ( lua_istable( L, pos ) )
	{
		for ( int i = 0; i < 3; ++i )
		{
			lua_rawgeti( L, pos, i + 1 );
			if ( !lua_isnumber( L, -1 ) )
			{
				lua_pop( L, 1 );
				Log::Warn( "CheckVec3: Input table must contain three numbers." );
				return false;
			}
			vec[ i ] = lua_tonumber( L, -1 );
			lua_pop( L, 1 );
		}
		return true;
	}

	if ( lua_isnumber( L, pos ) &&
	     lua_isnumber( L, pos + 1 ) &&
	     lua_isnumber( L, pos + 2 ) )
	{
		for ( int i = 0; i < 3; ++i )
		{
			vec[ i ] = lua_tonumber( L, pos + i );
		}
		return true;
	}

	Log::Warn( "CheckVec3: Input must be a vec3 table or three numeric arguments." );
	return false;
}

int SetAttributeInt( lua_State* L, bgAttributeFamily_t family, size_t objectIndex, const char* ownerName, const char* fieldName, int valueIndex )
{
#ifndef BUILD_SGAME
	(void) family;
	(void) objectIndex;
	(void) fieldName;
	(void) valueIndex;
	return luaL_error( L, "%s attributes are read-only on the client", ownerName );
#else
	std::string error;
	const int field = BG_FindAttributeField( family, fieldName );
	if ( !lua_isinteger( L, valueIndex ) )
	{
		return luaL_error( L, "%s field '%s' expects an integer", ownerName, fieldName );
	}
	if ( !BG_SetAttributeInt( family, objectIndex, field, lua_tointeger( L, valueIndex ), true, &error ) )
	{
		return luaL_error( L, "%s", error.c_str() );
	}
	BG_PublishAttributeConfig();
	return 0;
#endif
}

int SetAttributeFloat( lua_State* L, bgAttributeFamily_t family, size_t objectIndex, const char* ownerName, const char* fieldName, int valueIndex )
{
#ifndef BUILD_SGAME
	(void) family;
	(void) objectIndex;
	(void) fieldName;
	(void) valueIndex;
	return luaL_error( L, "%s attributes are read-only on the client", ownerName );
#else
	std::string error;
	const int field = BG_FindAttributeField( family, fieldName );
	if ( !lua_isnumber( L, valueIndex ) )
	{
		return luaL_error( L, "%s field '%s' expects a number", ownerName, fieldName );
	}
	if ( !BG_SetAttributeFloat( family, objectIndex, field, lua_tonumber( L, valueIndex ), true, &error ) )
	{
		return luaL_error( L, "%s", error.c_str() );
	}
	BG_PublishAttributeConfig();
	return 0;
#endif
}

int SetAttributeBool( lua_State* L, bgAttributeFamily_t family, size_t objectIndex, const char* ownerName, const char* fieldName, int valueIndex )
{
#ifndef BUILD_SGAME
	(void) family;
	(void) objectIndex;
	(void) fieldName;
	(void) valueIndex;
	return luaL_error( L, "%s attributes are read-only on the client", ownerName );
#else
	std::string error;
	const int field = BG_FindAttributeField( family, fieldName );
	if ( !lua_isboolean( L, valueIndex ) )
	{
		return luaL_error( L, "%s field '%s' expects a boolean", ownerName, fieldName );
	}
	if ( !BG_SetAttributeBool( family, objectIndex, field, lua_toboolean( L, valueIndex ) != 0, true, &error ) )
	{
		return luaL_error( L, "%s", error.c_str() );
	}
	BG_PublishAttributeConfig();
	return 0;
#endif
}

int ResetAttribute( lua_State* L, bgAttributeFamily_t family, size_t objectIndex, const char* ownerName, const char* fieldName )
{
	const int field = BG_FindAttributeField( family, fieldName );
	if ( field < 0 )
	{
		return luaL_error( L, "unknown %s field '%s'", ownerName, fieldName );
	}

#ifndef BUILD_SGAME
	(void) objectIndex;
	return luaL_error( L, "%s attributes are read-only on the client", ownerName );
#else
	std::string error;
	if ( !BG_ResetAttributeValue( family, objectIndex, field, true, &error ) )
	{
		return luaL_error( L, "%s", error.c_str() );
	}
	BG_PublishAttributeConfig();
	return 0;
#endif
}

int ResetAttributeFamily( lua_State* L, bgAttributeFamily_t family, const char* ownerName )
{
#ifndef BUILD_SGAME
	(void) family;
	return luaL_error( L, "%s attributes are read-only on the client", ownerName );
#else
	(void) L;
	(void) ownerName;
	BG_ResetAttributeFamilyOverrides( family );
	BG_PublishAttributeConfig();
	return 0;
#endif
}

namespace {

struct PairsHelper
{
	size_t cur;
	std::function<int( lua_State* L, size_t& index )> next_func;
};

int next( lua_State* L )
{
	PairsHelper* self = static_cast<PairsHelper*>( lua_touserdata( L, lua_upvalueindex( 1 ) ) );
	int ret = self->next_func( L, self->cur );
	self->cur++;
	return ret;
}

int destroy( lua_State* L )
{
	static_cast<PairsHelper*>( lua_touserdata( L, 1 ) )->~PairsHelper();
	return 0;
}

}  // namespace

int CreatePairsHelper( lua_State* L, std::function<int( lua_State* L, size_t& index )> next_func )
{
	void* storage = lua_newuserdata( L, sizeof( PairsHelper ) );
	if ( luaL_newmetatable( L, "Shared::Lua::PairsHelper" ) )
	{
		static luaL_Reg mt[] = {
			{ "__gc", destroy },
			{ NULL, NULL },
		};
		luaL_setfuncs( L, mt, 0 );
	}
	lua_setmetatable( L, -2 );
	lua_pushcclosure( L, next, 1 );
	new ( storage ) PairsHelper{ 0, next_func };
	return 1;
}

}  // namespace Lua
}  // namespace Shared
