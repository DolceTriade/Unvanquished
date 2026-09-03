/*
===========================================================================

Unvanquished GPL Source Code
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of the Unvanquished GPL Source Code (Unvanquished Source Code).

Unvanquished is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Unvanquished is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Unvanquished; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

===========================================================================
*/

#include "common/Common.h"
#include "lua/BotBehavior.h"
#include "sg_bot_parse.h"
#include "sg_bot_trace.h"
#include "sg_bot_util.h"
#include "Entities.h"

Cvar::Modified<Cvar::Cvar<int>> g_bot_defaultFill("g_bot_defaultFill", "fills both teams with that number of bots at start of game", Cvar::NONE, 0);
static Cvar::Range<Cvar::Cvar<int>> generateNeededMesh(
	"g_bot_navgen_onDemand",
	"automatically generate navmeshes when a bot is added (1 = in background, -1 = blocking)",
	Cvar::NONE, 1, -1, 1);
static Cvar::Cvar<int> traceClient(
	"g_bot_traceClient", "show running BT node for this client num", Cvar::NONE, -1);

static botMemory_t g_botMind[MAX_CLIENTS];
static AITreeList_t treeList;

AIBehaviorTree_t *BotBehaviorTree( Str::StringRef behavior )
{
	if ( Str::IsSuffix( ".lua", behavior ) || Str::IsSuffix( ".bt", behavior ) )
	{
		return ReadBehaviorTree( behavior.c_str(), &treeList );
	}

	if ( AIBehaviorTree_t* tree = ReadBehaviorTree( Str::Format( "%s.lua", behavior ).c_str(), &treeList ) )
	{
		return tree;
	}

	return ReadBehaviorTree( behavior.c_str(), &treeList );
}

static bool NodeReferencesBehavior( const AIGenericNode_t *node, const AIBehaviorTree_t *target )
{
	if ( !node )
	{
		return false;
	}

	switch ( node->type )
	{
		case SELECTOR_NODE:
		{
			const AINodeList_t *list = reinterpret_cast<const AINodeList_t *>( node );
			for ( int i = 0; i < list->numNodes; ++i )
			{
				if ( NodeReferencesBehavior( list->list[ i ], target ) )
				{
					return true;
				}
			}
			return false;
		}

		case CONDITION_NODE:
			return NodeReferencesBehavior(
				reinterpret_cast<const AIConditionNode_t *>( node )->child, target );

		case DECORATOR_NODE:
			return NodeReferencesBehavior(
				reinterpret_cast<const AIDecoratorNode_t *>( node )->child, target );

		case BEHAVIOR_NODE:
			return node == reinterpret_cast<const AIGenericNode_t *>( target );

		case ACTION_NODE:
		case SPAWN_NODE:
		case LUA_BEHAVIOR_NODE:
		case LUA_ACTION_NODE:
			return false;
	}

	return false;
}

bool G_BotUnloadBehavior( Str::StringRef behavior, std::string *reason )
{
	auto it = std::find_if( treeList.begin(), treeList.end(),
	                        [behavior]( AIBehaviorTree_t *tree )
	                        {
		                        return !Q_stricmp( tree->name, behavior.c_str() );
	                        } );
	if ( it == treeList.end() )
	{
		if ( reason )
		{
			*reason = Str::Format( "behavior '%s' is not currently loaded", behavior );
		}
		return false;
	}

	AIBehaviorTree_t *tree = *it;

	for ( int i = 0; i < MAX_CLIENTS; ++i )
	{
		gentity_t *ent = &g_entities[ i ];
		if ( !ent->client || ent->client->pers.connected == CON_DISCONNECTED || !ent->client->pers.isBot || !ent->botMind )
		{
			continue;
		}

		if ( ent->botMind->behaviorTree == tree )
		{
			if ( reason )
			{
				*reason = Str::Format( "behavior '%s' is still in use by bot '%s'",
				                       behavior, ent->client->pers.netname );
			}
			return false;
		}
	}

	for ( AIBehaviorTree_t *other : treeList )
	{
		if ( other == tree )
		{
			continue;
		}

		if ( NodeReferencesBehavior( other->root, tree ) )
		{
			if ( reason )
			{
				*reason = Str::Format( "behavior '%s' is still included by behavior '%s'",
				                       behavior, other->name );
			}
			return false;
		}

		if ( other->classSelectionTree != other->root
		     && NodeReferencesBehavior( other->classSelectionTree, tree ) )
		{
			if ( reason )
			{
				*reason = Str::Format( "behavior '%s' is still included by behavior '%s'",
				                       behavior, other->name );
			}
			return false;
		}
	}

	treeList.erase( it );
	FreeBehaviorTree( tree );
	return true;
}

