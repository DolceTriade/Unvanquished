/*
===========================================================================

Unvanquished GPL Source Code
Copyright (C) 2012-2023 Unvanquished Developers

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

#include "common/Common.h"
#include "bg_gameplay.h"

#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

#ifdef BUILD_SGAME
#include "sgame/sg_local.h"
#endif

#define BG_GAMEPLAY_VAR(type, name, default_value, flags, alias) type name = default_value;
#include "bg_gameplay.def"
#undef BG_GAMEPLAY_VAR

namespace {

constexpr int GAMEPLAY_CONFIG_SCHEMA = 1;

template<typename T>
constexpr gameplayVarType_t GameplayType();

template<>
constexpr gameplayVarType_t GameplayType<int>()
{
	return GAMEPLAY_INTEGER;
}

template<>
constexpr gameplayVarType_t GameplayType<float>()
{
	return GAMEPLAY_FLOAT;
}

struct gameplayVarState_t
{
	gameplayVarInfo_t info;
	gameplayValue_t baseline;
	gameplayValue_t overrideValue;
	bool overridePresent;
	bool configDefined;
};

gameplayVarState_t gameplayVars[] =
{
#define BG_GAMEPLAY_VAR(type, name, default_value, flags, alias) \
	{ { #name, alias, GameplayType<type>(), flags, &name }, {}, {}, false, alias == nullptr },
#include "bg_gameplay.def"
#undef BG_GAMEPLAY_VAR
};

constexpr size_t numGameplayVars = ARRAY_LEN( gameplayVars );

void SetError( std::string* error, Str::StringRef message )
{
	if ( error )
	{
		*error = message;
	}
}

float CanonicalizeFloat( float value )
{
	std::ostringstream stream;
	stream.imbue( std::locale::classic() );
	stream << std::setprecision( std::numeric_limits<float>::max_digits10 ) << value;

	std::istringstream parse( stream.str() );
	parse.imbue( std::locale::classic() );

	float canonical = value;
	if ( !( parse >> canonical ) )
	{
		Sys::Drop( "failed to canonicalize gameplay float" );
	}

	return canonical;
}

gameplayValue_t ReadCurrentValue( const gameplayVarState_t& state )
{
	gameplayValue_t value{};
	if ( state.info.type == GAMEPLAY_INTEGER )
	{
		value.integer = *static_cast<int*>( state.info.storage );
	}
	else
	{
		value.number = *static_cast<float*>( state.info.storage );
	}
	return value;
}

void WriteCurrentValue( gameplayVarState_t& state, gameplayValue_t value )
{
	if ( state.info.type == GAMEPLAY_INTEGER )
	{
		*static_cast<int*>( state.info.storage ) = value.integer;
	}
	else
	{
		*static_cast<float*>( state.info.storage ) = value.number;
	}
}

bool ValuesEqual( const gameplayVarState_t& state, gameplayValue_t lhs, gameplayValue_t rhs )
{
	if ( state.info.type == GAMEPLAY_INTEGER )
	{
		return lhs.integer == rhs.integer;
	}
	return lhs.number == rhs.number;
}

void RecomputeDerivedValues()
{
	AVG_FALL_DISTANCE = static_cast<int>( ( MIN_FALL_DISTANCE + MAX_FALL_DISTANCE ) / 2.0f );
	JETPACK_FUEL_IGNITE = JETPACK_FUEL_MAX / 20.0f;
	JETPACK_FUEL_LOW = JETPACK_FUEL_MAX / 5.0f;
	JETPACK_FUEL_STOP = JETPACK_FUEL_RESTORE * 150.0f;
	JETPACK_FUEL_REFUEL = JETPACK_FUEL_MAX - JETPACK_FUEL_USAGE * 1000.0f;
}

bool ParseGameplayValue( const gameplayVarState_t& state, const char* token, gameplayValue_t& value )
{
	if ( state.info.type == GAMEPLAY_INTEGER )
	{
		value.integer = atoi( token );
		return true;
	}

	value.number = atof( token );
	return true;
}

void RefreshOverrideState( gameplayVarState_t& state )
{
	if ( state.info.flags & GAMEPLAY_DERIVED )
	{
		state.overridePresent = false;
		return;
	}

	gameplayValue_t current = ReadCurrentValue( state );
	if ( ValuesEqual( state, current, state.baseline ) )
	{
		state.overridePresent = false;
		return;
	}

	state.overrideValue = current;
	state.overridePresent = true;
}

}  // namespace

size_t BG_NumGameplayVars()
{
	return numGameplayVars;
}

const gameplayVarInfo_t* BG_GameplayVar( size_t index )
{
	return index < numGameplayVars ? &gameplayVars[ index ].info : nullptr;
}

int BG_FindGameplayVarByName( const char* name )
{
	for ( size_t i = 0; i < numGameplayVars; ++i )
	{
		if ( !Q_stricmp( gameplayVars[ i ].info.name, name ) )
		{
			return i;
		}
	}

	return -1;
}

bool BG_CheckConfigVars()
{
	bool ok = true;

	for ( gameplayVarState_t& state : gameplayVars )
	{
		if ( state.info.alias && !state.configDefined )
		{
			ok = false;
			Log::Warn( "config var %s was not defined", state.info.alias );
		}
	}

	return ok;
}

void BG_ResetGameplayToDefaults()
{
#define BG_GAMEPLAY_VAR(type, name, default_value, flags, alias) name = default_value;
#include "bg_gameplay.def"
#undef BG_GAMEPLAY_VAR

	for ( gameplayVarState_t& state : gameplayVars )
	{
		state.overridePresent = false;
		state.configDefined = state.info.alias == nullptr;
	}

	RecomputeDerivedValues();
}

void BG_CommitGameplayBaseline()
{
	RecomputeDerivedValues();

	for ( gameplayVarState_t& state : gameplayVars )
	{
		state.baseline = ReadCurrentValue( state );
		state.overridePresent = false;
	}
}

bool BG_ParseConfigVar( const char* varName, const char** text, const char* filename )
{
	for ( gameplayVarState_t& state : gameplayVars )
	{
		if ( state.info.alias && !Q_stricmp( state.info.alias, varName ) )
		{
			const char* token = COM_Parse( text );
			if ( !*token )
			{
				Log::Warn( "%s expected argument for '%s'", filename, varName );
				return false;
			}

			gameplayValue_t value{};
			ParseGameplayValue( state, token, value );
			WriteCurrentValue( state, value );
			state.configDefined = true;
			return true;
		}
	}

	return false;
}

bool BG_SetGameplayInt( size_t index, int value, bool updateOverride, std::string* error )
{
	if ( index >= numGameplayVars )
	{
		SetError( error, "invalid gameplay variable index" );
		return false;
	}

	gameplayVarState_t& state = gameplayVars[ index ];
	if ( state.info.type != GAMEPLAY_INTEGER )
	{
		SetError( error, "gameplay variable expects a float" );
		return false;
	}

	if ( state.info.flags & GAMEPLAY_DERIVED )
	{
		SetError( error, "gameplay variable is derived and read-only" );
		return false;
	}

	gameplayValue_t gameplayValue{};
	gameplayValue.integer = value;
	WriteCurrentValue( state, gameplayValue );
	RecomputeDerivedValues();
	if ( updateOverride )
	{
		RefreshOverrideState( state );
	}
	return true;
}

bool BG_SetGameplayFloat( size_t index, float value, bool updateOverride, std::string* error )
{
	if ( index >= numGameplayVars )
	{
		SetError( error, "invalid gameplay variable index" );
		return false;
	}

	gameplayVarState_t& state = gameplayVars[ index ];
	if ( state.info.type != GAMEPLAY_FLOAT )
	{
		SetError( error, "gameplay variable expects an integer" );
		return false;
	}

	if ( state.info.flags & GAMEPLAY_DERIVED )
	{
		SetError( error, "gameplay variable is derived and read-only" );
		return false;
	}

	gameplayValue_t gameplayValue{};
	gameplayValue.number = CanonicalizeFloat( value );
	WriteCurrentValue( state, gameplayValue );
	RecomputeDerivedValues();
	if ( updateOverride )
	{
		RefreshOverrideState( state );
	}
	return true;
}

bool BG_ResetGameplayValue( size_t index, bool updateOverride, std::string* error )
{
	if ( index >= numGameplayVars )
	{
		SetError( error, "invalid gameplay variable index" );
		return false;
	}

	gameplayVarState_t& state = gameplayVars[ index ];
	if ( state.info.flags & GAMEPLAY_DERIVED )
	{
		SetError( error, "gameplay variable is derived and read-only" );
		return false;
	}

	WriteCurrentValue( state, state.baseline );
	RecomputeDerivedValues();
	if ( updateOverride )
	{
		state.overridePresent = false;
	}
	return true;
}

void BG_ResetGameplayOverrides()
{
	for ( gameplayVarState_t& state : gameplayVars )
	{
		WriteCurrentValue( state, state.baseline );
		state.overridePresent = false;
	}
	RecomputeDerivedValues();
}

std::string BG_BuildGameplayConfig()
{
	std::ostringstream config;
	config.imbue( std::locale::classic() );
	config << std::setprecision( std::numeric_limits<float>::max_digits10 );

	size_t overrideCount = 0;
	for ( const gameplayVarState_t& state : gameplayVars )
	{
		if ( state.overridePresent )
		{
			overrideCount++;
		}
	}

	config << GAMEPLAY_CONFIG_SCHEMA << ' ' << numGameplayVars << ' ' << overrideCount;

	for ( size_t i = 0; i < numGameplayVars; ++i )
	{
		const gameplayVarState_t& state = gameplayVars[ i ];
		if ( !state.overridePresent )
		{
			continue;
		}

		config << ' ' << i << ' ';
		if ( state.info.type == GAMEPLAY_INTEGER )
		{
			config << state.overrideValue.integer;
		}
		else
		{
			config << state.overrideValue.number;
		}
	}

	return config.str();
}

bool BG_ApplyGameplayConfig( const char* config, std::string* error )
{
	BG_ResetGameplayOverrides();

	if ( !config || !*config )
	{
		return true;
	}

	std::istringstream stream( config );
	stream.imbue( std::locale::classic() );
	int schema = 0;
	size_t fieldCount = 0;
	size_t overrideCount = 0;

	if ( !( stream >> schema >> fieldCount >> overrideCount ) )
	{
		SetError( error, "malformed gameplay config" );
		return false;
	}

	if ( schema != GAMEPLAY_CONFIG_SCHEMA )
	{
		SetError( error, "unsupported gameplay config schema" );
		return false;
	}

	if ( fieldCount != numGameplayVars )
	{
		SetError( error, "gameplay config field count mismatch" );
		return false;
	}

	for ( size_t i = 0; i < overrideCount; ++i )
	{
		size_t index = 0;
		if ( !( stream >> index ) || index >= numGameplayVars )
		{
			SetError( error, "invalid gameplay config index" );
			return false;
		}

		gameplayVarState_t& state = gameplayVars[ index ];
		gameplayValue_t value{};

		if ( state.info.type == GAMEPLAY_INTEGER )
		{
			if ( !( stream >> value.integer ) )
			{
				SetError( error, "malformed gameplay integer override" );
				return false;
			}
		}
		else
		{
			if ( !( stream >> value.number ) )
			{
				SetError( error, "malformed gameplay float override" );
				return false;
			}
		}

		WriteCurrentValue( state, value );
		state.overrideValue = value;
		state.overridePresent = true;
	}

	RecomputeDerivedValues();
	return true;
}

#ifdef BUILD_SGAME
void BG_PublishGameplayConfig()
{
	const std::string config = BG_BuildGameplayConfig();
	if ( config.size() >= BIG_INFO_STRING )
	{
		Sys::Drop( "gameplay config too large to publish (%zu bytes, max %zu)",
		           config.size(), BIG_INFO_STRING - 1 );
	}
	trap_SetConfigstring( CS_GAMEPLAY, config.c_str() );
}
#endif
