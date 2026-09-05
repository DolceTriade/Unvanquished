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
#include "shared/bg_quakerewards.h"

namespace QuakeRewards {

namespace {

constexpr int MULTIKILL_WINDOW = 3000;
constexpr int GLOBAL_SPREE_THRESHOLD = 5;
constexpr int SHUTDOWN_THRESHOLD = 10;

struct Reward
{
	quakeReward_t kind = QR_NONE;
	const char *label = nullptr;
	bool globalText = false;
	bool negative = false;
};

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

bool IsHumiliationKill( gentity_t *attacker, meansOfDeath_t meansOfDeath )
{
	if ( !attacker || !attacker->client )
	{
		return false;
	}


	return meansOfDeath == MOD_ABUILDER_CLAW || meansOfDeath == MOD_SLOWBLOB || meansOfDeath == MOD_BLASTER;
}

Reward MakeReward( quakeReward_t kind, const char *label, bool globalText = false, bool negative = false )
{
	return { kind, label, globalText, negative };
}

Reward MultiKillReward( int kills )
{
	switch ( kills )
	{
		case 2: return MakeReward( QR_MULTI_KILL_2, "Double Kill" );
		case 3: return MakeReward( QR_MULTI_KILL_3, "Triple Kill" );
		case 4: return MakeReward( QR_MULTI_KILL_4, "Quad Kill", true );
		case 5: return MakeReward( QR_MULTI_KILL_5, "Penta-crush", true );
		case 6: return MakeReward( QR_MULTI_KILL_6, "Obliterated", true );
		case 7: return MakeReward( QR_MULTI_KILL_7, "Epic", true );
		case 8: return MakeReward( QR_MULTI_KILL_8, "Ludicrous Gibs", true );
		default: return kills >= 9 ? MakeReward( QR_MULTI_KILL_9_OR_MORE, "Holy Shit", true ) : Reward{};
	}
}

Reward SpreeReward( int kills )
{
	switch ( kills )
	{
		case 5: return MakeReward( QR_KILLSTREAK_5, "Killstreak", kills >= GLOBAL_SPREE_THRESHOLD );
		case 10: return MakeReward( QR_KILLSTREAK_10, "Rampage", kills >= GLOBAL_SPREE_THRESHOLD );
		case 15: return MakeReward( QR_KILLSTREAK_15, "Domination", kills >= GLOBAL_SPREE_THRESHOLD );
		case 20: return MakeReward( QR_KILLSTREAK_20, "Unstoppable", kills >= GLOBAL_SPREE_THRESHOLD );
		case 25: return MakeReward( QR_KILLSTREAK_25, "Godlike", kills >= GLOBAL_SPREE_THRESHOLD );
		default: return Reward{};
	}
}

Reward ShutdownReward()
{
	return MakeReward( QR_SHUTDOWN, "Revenge", false, true );
}

void PlayReward( gentity_t *clientEnt, quakeReward_t reward )
{
	if ( !clientEnt || !clientEnt->client || reward == QR_NONE )
	{
		return;
	}

	gentity_t *event = G_NewTempEntity( VEC2GLM( clientEnt->client->ps.origin ), EV_QUAKE_REWARD );
	event->s.eventParm = reward;
	event->r.svFlags = SVF_SINGLECLIENT;
	event->r.singleClient = clientEnt->num();
}

void PlayReward( gentity_t *firstClientEnt, gentity_t *secondClientEnt, quakeReward_t reward )
{
	PlayReward( firstClientEnt, reward );

	if ( secondClientEnt && secondClientEnt != firstClientEnt )
	{
		PlayReward( secondClientEnt, reward );
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
		PlayReward( victim, QR_SUICIDE );
		return;
	}

	if ( !attacker || !attacker->client )
	{
		return;
	}

	if ( attacker == victim )
	{
		PlayReward( victim, QR_SUICIDE );
		return;
	}

	if ( G_OnSameTeam( attacker, victim ) )
	{
		PlayReward( attacker, victim, QR_TEAM_KILL );
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
		reward = MakeReward( QR_HUMILIATION, "Humiliated", false, true );
	}
	else if ( firstBlood )
	{
		reward = MakeReward( QR_FIRST_BLOOD, "First Blood", true );
	}
	else
	{
		reward = MultiKillReward( rewardState.multikillCount );

		if ( reward.kind == QR_NONE )
		{
			reward = SpreeReward( rewardState.spreeCount );
		}
	}

	if ( reward.negative )
	{
		PlayReward( attacker, victim, reward.kind );
	}
	else
	{
		PlayReward( attacker, reward.kind );
	}

	if ( reward.globalText )
	{
		BroadcastRewardText( Str::Format( "%s^*: %s!", attackerClient->pers.netname, reward.label ) );
	}

	if ( victimSpreeCount >= SHUTDOWN_THRESHOLD )
	{
		Reward victimSpreeReward = SpreeReward( victimSpreeCount );
		Reward shutdown = ShutdownReward();

		PlayReward( attacker, victim, shutdown.kind );

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
