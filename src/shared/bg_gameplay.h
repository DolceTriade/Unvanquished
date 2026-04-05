/*
===========================================================================

Unvanquished GPL Source Code
Copyright (C) 2012-2013 Unvanquished Developers

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

#ifndef G_GAMEPLAY_H_
#define G_GAMEPLAY_H_

enum gameplayVarType_t
{
	GAMEPLAY_INTEGER,
	GAMEPLAY_FLOAT
};

enum gameplayVarFlags_t
{
	GAMEPLAY_WRITABLE = 0,
	GAMEPLAY_DERIVED = 1 << 0
};

union gameplayValue_t
{
	int integer;
	float number;
};

struct gameplayVarInfo_t
{
	const char* name;
	const char* alias;
	gameplayVarType_t type;
	int flags;
	void* storage;
};

/*
 *============
 * Constants
 *============
 */

/*
 * Common constants for the build weapons for both teams.
 */

// Pressing +deconstruct for less than this many milliseconds toggles the deconstruction mark
#define BG_GAMEPLAY_VAR(type, name, default_value, flags, alias) extern type name;
#include "bg_gameplay.def"
#undef BG_GAMEPLAY_VAR

#define QU_TO_METER 0.03125f // in m/qu

// how long you can sustain underwater before taking damage
#define OXYGEN_MAX_TIME       12000
// how many bits are needed to store this number (plus a margin cgame needs)
#define LOW_OXYGEN_TIME_BITS  14

#define LEVEL2_AREAZAP_MAX_TARGETS 6  // Hard limit since BG_PackEntityNumbers can only handle this much.

// movement
#define MIN_WALK_NORMAL   0.7f // can't walk on very steep slopes
#define STEPSIZE          18

#define MISSILE_PRESTEP_TIME 50

size_t BG_NumGameplayVars();
const gameplayVarInfo_t* BG_GameplayVar( size_t index );
int BG_FindGameplayVarByName( const char* name );
bool BG_CheckConfigVars();
void BG_ResetGameplayToDefaults();
void BG_CommitGameplayBaseline();
bool BG_ParseConfigVar( const char* varName, const char** text, const char* filename );
bool BG_SetGameplayInt( size_t index, int value, bool updateOverride, std::string* error = nullptr );
bool BG_SetGameplayFloat( size_t index, float value, bool updateOverride, std::string* error = nullptr );
bool BG_ResetGameplayValue( size_t index, bool updateOverride, std::string* error = nullptr );
void BG_ResetGameplayOverrides();
std::string BG_BuildGameplayConfig();
bool BG_ApplyGameplayConfig( const char* config, std::string* error = nullptr );

#ifdef BUILD_SGAME
void BG_PublishGameplayConfig();
#endif

#endif // G_GAMEPLAY_H_