/*
=======================
Bot management functions
=======================
*/
struct nameInfo_t {
	std::string name;
	bool inUse;
};
static BoundedVector<nameInfo_t, MAX_CLIENTS> botNames[NUM_TEAMS];

static void G_BotListTeamNames( gentity_t *ent, const char *heading, team_t team, const char *marker )
{
	if ( !botNames[team].empty() )
	{
		ADMP( heading );
		ADMBP_begin();

		for ( nameInfo_t &nameInfo : botNames[team] )
		{
			ADMBP( va( "  %s^* %s", nameInfo.inUse ? marker : " ", nameInfo.name.c_str() ) );
		}

		ADMBP_end();
	}
}

void G_BotListNames( gentity_t *ent )
{
	G_BotListTeamNames( ent, QQ( N_( "^3Alien bot names:" ) ), TEAM_ALIENS, "^1*" );
	G_BotListTeamNames( ent, QQ( N_( "^3Human bot names:" ) ), TEAM_HUMANS, "^5*" );
}

bool G_BotClearNames()
{
	for ( auto &teamsBotNames : botNames )
	{
		for ( nameInfo_t &nameInfo : teamsBotNames )
		{
			if ( nameInfo.inUse )
			{
				return false;
			}
		}
	}

	for ( auto &teamsBotNames : botNames )
	{
		teamsBotNames.clear();
	}

	return true;
}

int G_BotAddNames( team_t team, int arg, int last )
{
	int  added = 0;
	char name[MAX_NAME_LENGTH];

	while ( arg < last )
	{
		trap_Argv( arg++, name, sizeof( name ) );

		// name already in the list? (quick check, including colours & invalid)
		for ( int t = 1; t < NUM_TEAMS; ++t )
		{
			for ( nameInfo_t &nameInfo : botNames[t] )
			{
				if ( !Q_stricmp( nameInfo.name.c_str(), name ) )
				{
					goto next;
				}
			}
		}

		botNames[team].append({ name, /*.inUse = */ false });
		++added;

		next:
		;
	}

	return added;
}

static const char *G_BotSelectName( team_t team )
{
	unsigned int count = 0;
	for ( nameInfo_t &nameInfo : botNames[team] )
	{
		if ( !nameInfo.inUse )
		{
			count++;
		}
	}

	if (count == 0)
	{
		return nullptr;
	}

	unsigned int choice = BG_randrange( count );

	unsigned int index = 0;
	for ( nameInfo_t &nameInfo : botNames[team] )
	{
		if ( nameInfo.inUse )
		{
			continue;
		}

		if (index++ == choice)
		{
			return nameInfo.name.c_str();
		}
	}

	ASSERT_UNREACHABLE();
}

static void G_BotNameUsed( team_t team, const char *name, bool inUse )
{
	for ( nameInfo_t &nameInfo : botNames[team] )
	{
		if ( !Q_stricmp( name, nameInfo.name.c_str() ) )
		{
			nameInfo.inUse = inUse;
			return;
		}
	}
}

void G_BotSetSkill( int clientNum, int skill )
{
	gentity_t *bot = &g_entities[clientNum];

	if ( !bot || !bot->client || bot->client->pers.connected == CON_DISCONNECTED || !bot->client->pers.isBot )
	{
		return;
	}

	BotSetSkillLevel( bot, skill );
}

void G_BotChangeBehavior( int clientNum, Str::StringRef behavior )
{
	gentity_t *bot = &g_entities[clientNum];
	ASSERT( bot->client->pers.isBot && bot->botMind );

	G_BotSetBehavior( bot->botMind, static_cast<team_t>( bot->client->pers.team ), behavior );
}

static std::string G_BotDefaultBehavior( team_t team )
{
	switch ( team )
	{
		case TEAM_HUMANS:
			return g_bot_defaultBehaviorHuman.Get();

		case TEAM_ALIENS:
			return g_bot_defaultBehaviorAlien.Get();

		default:
			return BOT_DEFAULT_BEHAVIOR;
	}
}

