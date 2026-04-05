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

===========================================================================
*/
#ifndef SHARED_LUA_MISSILES_H_
#define SHARED_LUA_MISSILES_H_

#include "common/Common.h"
#include "shared/bg_attributes.h"
#include "shared/bg_lua.h"
#include "shared/bg_public.h"
#include "shared/lua/LuaLib.h"

namespace Shared {
namespace Lua {

struct MissileProxy
{
	MissileProxy( int missile );

	int missile;
	const missileAttributes_t* attributes;
};

struct Missiles
{
	static int index( lua_State* L );
	static int pairs( lua_State* L );
	static int reset( lua_State* L, Missiles* self );
	static int reset_all( lua_State* L, Missiles* self );

	static std::vector<MissileProxy> missiles;
};

const bgAttributeTrackedField_t* MissileAttributeFields();
size_t NumMissileAttributeFields();

template<>
void ExtraInit<Missiles>( lua_State* L, int metatable_index );
template<>
void ExtraInit<MissileProxy>( lua_State* L, int metatable_index );

} // namespace Lua
} // namespace Shared

#endif // SHARED_LUA_MISSILES_H_
