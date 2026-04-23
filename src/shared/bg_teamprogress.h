#ifndef BG_TEAMPROGRESS_H_
#define BG_TEAMPROGRESS_H_

#include "bg_public.h"

struct unlockable_t
{
	int    type;
	int    num;
	team_t team;
	bool   unlocked;
	bool   statusKnown;
	int    unlockThreshold;
	int    lockThreshold;
};

extern bool         unlockablesDataAvailable;
extern int          unlockablesTeamKnowledge;
extern unlockable_t unlockables[ NUM_UNLOCKABLES ];
extern int          unlockablesTypeOffset[ UNLT_NUM_UNLOCKABLETYPES ];

const char *UnlockableHumanName( unlockable_t *unlockable );
float      UnlockToLockThreshold( float unlockThreshold );
int        NormalizeUnlockThreshold( int unlockThreshold );

void BG_ClearUnlockablesMasks();
void BG_ResetUnlockablesMask( team_t team );
void BG_SetUnlockablesMaskBit( team_t team, int bit );
void BG_SetUnlockablesMask( team_t team, int mask );
void BG_TeamProgressNotifyStatusChanges( const int *statusChanges, int count );

#endif // BG_TEAMPROGRESS_H_