bool G_BotSetBehavior( botMemory_t *botMind, team_t team, Str::StringRef behavior )
{
	G_Bot_ResetBehaviorState( *botMind );
	botMind->blackboardTransient = 0;
	botMind->myTimer = level.time;
	botMind->buildCooldownUntil = 0;

	botMind->behaviorTree = BotBehaviorTree( behavior );

	if ( !botMind->behaviorTree )
	{
		Log::Warn( "Problem when loading behavior tree %s, trying default", behavior );
		const std::string behaviorString = G_BotDefaultBehavior( team );
		botMind->behaviorTree = BotBehaviorTree( behaviorString );

		if ( !botMind->behaviorTree )
		{
			Log::Warn( "Problem when loading default behavior tree" );
			return false;
		}
	}
	return true;
}

static void G_InitBotMind( int clientNum )
{
	gentity_t *self = &g_entities[ clientNum ];
	botMemory_t *botMind = self->botMind = &g_botMind[ clientNum ];
	G_Bot_ResetBehaviorState( *botMind );
	ResetStruct( *botMind );
}

bool G_BotSetDefaults( int clientNum, team_t team, Str::StringRef behavior )
{
	gentity_t *self = &g_entities[ clientNum ];
	G_InitBotMind( clientNum );
	botMemory_t *botMind = self->botMind;

	if ( !G_BotSetBehavior( botMind, team, behavior ) )
	{
		return false;
	}

	self->pain = BotPain;

	return true;
}

bool G_BotAdd( const char *name, team_t team, int skill, const char *behavior, bool filler )
{
	char userinfo[MAX_INFO_STRING];
	const char* s = 0;
	bool autoname = false;

	ASSERT( navMeshLoaded == navMeshStatus_t::LOADED || navMeshLoaded == navMeshStatus_t::GENERATING );

	// find what clientNum to use for bot
	int clientNum = trap_BotAllocateClient();

	if ( clientNum < 0 )
	{
		Log::Warn( "no more slots for bot" );
		return false;
	}
	gentity_t *bot = &g_entities[ clientNum ];

	if ( !Q_stricmp( name, BOT_NAME_FROM_LIST ) )
	{
		name = G_BotSelectName( team );
		autoname = name != nullptr;
	}

	// ClientBotConnect calls CalculateRanks, which expects connected bots to have botMind.
	G_InitBotMind( clientNum );
	bool okay = true;

	// register user information
	userinfo[0] = '\0';
	Info_SetValueForKey( userinfo, "cg_unlagged", "0", false ); // bots do not lag
	Info_SetValueForKey( userinfo, "name", name ? name : "", false ); // allow defaulting
	Info_SetValueForKey( userinfo, "rate", "25000", false );
	Info_SetValueForKey( userinfo, "snaps", "20", false );
	if ( autoname )
	{
		Info_SetValueForKey( userinfo, "autoname", name, false );
	}

	trap_SetUserinfo( clientNum, userinfo );

	// have it connect to the game as a normal client
	if ( ( s = ClientBotConnect( clientNum, true ) ) )
	{
		// won't let us join
		Log::Warn( s );
		okay = false;
	}

	if ( !okay )
	{
		G_BotDel( clientNum );
		return false;
	}

	if ( autoname )
	{
		G_BotNameUsed( team, name, true );
	}

	ClientBegin( clientNum );
	okay = G_BotSetDefaults( clientNum, team, behavior );
	if ( !okay )
	{
		G_BotDel( clientNum );
		return false;
	}
	bot->pain = BotPain; // ClientBegin resets the pain function
	level.clients[clientNum].pers.isFillerBot = filler;
	G_ChangeTeam( bot, team );
	BotSetSkillLevel( bot, skill );
	return true;
}

void G_BotDel( int clientNum )
{
	gentity_t *bot = &g_entities[clientNum];
	char userinfo[MAX_INFO_STRING];
	const char *autoname;

	if ( !g_clients[ clientNum ].pers.isBot )
	{
		Log::Warn( "'^7%s^*' is not a bot", bot->client->pers.netname );
		return;
	}

	trap_GetUserinfo( clientNum, userinfo, sizeof( userinfo ) );

	autoname = Info_ValueForKey( userinfo, "autoname" );
	if ( autoname && *autoname )
	{
		G_BotNameUsed( G_Team( bot ), autoname, false );
	}

	trap_SendServerCommand( -1, va( "print_tr %s %s", QQ( N_( "$1$^* disconnected" ) ),
					Quote( bot->client->pers.netname ) ) );
	trap_DropClient( clientNum, "disconnected" );
}

