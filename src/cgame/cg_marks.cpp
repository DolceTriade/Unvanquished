/*
===========================================================================

Unvanquished GPL Source Code
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2000-2009 Darklegion Development

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

// cg_marks.c -- wall marks

#include "common/Common.h"
#include "cg_local.h"

/*
===================================================================

MARK POLYS

===================================================================
*/

markPoly_t cg_activeMarkPolys; // double linked list
markPoly_t *cg_freeMarkPolys; // single linked list
markPoly_t cg_markPolys[ MAX_MARK_POLYS ];

static void CG_ClearCreepMarkCache();

/*
===================
CG_InitMarkPolys

This is called at startup and for tournament restarts
===================
*/
void CG_InitMarkPolys()
{
	int i;

	memset( cg_markPolys, 0, sizeof( cg_markPolys ) );

	cg_activeMarkPolys.nextMark = &cg_activeMarkPolys;
	cg_activeMarkPolys.prevMark = &cg_activeMarkPolys;
	cg_freeMarkPolys = cg_markPolys;

	for ( i = 0; i < MAX_MARK_POLYS - 1; i++ )
	{
		cg_markPolys[ i ].nextMark = &cg_markPolys[ i + 1 ];
	}

	CG_ClearCreepMarkCache();
}

/*
==================
CG_FreeMarkPoly
==================
*/
static void CG_FreeMarkPoly( markPoly_t *le )
{
	if ( !le->prevMark )
	{
		Sys::Drop( "CG_FreeMarkPoly: not active" );
	}

	// remove from the doubly linked active list
	le->prevMark->nextMark = le->nextMark;
	le->nextMark->prevMark = le->prevMark;

	// the free list is only singly linked
	le->nextMark = cg_freeMarkPolys;
	cg_freeMarkPolys = le;
}

/*
===================
CG_AllocMark

Will always succeed, even if it requires freeing an old active mark
===================
*/
static markPoly_t *CG_AllocMarkPoly()
{
	markPoly_t *le;
	int        time;

	if ( !cg_freeMarkPolys )
	{
		// no free entities, so free the one at the end of the chain
		// remove the oldest active entity
		time = cg_activeMarkPolys.prevMark->time;

		while ( cg_activeMarkPolys.prevMark && time == cg_activeMarkPolys.prevMark->time )
		{
			CG_FreeMarkPoly( cg_activeMarkPolys.prevMark );
		}
	}

	le = cg_freeMarkPolys;
	cg_freeMarkPolys = cg_freeMarkPolys->nextMark;

	*le = {};

	// link into the active list
	le->nextMark = cg_activeMarkPolys.nextMark;
	le->prevMark = &cg_activeMarkPolys;
	cg_activeMarkPolys.nextMark->prevMark = le;
	cg_activeMarkPolys.nextMark = le;
	return le;
}

struct creepMarkCache_t;

struct mark_t
{
	// Set at register time.

	qhandle_t shader;
	// origin should be a point within a unit of the plane.
	vec3_t origin;
	// dir should be the plane normal
	vec3_t dir;
	float orientation;
	float red;
	float green;
	float blue;
	float alpha;
	bool alphaFade;
	float radius;
	/* temporary marks will not be stored or randomly oriented,
	but immediately passed to the renderer. */
	bool temporary;
	creepMarkCache_t *creepCache = nullptr;

	// Set at processing time.

	vec3_t axis[ 3 ];
	float texCoordScale;
};

BoundedVector<mark_t, MAX_MARK_POLYS> newMarks;

// Creep is a temporary mark, but unlike shadows and wakes it generally stays
// on the same world geometry for a long time. Keep its already projected mesh
// in cgame so it does not have to be clipped by the renderer every frame.
struct creepMarkCache_t
{
	bool valid = false;
	int buildable = 0;
	qhandle_t shader = 0;
	vec3_t origin;
	vec3_t dir;
	float radius = 0;
	int projectedTime = 0;
	int lastUsedTime = 0;
	std::vector<polyVert_t> vertices;
	std::vector<int> polySizes;
};

