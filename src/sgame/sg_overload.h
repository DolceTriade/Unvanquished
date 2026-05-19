/*
===========================================================================

Unvanquished GPL Source Code
Copyright (C) 2026 Unvanquished Developers

This file is part of the Unvanquished GPL Source Code (Unvanquished Source Code).

Unvanquished is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Unvanquished is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Unvanquished. If not, see <http://www.gnu.org/licenses/>.

===========================================================================
*/

#ifndef SG_OVERLOAD_H_
#define SG_OVERLOAD_H_

#include "sg_local.h"
#include "shared/bg_attributes.h"

#include <string>
#include <vector>

enum class overloadPurchaseKind_t
{
	BP_BUNDLE,
	UNLOCK,
	UPGRADE,
};

enum class effectTarget_t
{
	GAMEPLAY,
	ATTRIBUTE,
};

enum class effectValueType_t
{
	INTEGER,
	FLOAT,
};

struct overloadEffect_t
{
	effectTarget_t      target;
	effectValueType_t   valueType;
	int                 gameplayIndex;
	bgAttributeFamily_t attributeFamily;
	int                 attributeObject;
	int                 attributeField;
	double              baseline;
	double              step;
	double              minValue;
	double              maxValue;
};

struct overloadPurchaseDef_t
{
	overloadPurchaseKind_t kind;
	team_t                 team;
	std::string            thing;
	std::string            thingLabel;
	std::string            stat;
	std::string            statLabel;
	std::string            displayName;
	std::string            uiDescription;
	int                    requiredCompletedCount;
	int                    baseCost;
	int                    costStep;
	int                    bundleAmount;
	int                    maxRanks;
	bgAttributeFamily_t    unlockFamily;
	int                    unlockObject;
	int                    unlockField;
	std::vector<overloadEffect_t> effects;
};

extern std::vector<overloadPurchaseDef_t> overloadPurchases;

TeamEconomyState& TeamEconomy( team_t team );
bool EntryIsAvailable( team_t team, const overloadPurchaseDef_t& entry );
int RemainingSpendCapacity( const overloadPurchaseDef_t& entry, int entryIndex, team_t team );

#endif