void G_BotDelAllBots()
{
	for ( int i = 0; i < MAX_CLIENTS; i++ )
	{
		if ( level.clients[ i ].pers.connected != CON_DISCONNECTED && level.clients[ i ].pers.isBot )
		{
			G_BotDel( i );
		}
	}

	for ( auto &teamsBotNames : botNames )
	{
		for ( nameInfo_t &nameInfo : teamsBotNames )
		{
			nameInfo.inUse = false;
		}
	}

	g_bot_defaultFill.GetModifiedValue(); // clear modified flag
	for ( auto &team : level.team )
	{
		team.botFillTeamSize = 0;
	}
}

static void ShowRunningNode( gentity_t *self, AINodeStatus_t status )
{
	const char *name = self->client->pers.netname;
	botTraceDescriptor_t descriptor = G_BotBuildTraceDescriptor( self, status );

	switch ( descriptor.stateKind )
	{
		case botTraceStateKind_t::ROOT_FAILURE:
			Log::defaultLogger.WithoutSuppression().Notice( "%s^* root tree exited with STATUS_FAILURE", name );
			break;

		case botTraceStateKind_t::ROOT_SUCCESS:
			Log::defaultLogger.WithoutSuppression().Notice( "%s^* root tree exited with STATUS_SUCCESS", name );
			break;

		case botTraceStateKind_t::RUNNING:
			Log::defaultLogger.WithoutSuppression().Notice(
				"%s^* running at %s:%d, action %s",
				name, descriptor.sourceName, descriptor.sourceLine, descriptor.actionName );
			break;

		case botTraceStateKind_t::NONE:
			break;
	}
}

/*
=======================
Bot Thinks
=======================
*/

static Cvar::Cvar<float> g_bot_jetpackTimeout("g_bot_jetpackTimeout", "time in milliseconds until a jetpack flight is aborted", Cvar::NONE, 10000);

