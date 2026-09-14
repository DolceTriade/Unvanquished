#include "common/Common.h"
#include "cg_local.h"
#include "shared/bg_quakerewards.h"

namespace QuakeRewards {

namespace {

Cvar::Cvar<bool> soundsEnabled(
	"cg_quakeRewards",
	"enable/disable Quake reward sounds",
	Cvar::NONE, true);

constexpr const char *quakeRewardSounds[ QR_NUM_REWARDS ] = {
	nullptr,
	"sound/feedback/quakesounds/firstblood",
	"sound/feedback/quakesounds/humiliated",
	"sound/feedback/quakesounds/betrayal",
	"sound/feedback/quakesounds/haha",
	"sound/feedback/quakesounds/doublekill",
	"sound/feedback/quakesounds/triplekill",
	"sound/feedback/quakesounds/quadkill",
	"sound/feedback/quakesounds/pentacrush",
	"sound/feedback/quakesounds/obliterated",
	"sound/feedback/quakesounds/epic",
	"sound/feedback/quakesounds/ludicrousgibs",
	"sound/feedback/quakesounds/holyshit",
	"sound/feedback/quakesounds/killstreak",
	"sound/feedback/quakesounds/rampage",
	"sound/feedback/quakesounds/domination",
	"sound/feedback/quakesounds/unstoppable",
	"sound/feedback/quakesounds/godlike",
	"sound/feedback/quakesounds/revenge"
};

}

void RegisterSounds()
{
	for ( int reward = QR_FIRST_BLOOD; reward < QR_NUM_REWARDS; ++reward )
	{
		cgs.media.quakeRewardSounds[ reward ] = trap_S_RegisterSound(
			quakeRewardSounds[ reward ], false );
	}
}

void Event( quakeReward_t reward )
{
	if ( !soundsEnabled.Get() || reward <= QR_NONE || reward >= QR_NUM_REWARDS )
	{
		return;
	}

	trap_S_StartLocalSound( cgs.media.quakeRewardSounds[ reward ], soundChannel_t::CHAN_ANNOUNCER );
}

} // namespace QuakeRewards
