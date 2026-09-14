#ifndef BG_OVERLOAD_H_
#define BG_OVERLOAD_H_

#include "bg_public.h"

enum class overloadPurchaseKind_t
{
	BP_BUNDLE,
	UNLOCK,
	UPGRADE,
};

inline const char* BG_OverloadPurchaseKindToken( overloadPurchaseKind_t kind )
{
	switch ( kind )
	{
		case overloadPurchaseKind_t::BP_BUNDLE: return "bp";
		case overloadPurchaseKind_t::UNLOCK: return "unlock";
		case overloadPurchaseKind_t::UPGRADE: return "upgrade";
	}

	return "upgrade";
}

inline overloadPurchaseKind_t BG_OverloadPurchaseKindFromToken( const char* token )
{
	if ( !Q_stricmp( token, "bp" ) )
	{
		return overloadPurchaseKind_t::BP_BUNDLE;
	}

	if ( !Q_stricmp( token, "unlock" ) )
	{
		return overloadPurchaseKind_t::UNLOCK;
	}

	return overloadPurchaseKind_t::UPGRADE;
}

#endif // BG_OVERLOAD_H_