void G_BotThink( gentity_t *self )
{
	char buf[MAX_STRING_CHARS];
	usercmd_t *botCmdBuffer;

	self->botMind->cmdBuffer = self->client->pers.cmd;
	botCmdBuffer = &self->botMind->cmdBuffer;

	//reset command buffer
	usercmdClearButtons( botCmdBuffer->buttons );

	// for nudges, e.g. spawn blocking
	glm::vec3 nudge = { 0, 0, 0 };
	if ( botCmdBuffer->doubleTap != dtType_t::DT_NONE )
	{
		nudge = { botCmdBuffer->forwardmove, botCmdBuffer->rightmove, botCmdBuffer->upmove };
	}

	botCmdBuffer->forwardmove = 0;
	botCmdBuffer->rightmove = 0;
	botCmdBuffer->upmove = 0;
	botCmdBuffer->doubleTap = dtType_t::DT_NONE;
	botCmdBuffer->flags = 0;

	//acknowledge recieved server commands
	//MUST be done
	while ( trap_BotGetServerCommand( self->num(), buf, sizeof( buf ) ) );

	BotSearchForEnemy( self );

	// Populate transient caches
	BotFindClosestBuildings( self );
	BotFindDamagedFriendlyStructure( self );
	BotCalculateStuckTime( self );

	//infinite funds cvar
	if ( g_bot_infiniteFunds.Get() )
	{
		G_AddCreditToClient( self->client, HUMAN_MAX_CREDITS, true );
	}

	G_BotOverloadThink( self );

	//reset the user specified client number if the client disconnected
	if ( self->botMind->userSpecifiedClientNum )
	{
		int userSpecifiedClientNum = *self->botMind->userSpecifiedClientNum;
		gentity_t *ent = &g_entities[ userSpecifiedClientNum ];
		if ( !ent->client || ent->client->pers.connected == CON_DISCONNECTED )
		{
			self->botMind->userSpecifiedClientNum = Util::nullopt;
		}
	}

	self->botMind->blackboardTransient = 0;

	if ( !self->botMind->behaviorTree )
	{
		Log::Warn( "NULL behavior tree" );
		return;
	}

	// always update the path corridor
	if ( self->botMind->goal.isValid() )
	{
		botRouteTarget_t routeTarget;
		BotTargetToRouteTarget( self, self->botMind->goal, &routeTarget );
		G_BotUpdatePath( self->s.number, &routeTarget, &self->botMind->m_nav );
	}

	self->botMind->willSprint( false ); //let the BT decide that
	AINodeStatus_t status =
		self->botMind->behaviorTree->run( self, ( AIGenericNode_t * ) self->botMind->behaviorTree );

	self->botMind->lastThink = level.time;

	if ( traceClient.Get() == self->num() && self->botMind->behaviorTree->type != LUA_BEHAVIOR_NODE )
	{
		ShowRunningNode( self, status );
	}

	G_BotTraceTransition( self, status );

	// if we have a jetpack and are falling too fast: fire it
	if ( G_Team( self ) == TEAM_HUMANS && BG_InventoryContainsUpgrade( UP_JETPACK, self->client->ps.stats ) )
	{
		glm::vec3 ownVelocity = VEC2GLM( self->client->ps.velocity );
		if ( ownVelocity.z < -300 )
		{
			self->botMind->cmdBuffer.upmove = 127;
		}
		// clear jetpack state after some time
		switch ( self->botMind->jetpackState )
		{
		case BOT_JETPACK_NAVCON_WAITING:
		case BOT_JETPACK_NAVCON_FLYING:
		case BOT_JETPACK_NAVCON_LANDING:
			if ( level.time > self->botMind->lastNavconTime + g_bot_jetpackTimeout.Get() )
			{
				self->botMind->jetpackState = BOT_JETPACK_NONE;
			}
			break;
		default:
			break;
		}
	}

	// if we were nudged...
	VectorAdd( self->client->ps.velocity, nudge, self->client->ps.velocity );

	// for real clients this is read off the network as a 16-bit value
	for ( int &angle : self->botMind->cmdBuffer.angles )
	{
		angle &= 65535;
	}

	// ensure we really want to sprint or not
	self->client->pers.cmd = self->botMind->cmdBuffer;
	self->botMind->doSprint(
			BG_Class( self->client->ps.stats[ STAT_CLASS ] )->staminaJumpCost,
			self->client->ps.stats[ STAT_STAMINA ],
			self->client->pers.cmd );
}

void G_BotSpectatorThink( gentity_t *self )
{
	char buf[MAX_STRING_CHARS];

	//acknowledge recieved console messages
	//MUST be done
	while ( trap_BotGetServerCommand( self->num(), buf, sizeof( buf ) ) );

	if ( navMeshLoaded == navMeshStatus_t::GENERATING )
	{
		return;
	}

	self->botMind->spawnTime = level.time;
	self->botMind->myTimer = level.time;
	self->botMind->buildCooldownUntil = 0;

	if ( self->client->ps.pm_flags & PMF_QUEUED )
	{
		//we're queued to spawn, all good
		//check for humans in the spawn que
		{
			spawnQueue_t *sq;
			if ( self->client->pers.team != TEAM_NONE )
			{
				sq = &level.team[ self->client->pers.team ].spawnQueue;
			}
			else
			{
				sq = nullptr;
			}

			if ( sq && PlayersBehindBotInSpawnQueue( self ) )
			{
				G_RemoveFromSpawnQueue( sq, self->s.number );
				G_PushSpawnQueue( sq, self->s.number );
			}
		}
		return;
	}

	G_Bot_ResetBehaviorState( *self->botMind );

	// Reset non-time-dependent alive state
	self->botMind->lastThink = -999999;
	self->botMind->stuckTime = 0;
	self->botMind->stuckPosition = {1.0e12f, 1.0e12f, 1.0e12f};
	self->botMind->futureAimTime = 0;
	self->botMind->futureAimTimeInterval = 0;
	BotResetEnemyQueue( &self->botMind->enemyQueue );
	self->botMind->enemyLastSeen = -999999;
	self->botMind->abandonedEnemy = nullptr;
	self->botMind->abandonedEnemyUntil = 0;
	self->botMind->exhausted = false;
	self->botMind->buildCooldownUntil = 0;

	//FIXME: duplicate of sg_cmds.cpp:883 function "void Cmd_Team_f( gentity_t * )"
	if ( g_doWarmup.Get() && ( ( level.warmupTime - level.time ) / 1000 ) > 0 )
	{
		return;
	}

	if ( self->client->sess.restartTeam == TEAM_NONE )
	{
		int teamnum = self->client->pers.team;

		// I think all that decision taking about birth... should go in BT.
		if ( teamnum == TEAM_HUMANS )
		{
			// TODO: use wp->canBuyNow() from sg_bot_util
			weapon_t weapon = WP_NONE;
			if ( g_bot_rifle.Get() )
			{
				weapon = WP_MACHINEGUN;
			}
			else if ( g_bot_ckit.Get() )
			{
				weapon = WP_HBUILD;
			}

			G_ScheduleSpawn( self->client, PCL_HUMAN_NAKED, weapon );
		}
		else if ( teamnum == TEAM_ALIENS )
		{
			G_ScheduleSpawn( self->client, PCL_ALIEN_LEVEL0 );
		}
	}
}

