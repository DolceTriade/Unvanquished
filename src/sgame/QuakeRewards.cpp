/*
===========================================================================

Copyright 2026 Unvanquished Developers

This file is part of Unvanquished.

Daemon is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Daemon is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Daemon.  If not, see <http://www.gnu.org/licenses/>.

===========================================================================
*/

#include "common/Common.h"
#include "sg_local.h"

namespace QuakeRewards {

namespace {

constexpr int MULTIKILL_WINDOW = 3000;
constexpr int GLOBAL_SPREE_THRESHOLD = 5;
constexpr int SHUTDOWN_THRESHOLD = 10;

enum class RewardKind
{
	None,
	FirstBlood,
	Humiliation,
	Domination,
	Flawless,
	DoubleKill,
	TripleKill,
	QuadKill,
	PentaCrush,
	Obliterated,
	Epic,
	LudicrousGibs,
	HolyShit,
	Killstreak,
	Rampage,
	DominationSpree,
	Unstoppable,
	Godlike,
	EpicSpree,
	Shutdown
};

struct Reward
{
	RewardKind kind = RewardKind::None;
	int soundIndex = 0;
	const char *label = nullptr;
	bool globalText = false;
	bool negative = false;
};

std::string RewardPath( const char *name )
{
	return Str::Format( "sound/feedback/quakesounds/%s", name );
}

struct RewardSoundSet
{
	int firstBlood;
	int humiliation;
	int domination;
	int flawless;
	int doublekill;
	int triplekill;
	int quadkill;
	int pentacrush;
	int obliterated;
	int epic;
	int ludicrousgibs;
	int holyshit;
	int killstreak;
	int rampage;
	int dominationSpree;
	int unstoppable;
	int godlike;
	int epicSpree;
	int shutdown;
} sounds;

int RegisterRewardSound( const char *name )
{
	return G_SoundIndex( RewardPath( name ).c_str() );
}

gentity_t *GetEffectiveAttackerEntity( gentity_t *source )
{
	if ( !source || source == &g_entities[ ENTITYNUM_WORLD ] )
	{
		return nullptr;
	}

	if ( source->client )
	{
		return source;
	}

	if ( source->parent && source->parent->client )
	{
		return source->parent;
	}

	return nullptr;
}

bool IsGrangerClass( class_t pcl )
{
	return pcl == PCL_ALIEN_BUILDER0 || pcl == PCL_ALIEN_BUILDER0_UPG;
}

bool IsHumiliationKill( gentity_t *attacker, meansOfDeath_t meansOfDeath )
{
	if ( !attacker || !attacker->client )
	{
		return false;
	}

	class_t attackerClass = static_cast<class_t>( attacker->client->ps.stats[ STAT_CLASS ] );

	if ( meansOfDeath == MOD_BLASTER )
	{
		return G_Team( attacker ) == TEAM_HUMANS;
	}

	return meansOfDeath == MOD_ABUILDER_CLAW && IsGrangerClass( attackerClass );
}

Reward MakeReward( RewardKind kind, int soundIndex, const char *label, bool globalText = false, bool negative = false )
{
	return { kind, soundIndex, label, globalText, negative };
}

Reward MultiKillReward( int kills )
{
	switch ( kills )
	{
		case 2: return MakeReward( RewardKind::DoubleKill, sounds.doublekill, "Double Kill" );
		case 3: return MakeReward( RewardKind::TripleKill, sounds.triplekill, "Triple Kill" );
		case 4: return MakeReward( RewardKind::QuadKill, sounds.quadkill, "Quad Kill", true );
		case 5: return MakeReward( RewardKind::PentaCrush, sounds.pentacrush, "Penta-crush", true );
		case 6: return MakeReward( RewardKind::Obliterated, sounds.obliterated, "Obliterated", true );
		case 7: return MakeReward( RewardKind::Epic, sounds.epic, "Epic", true );
		case 8: return MakeReward( RewardKind::LudicrousGibs, sounds.ludicrousgibs, "Ludicrous Gibs", true );
		default: return kills >= 9 ? MakeReward( RewardKind::HolyShit, sounds.holyshit, "Holy Shit", true ) : Reward{};
	}
}

Reward SpreeReward( int kills )
{
	switch ( kills )
	{
		case 5: return MakeReward( RewardKind::Killstreak, sounds.killstreak, "Killstreak", kills >= GLOBAL_SPREE_THRESHOLD );
		case 10: return MakeReward( RewardKind::Rampage, sounds.rampage, "Rampage", kills >= GLOBAL_SPREE_THRESHOLD );
		case 15: return MakeReward( RewardKind::DominationSpree, sounds.dominationSpree, "Domination", kills >= GLOBAL_SPREE_THRESHOLD );
		case 20: return MakeReward( RewardKind::Unstoppable, sounds.unstoppable, "Unstoppable", kills >= GLOBAL_SPREE_THRESHOLD );
		case 25: return MakeReward( RewardKind::Godlike, sounds.godlike, "Godlike", kills >= GLOBAL_SPREE_THRESHOLD );
		default: return kills >= 30 ? MakeReward( RewardKind::EpicSpree, sounds.epicSpree, "Epic", true ) : Reward{};
	}
}

Reward ShutdownReward()
{
	return MakeReward( RewardKind::Shutdown, sounds.shutdown, "Revenge", false, true );
}

void PlayReward( gentity_t *clientEnt, int soundIndex )
{
	if ( !clientEnt || !clientEnt->client || !soundIndex )
	{
		return;
	}

	gentity_t *event = G_NewTempEntity( VEC2GLM( clientEnt->client->ps.origin ), EV_GLOBAL_SOUND );
	event->s.eventParm = soundIndex;
	event->r.svFlags = SVF_SINGLECLIENT;
	event->r.singleClient = clientEnt->num();
}

void PlayReward( gentity_t *firstClientEnt, gentity_t *secondClientEnt, int soundIndex )
{
	PlayReward( firstClientEnt, soundIndex );

	if ( secondClientEnt && secondClientEnt != firstClientEnt )
	{
		PlayReward( secondClientEnt, soundIndex );
	}
}

void BroadcastRewardText( const std::string& message )
{
	trap_SendServerCommand( -1, va( "print %s", Quote( message.c_str() ) ) );
}

void ResetClientRewardState( gclient_t *client )
{
	if ( !client )
	{
		return;
	}

	client->pers.quakeRewards.spreeCount = 0;
	client->pers.quakeRewards.multikillCount = 0;
	client->pers.quakeRewards.lastKillTime = 0;
	client->pers.quakeRewards.lastVictimClientNum = ENTITYNUM_NONE;
}

} // namespace

void Init()
{
	level.quakeFirstBloodClaimed = false;

	sounds.firstBlood = RegisterRewardSound( "firstblood" );
	sounds.humiliation = RegisterRewardSound( "humiliated" );
	sounds.domination = RegisterRewardSound( "domination" );
	sounds.flawless = RegisterRewardSound( "flawless" );
	sounds.doublekill = RegisterRewardSound( "doublekill" );
	sounds.triplekill = RegisterRewardSound( "triplekill" );
	sounds.quadkill = RegisterRewardSound( "quadkill" );
	sounds.pentacrush = RegisterRewardSound( "pentacrush" );
	sounds.obliterated = RegisterRewardSound( "obliterated" );
	sounds.epic = RegisterRewardSound( "epic" );
	sounds.ludicrousgibs = RegisterRewardSound( "ludicrousgibs" );
	sounds.holyshit = RegisterRewardSound( "holyshit" );
	sounds.killstreak = RegisterRewardSound( "killstreak" );
	sounds.rampage = RegisterRewardSound( "rampage" );
	sounds.dominationSpree = RegisterRewardSound( "domination" );
	sounds.unstoppable = RegisterRewardSound( "unstoppable" );
	sounds.godlike = RegisterRewardSound( "godlike" );
	sounds.epicSpree = RegisterRewardSound( "epic" );
	sounds.shutdown = RegisterRewardSound( "revenge" );

	for ( int i = 0; i < level.maxclients; ++i )
	{
		ResetClientRewardState( &level.clients[ i ] );
	}
}

void NotifyPlayerDeath( gentity_t *victim, gentity_t *source, Util::optional<glm::vec3> /*location*/,
                        int /*flags*/, meansOfDeath_t meansOfDeath )
{
	if ( !victim || !victim->client )
	{
		return;
	}

	gentity_t *attacker = GetEffectiveAttackerEntity( source );
	int victimSpreeCount = victim->client->pers.quakeRewards.spreeCount;

	ResetClientRewardState( victim->client );

	if ( meansOfDeath == MOD_SUICIDE )
	{
		PlayReward( victim, sounds.flawless );
		return;
	}

	if ( !attacker || !attacker->client )
	{
		return;
	}

	if ( attacker == victim )
	{
		PlayReward( victim, sounds.flawless );
		return;
	}

	if ( G_OnSameTeam( attacker, victim ) )
	{
		PlayReward( attacker, victim, sounds.domination );
		return;
	}

	gclient_t *attackerClient = attacker->client;
	auto &rewardState = attackerClient->pers.quakeRewards;
	bool firstBlood = !level.quakeFirstBloodClaimed;
	level.quakeFirstBloodClaimed = true;

	if ( rewardState.lastKillTime > 0 && level.time - rewardState.lastKillTime <= MULTIKILL_WINDOW )
	{
		++rewardState.multikillCount;
	}
	else
	{
		rewardState.multikillCount = 1;
	}

	++rewardState.spreeCount;
	rewardState.lastKillTime = level.time;
	rewardState.lastVictimClientNum = victim->num();

	Reward reward;

	if ( IsHumiliationKill( attacker, meansOfDeath ) )
	{
		reward = MakeReward( RewardKind::Humiliation, sounds.humiliation, "Humiliated", false, true );
	}
	else if ( firstBlood )
	{
		reward = MakeReward( RewardKind::FirstBlood, sounds.firstBlood, "First Blood", true );
	}
	else
	{
		reward = MultiKillReward( rewardState.multikillCount );

		if ( reward.kind == RewardKind::None )
		{
			reward = SpreeReward( rewardState.spreeCount );
		}
	}

	if ( reward.negative )
	{
		PlayReward( attacker, victim, reward.soundIndex );
	}
	else
	{
		PlayReward( attacker, reward.soundIndex );
	}

	if ( reward.globalText )
	{
		BroadcastRewardText( Str::Format( "%s^*: %s!", attackerClient->pers.netname, reward.label ) );
	}

	if ( victimSpreeCount >= SHUTDOWN_THRESHOLD )
	{
		Reward victimSpreeReward = SpreeReward( victimSpreeCount );
		Reward shutdown = ShutdownReward();

		PlayReward( attacker, victim, shutdown.soundIndex );

		if ( victimSpreeReward.label )
		{
			BroadcastRewardText( Str::Format( "%s^* ended %s^*'s %s!",
			                                  attackerClient->pers.netname,
			                                  victim->client->pers.netname,
			                                  victimSpreeReward.label ) );
		}
	}
}

} // namespace QuakeRewards