static creepMarkCache_t creepMarkCache[ MAX_GENTITIES ];
static Cvar::Range<Cvar::Cvar<int>> cg_creepMarkUpdateInterval(
	"cg_creepMarkUpdateInterval",
	"minimum milliseconds between creep mark projection updates (0 for every frame)",
	Cvar::CHEAT, 50, 0, 1000 );
static Cvar::Cvar<bool> cg_creepMarkCacheEnabled(
	"cg_creepMarkCache", "cache projected buildable creep marks", Cvar::CHEAT, true );
static Cvar::Range<Cvar::Cvar<int>> cg_creepMarkDistance(
	"cg_creepMarkDistance", "maximum distance at which buildable creep is drawn (0 for unlimited)",
	Cvar::CHEAT, 768, 0, 32768 );

static void CG_ClearCreepMarkCache()
{
	for ( creepMarkCache_t &cache : creepMarkCache )
	{
		cache = {};
	}
}

static bool CG_CreepMarkInputsMatch( const creepMarkCache_t &cache, int buildable,
	qhandle_t shader, const vec3_t origin, const vec3_t dir )
{
	return cache.valid && cache.buildable == buildable && cache.shader == shader &&
		!memcmp( cache.origin, origin, sizeof( vec3_t ) ) &&
		!memcmp( cache.dir, dir, sizeof( vec3_t ) );
}

static void CG_AddCachedCreepMark( const creepMarkCache_t &cache )
{
	size_t firstVert = 0;
	for ( size_t firstPoly = 0; firstPoly < cache.polySizes.size(); )
	{
		const int numVerts = cache.polySizes[ firstPoly ];
		size_t numPolys = 1;
		while ( firstPoly + numPolys < cache.polySizes.size() &&
			cache.polySizes[ firstPoly + numPolys ] == numVerts )
		{
			numPolys++;
		}

		trap_R_AddPolysToScene( cache.shader, numVerts, cache.vertices.data() + firstVert, numPolys );
		firstVert += numVerts * numPolys;
		firstPoly += numPolys;
	}
}