void G_BotIntermissionThink( gclient_t *client )
{
	client->readyToExit = true;
}

void G_BotSelectSpawnClass( gentity_t *self )
{
	if ( self->botMind->behaviorTree && self->botMind->behaviorTree->classSelectionTree )
	{
		BotEvaluateNode( self, self->botMind->behaviorTree->classSelectionTree );
	}
}

// Initialization happens whenever someone first tries to add a bot.
// Assuming the meshes already exist, this incurs some delay (a few tenths of a second), but on
// servers bots are normally added at the beginning of the round so it shouldn't be noticeable.
//
// If the mesh does not already exist and g_bot_navgen_onDemand is 1, the function returns true
// and bots can join, but they don't spawn until the navmesh is finished.
//
// If the mesh does not exist and g_bot_navgen_onDemand is -1, the function will block and the
// server will freeze until the navmesh is done (usually tens of seconds).
bool G_BotInit()
{
	switch ( navMeshLoaded )
	{
	case navMeshStatus_t::GENERATING:
	case navMeshStatus_t::LOADED:
		return true;
	case navMeshStatus_t::LOAD_FAILED:
		Log::Warn( "Navmesh initialization previously failed, doing nothing" );
		return false;
	case navMeshStatus_t::UNINITIALIZED:
		break;
	}

	G_BotNavInit( generateNeededMesh.Get() );

	if ( navMeshLoaded != navMeshStatus_t::LOADED && navMeshLoaded != navMeshStatus_t::GENERATING )
	{
		Log::Notice( "Failed to load navmeshes" );
		return false;
	}
	return true;
}

void G_BotCleanup()
{
	G_BotDelAllBots();

	G_BotClearNames();

	FreeTreeList( &treeList );
	G_BotNavCleanup();
}

// add or remove bots to match team size targets set by 'bot fill' command
static void G_BotCheckDefaultFill()
{
	Util::optional<int> fillCount = g_bot_defaultFill.GetModifiedValue();
	if ( fillCount ) // if modified
	{
		int adjustedCount = Math::Clamp( *fillCount, 0, MAX_CLIENTS );
		// init bots if they aren't already and if we need to
		if ( adjustedCount != 0 && !G_BotInit() )
		{
			Log::Warn( "Navigation mesh files unavailable for this map" );
			return;
		}

		for ( int team = TEAM_NONE + 1; team < NUM_TEAMS; ++team )
		{
			level.team[team].botFillTeamSize = adjustedCount;
			level.team[team].botFillSkillLevel = 0; // default
		}
	}
}

