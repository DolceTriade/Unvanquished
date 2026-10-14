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

#include <limits>
#include <cstring>
#include <vector>

#include "shared/CommonProxies.h"
#include "shared/bg_public.h"
#include "sgame/lua/register_lua_extensions.h"

namespace Lua {
namespace {

int Read( lua_State* L )
{
	const char* path = luaL_checkstring( L, 1 );
	fileHandle_t file;
	int length = BG_FOpenGameOrPakPath( path, file );
	if ( length < 0 )
	{
		return luaL_error( L, "File.read: failed to open '%s'", path );
	}

	std::string contents( length, '\0' );
	int read = length ? trap_FS_Read( contents.data(), length, file ) : 0;
	trap_FS_FCloseFile( file );
	if ( read != length )
	{
		return luaL_error( L, "File.read: failed to read '%s'", path );
	}

	lua_pushlstring( L, contents.data(), contents.size() );
	return 1;
}

int Write( lua_State* L )
{
	const char* path = luaL_checkstring( L, 1 );
	size_t length = 0;
	const char* contents = luaL_checklstring( L, 2, &length );
	if ( length > static_cast<size_t>( std::numeric_limits<int>::max() ) )
	{
		return luaL_error( L, "File.write: contents are too large" );
	}

	fileHandle_t file;
	if ( trap_FS_FOpenFile( path, &file, fsMode_t::FS_WRITE ) < 0 )
	{
		return luaL_error( L, "File.write: failed to open '%s'", path );
	}

	int written = trap_FS_Write( contents, static_cast<int>( length ), file );
	trap_FS_FCloseFile( file );
	if ( written != static_cast<int>( length ) )
	{
		return luaL_error( L, "File.write: failed to write '%s'", path );
	}

	return 0;
}

int List( lua_State* L )
{
	const char* path = luaL_checkstring( L, 1 );
	const char* extension = luaL_optstring( L, 2, "" );
	bool recursive = lua_toboolean( L, 3 );

	// The filesystem API returns a NUL-separated list and has no separate
	// query for the required buffer size. Keep the buffer large enough for
	// normal game directories while retaining the engine's integer-size API.
	constexpr int LIST_BUFFER_SIZE = 1024 * 1024;
	std::vector<char> buffer( LIST_BUFFER_SIZE, '\0' );
	int count = recursive
		? trap_FS_GetFileListRecursive( path, extension, buffer.data(), buffer.size() )
		: trap_FS_GetFileList( path, extension, buffer.data(), buffer.size() );

	lua_createtable( L, count, 0 );
	size_t offset = 0;
	for ( int i = 0; i < count; ++i )
	{
		if ( offset >= buffer.size() )
		{
			return luaL_error( L, "File.list: malformed file list for '%s'", path );
		}
		size_t length = std::strlen( buffer.data() + offset );
		if ( offset + length >= buffer.size() )
		{
			return luaL_error( L, "File.list: malformed file list for '%s'", path );
		}
		lua_pushlstring( L, buffer.data() + offset, length );
		lua_rawseti( L, -2, i + 1 );
		offset += length + 1;
	}
	return 1;
}

} // namespace

void RegisterFile( lua_State* L )
{
	lua_newtable( L );
	lua_pushcfunction( L, Read );
	lua_setfield( L, -2, "read" );
	lua_pushcfunction( L, Write );
	lua_setfield( L, -2, "write" );
	lua_pushcfunction( L, List );
	lua_setfield( L, -2, "list" );
	lua_setglobal( L, "File" );
}

} // namespace Lua