void CG_ProcessMarks()
{
	std::vector<markMsgInput_t> markMsgInput;
	std::vector<markMsgOutput_t> markMsgOutput;
	std::vector<mark_t*> projectedMarks;
	markMsgInput.reserve( newMarks.size() );
	projectedMarks.reserve( newMarks.size() );

	for ( mark_t &m : newMarks )
	{
		if ( m.creepCache && m.creepCache->valid )
		{
			CG_AddCachedCreepMark( *m.creepCache );
			continue;
		}

		markMsgInput_t input;
		auto& originalPoints = input.first;
		auto& projection = input.second;

		// create the texture axis
		VectorNormalize2( m.dir, m.axis[ 0 ] );
		PerpendicularVector( m.axis[ 1 ], m.axis[ 0 ] );
		RotatePointAroundVector( m.axis[ 2 ], m.axis[ 0 ], m.axis[ 1 ], m.orientation );
		CrossProduct( m.axis[ 0 ], m.axis[ 2 ], m.axis[ 1 ] );

		m.texCoordScale = 0.5 * 1.0 / m.radius;

		// create the full polygon
		originalPoints.resize( 4 );
		for ( size_t i = 0; i < 3; i++ )
		{
			originalPoints[ 0 ][ i ] = m.origin[ i ] - m.radius * m.axis[ 1 ][ i ] - m.radius * m.axis[ 2 ][ i ];
			originalPoints[ 1 ][ i ] = m.origin[ i ] + m.radius * m.axis[ 1 ][ i ] - m.radius * m.axis[ 2 ][ i ];
			originalPoints[ 2 ][ i ] = m.origin[ i ] + m.radius * m.axis[ 1 ][ i ] + m.radius * m.axis[ 2 ][ i ];
			originalPoints[ 3 ][ i ] = m.origin[ i ] - m.radius * m.axis[ 1 ][ i ] + m.radius * m.axis[ 2 ][ i ];
		}

		VectorScale( m.dir, -20, projection );
		markMsgInput.push_back( std::move( input ) );
		projectedMarks.push_back( &m );
	}

	trap_CM_BatchMarkFragments( 384, 128, markMsgInput, markMsgOutput );

	size_t numMarks = projectedMarks.size();
	for ( size_t k = 0; k < numMarks; k++ )
	{
		const markMsgOutput_t& output = markMsgOutput[ k ];
		const std::vector<markFragment_t> &markFragments = output.second;
		const auto &markPoints = output.first;

		const mark_t& m = *projectedMarks[ k ];
		creepMarkCache_t *cache = m.creepCache;
		if ( cache )
		{
			cache->vertices.clear();
			cache->polySizes.clear();
		}

		byte colors[ 4 ];
		colors[ 0 ] = m.red * 255;
		colors[ 1 ] = m.green * 255;
		colors[ 2 ] = m.blue * 255;
		colors[ 3 ] = m.alpha * 255;

		for ( const markFragment_t &markFragment : markFragments )
		{
			// we have an upper limit on the complexity of polygons
			// that we store persistently
			const int numPoints = std::min( markFragment.numPoints, MAX_VERTS_ON_POLY );

			polyVert_t verts[ MAX_VERTS_ON_POLY ];

			for ( int j = 0; j < numPoints; j++ )
			{
				polyVert_t& vert = verts[ j ];
				const std::array<float, 3> &markPoint = markPoints[ markFragment.firstPoint + j ];

				VectorCopy( markPoint, vert.xyz );

				vec3_t delta;
				VectorSubtract( vert.xyz, m.origin, delta );

				vert.st[ 0 ] = 0.5 + DotProduct( delta, m.axis[ 1 ] ) * m.texCoordScale;
				vert.st[ 1 ] = 0.5 + DotProduct( delta, m.axis[ 2 ] ) * m.texCoordScale;
				*(int*) vert.modulate = *(int*) colors;
			}

			// Store creep meshes for reuse. Other temporary marks remain transient.
			if ( m.temporary )
			{
				if ( cache )
				{
					cache->vertices.insert( cache->vertices.end(), verts, verts + numPoints );
					cache->polySizes.push_back( numPoints );
				}
				else
				{
					trap_R_AddPolyToScene( m.shader, numPoints, verts );
				}
				continue;
			}

			// otherwise save it persistently
			markPoly_t *mp = CG_AllocMarkPoly();

			mp->time = cg.time;
			mp->alphaFade = m.alphaFade;
			mp->shader = m.shader;
			mp->poly.numVerts = numPoints;
			mp->color[ 0 ] = m.red;
			mp->color[ 1 ] = m.green;
			mp->color[ 2 ] = m.blue;
			mp->color[ 3 ] = m.alpha;

			memcpy( mp->verts, verts, numPoints * sizeof( polyVert_t ) );
		}

		if ( cache )
		{
			cache->valid = true; // Empty projection results are useful cache entries too.
			cache->radius = m.radius;
			cache->projectedTime = cg.time;
			cache->lastUsedTime = cg.time;
			CG_AddCachedCreepMark( *cache );
		}
	}
}

void CG_RegisterMark( qhandle_t shader, const vec3_t origin, const vec3_t dir,
                    float orientation, float red, float green, float blue, float alpha,
                    bool alphaFade, float radius, bool temporary )
{
	if ( !cg_addMarks.Get() )
	{
		return;
	}

	if( temporary )
	{
		if( CG_CullPointAndRadius( origin, M_SQRT2 * radius ) )
		{
			return;
		}
	}

	if ( radius <= 0 )
	{
		Sys::Drop( "CG_ProcessMark called with <= 0 radius" );
	}

	mark_t m{};

	m.shader = shader;
	VectorCopy( origin, m.origin );
	VectorCopy( dir, m.dir );
	m.orientation = orientation;
	m.red = red;
	m.green = green;
	m.blue = blue;
	m.alpha = alpha;
	m.alphaFade = alphaFade;
	m.radius = radius;
	m.temporary = temporary;

	newMarks.append( m );
}

