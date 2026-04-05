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

#ifndef BG_ATTRIBUTES_H_
#define BG_ATTRIBUTES_H_

#include "common/Common.h"

enum bgAttributeFamily_t
{
	BG_ATTR_BUILDABLE,
	BG_ATTR_CLASS,
	BG_ATTR_WEAPON,
	BG_ATTR_UPGRADE,
	BG_ATTR_MISSILE,
	BG_ATTR_BEACON,
	BG_NUM_ATTRIBUTE_FAMILIES
};

enum bgAttributeValueType_t
{
	BG_ATTR_INTEGER,
	BG_ATTR_FLOAT,
	BG_ATTR_BOOL
};

union bgAttributeValue_t
{
	int integer;
	float number;
	bool boolean;
};

struct bgAttributeFieldInfo_t
{
	const char* name;
	bgAttributeValueType_t type;
};

struct bgAttributeTrackedField_t
{
	bgAttributeFieldInfo_t info;
	size_t offset;
};

const char* BG_AttributeFamilyName( bgAttributeFamily_t family );
size_t BG_NumAttributeFields( bgAttributeFamily_t family );
const bgAttributeFieldInfo_t* BG_AttributeField( bgAttributeFamily_t family, size_t fieldIndex );
int BG_FindAttributeField( bgAttributeFamily_t family, const char* name );
size_t BG_NumAttributeObjects( bgAttributeFamily_t family );
const char* BG_AttributeObjectName( bgAttributeFamily_t family, size_t objectIndex );
int BG_FindAttributeObject( bgAttributeFamily_t family, const char* name );
bool BG_GetAttributeValue( bgAttributeFamily_t family, size_t objectIndex, size_t fieldIndex, bgAttributeValue_t* value, std::string* error = nullptr );
bool BG_SetAttributeInt( bgAttributeFamily_t family, size_t objectIndex, size_t fieldIndex, int value, bool updateOverride, std::string* error = nullptr );
bool BG_SetAttributeFloat( bgAttributeFamily_t family, size_t objectIndex, size_t fieldIndex, float value, bool updateOverride, std::string* error = nullptr );
bool BG_SetAttributeBool( bgAttributeFamily_t family, size_t objectIndex, size_t fieldIndex, bool value, bool updateOverride, std::string* error = nullptr );
bool BG_ResetAttributeValue( bgAttributeFamily_t family, size_t objectIndex, size_t fieldIndex, bool updateOverride, std::string* error = nullptr );
void BG_ResetAttributeFamilyOverrides( bgAttributeFamily_t family );
void BG_ResetAllAttributeOverrides();
void BG_CommitAttributeBaselines();
std::string BG_BuildAttributeConfig();
bool BG_ApplyAttributeConfig( const char* config, std::string* error = nullptr );

#ifdef BUILD_SGAME
void BG_PublishAttributeConfig();
#endif

#endif // BG_ATTRIBUTES_H_