void G_BotFill(bool immediately)
{
	static int nextCheck = 0;
	if (!immediately && level.time < nextCheck) {
		return;  // don't check every frame to prevent warning spam
	}

	if ( !immediately && level.inClient && g_clients[ 0 ].pers.connected < CON_CONNECTED )
	{
		// In case cg_navgenOnLoad is enabled, give that a chance to finish so
		// that we don't have two dueling navgens.
		return;
	}

	G_BotCheckDefaultFill();

	nextCheck = level.time + 2000;
	//FIXME this function can actually be called before bots had time to connect
	//  resulting in filling too many bots.
	if ( level.matchTime < 2000 )
	{
		return;
	}

	struct fill_t
	{
		std::vector<int> current; // list of filler bots
		int target; // if <0, too many bots, if >0, not enough
	} fillers[ NUM_TEAMS ] = {};
	int missingFillers = 0;

	for (int client = 0; client < MAX_CLIENTS; client++)
	{
		auto& pers = level.clients[client].pers;
		if ( pers.connected == CON_CONNECTED && pers.isFillerBot )
		{
			fillers[ pers.team ].current.push_back( client );
		}
	}

	for ( team_t team : {TEAM_ALIENS, TEAM_HUMANS} )
	{
		auto& fill = fillers[team];
		fill.target = level.team[team].botFillTeamSize - level.team[team].numClients;
		// remove excedent
		while ( fill.target < 0 && fill.current.size() > 0 )
		{
			G_BotDel( fill.current.back() );
			fill.current.pop_back();
			fill.target++;
		}
		// remember how much bots are missing
		if ( fill.target > 0 )
		{
			missingFillers += fill.target;
		}
	}

	while ( missingFillers )
	{
		for ( team_t team : {TEAM_ALIENS, TEAM_HUMANS} )
		{
			if ( fillers[ team ].target > 0 )
			{
				const std::string behaviorString = G_BotDefaultBehavior( team );
				fillers[ team ].target--;
				if ( !G_BotAdd( BOT_NAME_FROM_LIST, team, level.team[team].botFillSkillLevel, behaviorString.c_str(), true ) )
				{
					//TODO modify the "/bot fill" number so that further warnings will be ignored.
					//     Note that this means players disconnecting would possibly not be replaced by bot
					//     so this might be more a problem than an improvement.
					//TODO if number of clients in both teams is unevent, this is unfair, so kick one of other team.
					//     to do this, it would be needed to add the new client to fillers[team].current
					return;
				}
				--missingFillers;
			}
		}
	}
}

// declares an intent to sprint or not, should be used by BT functions
void botMemory_t::willSprint( bool enable )
{
	wantSprinting = enable;
}

// applies the sprint intent, should only be called after all directional
// or speed intents have been decided, that is, in G_BotThink(), *after*
// the BT was examined.
// This currently implements an hysteresis to prevent smart bot to be so
// exhausted that they can't jump over an obstacle.
// The hysteresis is meant to allow to recharge enough stamina so that a
// sprint can be useful, instead of constantly enabling sprint, which in
// practice result is the "moonwalk bug": AIs moving slower than if just
// walking, and stamina not recharging at all.
void botMemory_t::doSprint( int jumpCost, int stamina, usercmd_t& cmd )
{
	exhausted = exhausted || ( skillLevel >= 5 && stamina <= jumpCost + jumpCost / 10 );
	if ( !exhausted && wantSprinting )
	{
		usercmdPressButton( cmd.buttons, BTN_SPRINT );
	}
	else
	{
		usercmdReleaseButton( cmd.buttons, BTN_SPRINT );
	}

	exhausted = exhausted && stamina <= jumpCost * 2;
}

// TODO: also reset state stored in BT nodes
void G_Bot_ResetBehaviorState( botMemory_t &memory )
{
	Lua::ResetBotBehaviorState( memory );
	G_BotResetTraceCache( memory );
	memory.currentNode = nullptr;
	memory.runningNodes.clear();
	memory.goal.clear();
	memory.clearNav();
	memory.overloadTargetPurchase = -1;
	memory.overloadNextPurchaseTime = 0;
	memory.lastNavconTime = 0;
	memory.lastNavconDistance = 0;
	memory.hasOffmeshGoal = false;
}

// assumes bot is a bot, otherwise will crash.
static std::string BotGoalToString( gentity_t *bot )
{
	const botTarget_t& target = bot->botMind->goal;
	if ( !target.isValid() )
	{
		return "<invalid>";
	}

	if ( target.targetsValidEntity() )
	{
		return etos( target.getTargetedEntity() );
	}
	else if ( target.targetsCoordinates() )
	{
		return vtos( GLM4READ( target.getPos() ) );
	}

	return "<unknown goal>";
}

std::string G_BotToString( gentity_t *bot )
{
	ASSERT( bot->inuse && bot->client->pers.isBot );

	return Str::Format( "^*%s^*: %s [b=%s g=%s s=%d ss=\"%s\"]",
			bot->client->pers.netname,
			BG_TeamName( G_Team( bot ) ),
			bot->botMind->behaviorTree->name,
			BotGoalToString( bot ),
			bot->botMind->skillLevel,
			bot->botMind->skillSetExplaination );
}

int G_BotTraceClient()
{
	return traceClient.Get();
}