void CG_RegisterCreepMark( int entityNum, int buildable, qhandle_t shader,
	const vec3_t origin, const vec3_t dir, float radius )
{
	if ( !cg_addMarks.Get() )
	{
		return;
	}

	if ( CG_CullPointAndRadius( origin, M_SQRT2 * radius ) )
	{
		return;
	}

	const int maxDistance = cg_creepMarkDistance.Get();
	if ( maxDistance && Distance( cg.refdef.vieworg, origin ) > maxDistance + radius )
	{
		return;
	}

	if ( !cg_creepMarkCacheEnabled.Get() )
	{
		CG_RegisterMark( shader, origin, dir, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
			false, radius, true );
		return;
	}

	creepMarkCache_t &cache = creepMarkCache[ entityNum ];
	if ( cache.lastUsedTime > cg.time )
	{
		cache = {};
	}

	const bool matchingInputs = CG_CreepMarkInputsMatch( cache, buildable, shader, origin, dir );
	const bool useCachedMesh = matchingInputs &&
		( cache.radius == radius || cg.time - cache.projectedTime < cg_creepMarkUpdateInterval.Get() );

	mark_t m{};
	m.shader = shader;
	VectorCopy( origin, m.origin );
	VectorCopy( dir, m.dir );
	m.red = m.green = m.blue = m.alpha = 1.0f;
	m.radius = radius;
	m.temporary = true;
	m.creepCache = &cache;

	if ( !useCachedMesh )
	{
		cache = {};
		cache.buildable = buildable;
		cache.shader = shader;
		VectorCopy( origin, cache.origin );
		VectorCopy( dir, cache.dir );
	}
	else
	{
		cache.lastUsedTime = cg.time;
	}

	newMarks.append( m );
}

void CG_ResetMarks()
{
	newMarks.clear();
}

/*
===============
CG_AddMarks
===============
*/
#define MARK_TOTAL_TIME 10000
#define MARK_FADE_TIME  1000

void CG_AddMarkPolys()
{
	int        j;
	markPoly_t *mp, *next;
	int        t;
	int        fade;
	qhandle_t  batchShader = 0;
	int        batchNumVerts = 0;
	std::vector<polyVert_t> batchVerts;

	auto flushBatch = [&]()
	{
		if ( !batchVerts.empty() )
		{
			trap_R_AddPolysToScene( batchShader, batchNumVerts, batchVerts.data(),
				batchVerts.size() / batchNumVerts );
			batchVerts.clear();
		}
	};

	if ( !cg_addMarks.Get() )
	{
		return;
	}

	mp = cg_activeMarkPolys.nextMark;

	for ( ; mp != &cg_activeMarkPolys; mp = next )
	{
		// grab next now, so if the local entity is freed we
		// still have it
		next = mp->nextMark;

		// see if it is time to completely remove it
		if ( cg.time > mp->time + MARK_TOTAL_TIME )
		{
			CG_FreeMarkPoly( mp );
			continue;
		}

		// fade all marks out with time
		t = mp->time + MARK_TOTAL_TIME - cg.time;

		if ( t < MARK_FADE_TIME )
		{
			fade = 255 * t / MARK_FADE_TIME;

			if ( mp->alphaFade )
			{
				for ( j = 0; j < mp->poly.numVerts; j++ )
				{
					mp->verts[ j ].modulate[ 3 ] = fade;
				}
			}
			else
			{
				for ( j = 0; j < mp->poly.numVerts; j++ )
				{
					mp->verts[ j ].modulate[ 0 ] = mp->color[ 0 ] * fade;
					mp->verts[ j ].modulate[ 1 ] = mp->color[ 1 ] * fade;
					mp->verts[ j ].modulate[ 2 ] = mp->color[ 2 ] * fade;
				}
			}
		}
		if ( batchVerts.empty() )
		{
			batchShader = mp->shader;
			batchNumVerts = mp->poly.numVerts;
		}
		else if ( batchShader != mp->shader || batchNumVerts != mp->poly.numVerts )
		{
			flushBatch();
			batchShader = mp->shader;
			batchNumVerts = mp->poly.numVerts;
		}

		batchVerts.insert( batchVerts.end(), mp->verts, mp->verts + mp->poly.numVerts );
	}

	flushBatch();
}
