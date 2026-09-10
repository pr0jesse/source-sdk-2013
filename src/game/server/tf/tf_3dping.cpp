//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Server-side validation and routing for 3D pings.
//
//=============================================================================

#include "cbase.h"
#include "tf_3dping.h"
#include "tf_3dping_shared.h"
#include "tf_player.h"
#include "tf_obj.h"
#include "tf_gc_server.h"
#include "tf_party.h"
#include "recipientfilter.h"
#include "usermessages.h"
#include "entity_healthkit.h"
#include "entity_ammopack.h"
#include "tf_ammo_pack.h"
#include "tf_gamerules.h"
#include "bot/tf_bot.h"
#include "player_vs_environment/tf_tank_boss.h"
#include "igamesystem.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

ConVar tf_3dping_cooldown( "tf_3dping_cooldown", TF_3DPING_DEFAULT_COOLDOWN, FCVAR_NONE,
	"Minimum time in seconds between 3D pings from the same player. Not the burst limit, see tf_3dping_burst_* below.", true, 0.05f, true, 10.0f );

ConVar tf_3dping_lifetime( "tf_3dping_lifetime", TF_3DPING_DEFAULT_LIFETIME, FCVAR_NONE,
	"How long a 3D ping marker stays visible, in seconds.", true, 1.0f, true, 30.0f );

ConVar tf_3dping_max_range( "tf_3dping_max_range", "8192", FCVAR_NONE,
	"Maximum distance in world units a 3D ping's target may be from the sender.", true, 256.0f, true, MAX_COORD_FLOAT );

// Burst rate limit.
ConVar tf_3dping_burst_count( "tf_3dping_burst_count", "5", FCVAR_NONE,
	"Number of pings within tf_3dping_burst_window that triggers the longer tf_3dping_burst_lockout cooldown.", true, 2.0f, true, 15.0f );

ConVar tf_3dping_burst_window( "tf_3dping_burst_window", "3.0", FCVAR_NONE,
	"Time window in seconds within which tf_3dping_burst_count pings count as a spam burst.", true, 1.0f, true, 30.0f );

ConVar tf_3dping_burst_lockout( "tf_3dping_burst_lockout", "5.0", FCVAR_NONE,
	"Extra cooldown in seconds imposed after a player trips the ping burst limiter.", true, 1.0f, true, 120.0f );

ConVar tf_3dping_team_allowed( "tf_3dping_team_allowed", "0", FCVAR_NONE,
	"Allow team-wide 3D pings on this server. A player's own cl_3dping_team_enabled has no effect until this is also on." );

ConVar tf_3dping_spectator_allowed( "tf_3dping_spectator_allowed", "0", FCVAR_NONE,
	"Let spectators receive team-scope 3D pings for the team they're observing. Requires tf_3dping_team_allowed too." );

ConVar tf_3dping_sourcetv_allowed( "tf_3dping_sourcetv_allowed", "0", FCVAR_NONE,
	"Let SourceTV receive team-scope 3D pings for whichever team it's currently observing. Requires tf_3dping_team_allowed too." );

// Per-player cooldown tracking, indexed by entindex(). Slot 0 unused.
static float s_flTF3DPingNextAllowed[ MAX_PLAYERS + 1 ];
static float s_flTF3DPingBurstWindowStart[ MAX_PLAYERS + 1 ];
static int   s_iTF3DPingBurstCount[ MAX_PLAYERS + 1 ];

static bool  s_bTF3DPingWasStealthed[ MAX_PLAYERS + 1 ];
static float s_flTF3DPingLastUnstealthTime[ MAX_PLAYERS + 1 ];

// Tracks decloak timing for the post-cloak ping lockout below, even if
// the player never pings.
class CTF3DPingStealthWatcher : public CAutoGameSystemPerFrame
{
public:
	CTF3DPingStealthWatcher() : CAutoGameSystemPerFrame( "CTF3DPingStealthWatcher" ) {}

	// Static timing state must be reset when curtime restarts.
	virtual void LevelInitPreEntity()
	{
		V_memset( s_flTF3DPingNextAllowed, 0, sizeof( s_flTF3DPingNextAllowed ) );
		V_memset( s_flTF3DPingBurstWindowStart, 0, sizeof( s_flTF3DPingBurstWindowStart ) );
		V_memset( s_iTF3DPingBurstCount, 0, sizeof( s_iTF3DPingBurstCount ) );
		V_memset( s_bTF3DPingWasStealthed, 0, sizeof( s_bTF3DPingWasStealthed ) );
		V_memset( s_flTF3DPingLastUnstealthTime, 0, sizeof( s_flTF3DPingLastUnstealthTime ) );
	}

	virtual void FrameUpdatePostEntityThink()
	{
		for ( int i = 1; i <= MAX_PLAYERS; i++ )
		{
			CTFPlayer *pPlayer = ToTFPlayer( UTIL_PlayerByIndex( i ) );
			if ( !pPlayer )
				continue;

			bool bStealthed = pPlayer->m_Shared.IsStealthed();
			if ( s_bTF3DPingWasStealthed[ i ] && !bStealthed )
			{
				s_flTF3DPingLastUnstealthTime[ i ] = gpGlobals->curtime;
			}
			s_bTF3DPingWasStealthed[ i ] = bStealthed;
		}
	}
};
static CTF3DPingStealthWatcher g_TF3DPingStealthWatcher;

// True while pPlayer is cloaked or still inside the post-cloak lockout
// window after decloaking.
static bool TF3DPing_IsStealthLocked( CTFPlayer *pPlayer )
{
	if ( pPlayer->m_Shared.IsStealthed() )
		return true;

	int iSlot = pPlayer->entindex();
	if ( iSlot < 1 || iSlot > MAX_PLAYERS )
		return false;

	return ( gpGlobals->curtime - s_flTF3DPingLastUnstealthTime[ iSlot ] ) < TF_3DPING_POST_CLOAK_LOCKOUT;
}

enum ETF3DPingRecipientScope
{
	TF_3DPING_RECIPIENT_NONE = 0,
	TF_3DPING_RECIPIENT_PARTY,	// sender's current TF party (default scope)
	TF_3DPING_RECIPIENT_TEAM,		// optional, off-by-default same-team broadcast
};

// Recipient scope is resolved server-side.
static ETF3DPingRecipientScope TF3DPing_GetRecipientScope( CTFPlayer *pSender, CTFPlayer *pOther, CTFParty *pSenderParty, bool bTeamPingEnabled )
{
	if ( !pOther || pOther == pSender )
		return TF_3DPING_RECIPIENT_NONE;

	// Spectator/SourceTV: only the team-scope stream for the team they're
	// watching, mirroring CTeamRecipientFilter. Never party scope.
	if ( pOther->GetTeamNumber() == TEAM_SPECTATOR || pOther->IsHLTV() )
	{
		if ( !bTeamPingEnabled )
			return TF_3DPING_RECIPIENT_NONE;

		bool bAllowed = pOther->IsHLTV() ? tf_3dping_sourcetv_allowed.GetBool() : tf_3dping_spectator_allowed.GetBool();
		if ( !bAllowed )
			return TF_3DPING_RECIPIENT_NONE;

		int iObsMode = pOther->GetObserverMode();
		if ( iObsMode != OBS_MODE_IN_EYE && iObsMode != OBS_MODE_CHASE && iObsMode != OBS_MODE_POI )
			return TF_3DPING_RECIPIENT_NONE;

		CBaseEntity *pObserverTarget = pOther->GetObserverTarget();
		if ( !pObserverTarget || pObserverTarget->GetTeamNumber() != pSender->GetTeamNumber() )
			return TF_3DPING_RECIPIENT_NONE;

		return TF_3DPING_RECIPIENT_TEAM;
	}

	if ( pOther->IsBot() )
		return TF_3DPING_RECIPIENT_NONE;

	CSteamID steamIDOther;
	if ( !pOther->GetSteamID( &steamIDOther ) )
		return TF_3DPING_RECIPIENT_NONE;

	// Default scope: current TF party only.
	if ( pSenderParty && pSenderParty->GetMemberIndexBySteamID( steamIDOther ) != -1 )
		return TF_3DPING_RECIPIENT_PARTY;

	// Optional, off-by-default scope: real same-team, connected players.
	if ( bTeamPingEnabled &&
		 pOther->GetTeamNumber() > TEAM_SPECTATOR &&
		 pOther->GetTeamNumber() == pSender->GetTeamNumber() )
	{
		return TF_3DPING_RECIPIENT_TEAM;
	}

	return TF_3DPING_RECIPIENT_NONE;
}

// Split into party-scope vs. team-scope messages, so each client knows
// which scope it came from and picks the right notification sound (see
// OnPingReceived()).
static void TF3DPing_SendToRecipients( CTFPlayer *pSender, ETF3DPingType eType, const Vector &vecPosition, bool bHostile )
{
	CSteamID steamIDSender;
	if ( !pSender->GetSteamID( &steamIDSender ) )
		return;

	// No live GC connection (most community servers) makes GetPartyForPlayer()
	// return NULL, falling back to self-only unless team pings are enabled.
	CTFParty *pSenderParty = GTFGCClientSystem()->GetPartyForPlayer( steamIDSender );

	bool bTeamPingEnabled = tf_3dping_team_allowed.GetBool() &&
		FStrEq( engine->GetClientConVarValue( pSender->entindex(), TF_3DPING_TEAM_CONVAR_NAME ), "1" );

	CRecipientFilter partyFilter;
	partyFilter.MakeReliable();
	partyFilter.AddRecipient( pSender );

	CRecipientFilter teamFilter;
	teamFilter.MakeReliable();

	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CTFPlayer *pOther = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		switch ( TF3DPing_GetRecipientScope( pSender, pOther, pSenderParty, bTeamPingEnabled ) )
		{
			case TF_3DPING_RECIPIENT_PARTY:	partyFilter.AddRecipient( pOther );	break;
			case TF_3DPING_RECIPIENT_TEAM:		teamFilter.AddRecipient( pOther );		break;
			default: break;
		}
	}

	UserMessageBegin( partyFilter, TF_3DPING_USERMSG_NAME );
		WRITE_BYTE( eType );
		WRITE_BYTE( pSender->entindex() );
		WRITE_VEC3COORD( vecPosition );
		WRITE_FLOAT( tf_3dping_lifetime.GetFloat() );
		WRITE_BYTE( bHostile ? 1 : 0 );	// selects the enemy vs. default marker icon client-side
		WRITE_BYTE( 1 );					// delivered via party scope (recipient is the sender or a party member)
	MessageEnd();

	if ( teamFilter.GetRecipientCount() > 0 )
	{
		UserMessageBegin( teamFilter, TF_3DPING_USERMSG_NAME );
			WRITE_BYTE( eType );
			WRITE_BYTE( pSender->entindex() );
			WRITE_VEC3COORD( vecPosition );
			WRITE_FLOAT( tf_3dping_lifetime.GetFloat() );
			WRITE_BYTE( bHostile ? 1 : 0 );
			WRITE_BYTE( 0 );				// delivered via team-wide broadcast scope only (not a party member)
		MessageEnd();
	}
}

// Detection radius grows with distance (a cone, not a cylinder), since the
// same aim error covers more world space farther away.
ConVar tf_3dping_aim_tolerance_angle( "tf_3dping_aim_tolerance_angle", "2.0", FCVAR_NONE,
	"How forgiving 3D ping target detection is, in degrees off your exact aim, before it falls back to a plain world ping. Higher = easier to tag things from a distance.", true, 0.0f, true, 15.0f );

#define TF_3DPING_AIM_BASE_TOLERANCE	0.5f

// Caps the distance-scaled slack above, so a far-off target's detection
// radius stays close to its own size instead of ballooning with range.
#define TF_3DPING_AIM_MAX_TOLERANCE		1.5f

// Extra height on top of a player target's WorldSpaceCenter(), so the
// marker sits at chest height instead of the waist.
#define TF_3DPING_PLAYER_HEIGHT_LIFT	14.0f

// Shared by the precise aim trace and the nearby search below, so the
// stealth/disguise checks apply identically either way.
static bool TF3DPing_ClassifyEntity( CTFPlayer *pSender, CBaseEntity *pHit, ETF3DPingType *pOutType, Vector *pOutPosition, bool *pOutHostile )
{
	if ( !pHit )
		return false;

	if ( pHit->IsPlayer() )
	{
		CTFPlayer *pTarget = ToTFPlayer( pHit );
		if ( !pTarget )
			return false;

		// A dead player's entity can stick around (ragdoll/observer transition).
		// Don't let a ping latch onto a corpse.
		if ( !pTarget->IsAlive() )
			return false;

		// Don't expose cloaked players through ping targeting.
		if ( pTarget->m_Shared.IsStealthed() )
			return false;

		// Disguised as the enemy relative to the sender: still undercover,
		// a ping must not mark their position.
		if ( pTarget->m_Shared.InCond( TF_COND_DISGUISED ) &&
			pTarget->m_Shared.GetDisguiseTeam() != pSender->GetTeamNumber() )
		{
			return false;
		}

		bool bRealEnemy = pTarget->GetTeamNumber() != pSender->GetTeamNumber();

		// Sentry Buster (CTFBot), always hostile. Uses the building icon.
		CTFBot *pBot = ToTFBot( pTarget );
		if ( pBot && pBot->HasMission( CTFBot::MISSION_DESTROY_SENTRIES ) )
		{
			*pOutType = TF_3DPING_BUILDING;
			*pOutPosition = pTarget->WorldSpaceCenter();
			*pOutHostile = true;
			return true;
		}

		// Preserve disguise information visible to the sender.
		bool bDisguisedAsSenderTeam = bRealEnemy &&
			pTarget->m_Shared.InCond( TF_COND_DISGUISED ) &&
			pTarget->m_Shared.GetDisguiseTeam() == pSender->GetTeamNumber();
		bool bEnemy = bRealEnemy && !bDisguisedAsSenderTeam;

		*pOutType = TF_3DPING_PLAYER;
		// WorldSpaceCenter(), not GetAbsOrigin() (feet level), plus a lift
		// toward chest height.
		*pOutPosition = pTarget->WorldSpaceCenter() + Vector( 0, 0, TF_3DPING_PLAYER_HEIGHT_LIFT );
		*pOutHostile = bEnemy;
		return true;
	}

	if ( dynamic_cast< CHealthKit* >( pHit ) )
	{
		*pOutType = TF_3DPING_PICKUP_HEALTH;
		*pOutPosition = pHit->WorldSpaceCenter();
		*pOutHostile = false;
		return true;
	}

	// Map ammo and dropped TF ammo use different entity classes.
	if ( dynamic_cast< CAmmoPack* >( pHit ) || dynamic_cast< CTFAmmoPack* >( pHit ) )
	{
		*pOutType = TF_3DPING_PICKUP_AMMO;
		*pOutPosition = pHit->WorldSpaceCenter();
		*pOutHostile = false;
		return true;
	}

	if ( pHit->IsBaseObject() )
	{
		CBaseObject *pObj = dynamic_cast< CBaseObject* >( pHit );
		if ( !pObj || pObj->IsPlacing() )
			return false;

		int iObjType = pObj->GetType();
		bool bRecognizedBuilding = ( iObjType == OBJ_SENTRYGUN || iObjType == OBJ_DISPENSER || iObjType == OBJ_TELEPORTER );
		if ( !bRecognizedBuilding )
			return false;

		*pOutType = TF_3DPING_BUILDING;
		*pOutPosition = pObj->WorldSpaceCenter();
		*pOutHostile = ( pObj->GetTeamNumber() != pSender->GetTeamNumber() );
		return true;
	}

	// MvM tank, always hostile. Uses the building icon.
	CTFTankBoss *pTank = dynamic_cast< CTFTankBoss* >( pHit );
	if ( pTank )
	{
		*pOutType = TF_3DPING_BUILDING;
		*pOutPosition = pTank->WorldSpaceCenter();
		*pOutHostile = true;
		return true;
	}

	return false;
}

// Ping positions are fixed when created.
static void TF3DPing_ClassifyTrace( CTFPlayer *pSender, const trace_t &tr, ETF3DPingType *pOutType, Vector *pOutPosition, bool *pOutHostile )
{
	*pOutType = TF_3DPING_WORLD;
	*pOutPosition = tr.endpos;
	*pOutHostile = false;

	TF3DPing_ClassifyEntity( pSender, tr.m_pEnt, pOutType, pOutPosition, pOutHostile );
}

#define TF_3DPING_MAX_TRACE_SKIPS	4

class CTF3DPingTraceFilter : public CTraceFilterSimple
{
public:
	CTF3DPingTraceFilter( const IHandleEntity *pSender, CBaseEntity **ppSkipList, int nSkipCount )
		: CTraceFilterSimple( pSender, COLLISION_GROUP_PLAYER )
		, m_ppSkipList( ppSkipList )
		, m_nSkipCount( nSkipCount )
	{
	}

	virtual bool ShouldHitEntity( IHandleEntity *pHandleEntity, int contentsMask )
	{
		if ( !CTraceFilterSimple::ShouldHitEntity( pHandleEntity, contentsMask ) )
			return false;

		CBaseEntity *pEntity = EntityFromEntityHandle( pHandleEntity );
		for ( int i = 0; i < m_nSkipCount; i++ )
		{
			if ( pEntity == m_ppSkipList[i] )
				return false;
		}
		return true;
	}

private:
	CBaseEntity **m_ppSkipList;
	int m_nSkipCount;
};

// Retraces past dead or cloaked players instead of stopping on them.
static void TF3DPing_TraceSeeThroughInvalidPlayers( CTFPlayer *pSender, const Vector &vecStart, const Vector &vecEnd, trace_t *pOutTrace )
{
	CBaseEntity *pSkipList[ TF_3DPING_MAX_TRACE_SKIPS ];
	pSkipList[0] = pSender;
	int nSkipCount = 1;

	Ray_t ray;
	ray.Init( vecStart, vecEnd );

	for ( int i = 0; i < TF_3DPING_MAX_TRACE_SKIPS; i++ )
	{
		CTF3DPingTraceFilter filter( pSender, pSkipList, nSkipCount );
		enginetrace->TraceRay( ray, MASK_SOLID, &filter, pOutTrace );

		if ( !pOutTrace->m_pEnt || !pOutTrace->m_pEnt->IsPlayer() )
			return;

		ETF3DPingType eDummyType;
		Vector vecDummyPosition;
		bool bDummyHostile;
		if ( TF3DPing_ClassifyEntity( pSender, pOutTrace->m_pEnt, &eDummyType, &vecDummyPosition, &bDummyHostile ) )
			return;	// a valid target, stop here

		if ( nSkipCount >= TF_3DPING_MAX_TRACE_SKIPS )
			return;	// avoid looping forever against a stack of invalid players

		pSkipList[ nSkipCount++ ] = pOutTrace->m_pEnt;
	}
}

// Finds taggable entities close to the aim ray.
static CBaseEntity *TF3DPing_FindAimedTarget( CTFPlayer *pSender, const Vector &vecStart, const Vector &vecEnd, float flMaxFraction,
	ETF3DPingType *pOutType, Vector *pOutPosition, bool *pOutHostile )
{
	Vector vecDir = vecEnd - vecStart;
	float flRayLength = VectorNormalize( vecDir );
	if ( flRayLength <= 0.0f )
		return NULL;

	float flAimSlackTan = tan( DEG2RAD( tf_3dping_aim_tolerance_angle.GetFloat() ) );

	CBaseEntity *pBest = NULL;
	float flBestFraction = flMaxFraction;

	for ( CBaseEntity *pEnt = gEntList.FirstEnt(); pEnt; pEnt = gEntList.NextEnt( pEnt ) )
	{
		if ( pEnt == pSender )
			continue;

		bool bCandidate = dynamic_cast< CHealthKit* >( pEnt ) || dynamic_cast< CAmmoPack* >( pEnt ) ||
			dynamic_cast< CTFAmmoPack* >( pEnt ) || pEnt->IsPlayer() || pEnt->IsBaseObject();
		if ( !bCandidate )
			continue;

		const Vector &vecCenter = pEnt->WorldSpaceCenter();
		Vector vecToEnt = vecCenter - vecStart;
		float flAlongRay = DotProduct( vecToEnt, vecDir );

		// Behind the sender, or farther than the ray even reaches: not aimed at.
		if ( flAlongRay < 0.0f || flAlongRay > flRayLength )
			continue;

		float flSlack = min( TF_3DPING_AIM_BASE_TOLERANCE + ( flAlongRay * flAimSlackTan ), TF_3DPING_AIM_MAX_TOLERANCE );
		float flRadius = pEnt->BoundingRadius() + flSlack;
		Vector vecClosestPointOnRay = vecStart + vecDir * flAlongRay;
		if ( vecClosestPointOnRay.DistToSqr( vecCenter ) > flRadius * flRadius )
			continue;

		float flFraction = flAlongRay / flRayLength;
		if ( flFraction >= flBestFraction )
			continue;

		// Close enough to the ray. Still must pass ClassifyEntity's rules.
		ETF3DPingType eCandidateType;
		Vector vecCandidatePosition;
		bool bCandidateHostile;
		if ( !TF3DPing_ClassifyEntity( pSender, pEnt, &eCandidateType, &vecCandidatePosition, &bCandidateHostile ) )
			continue;

		flBestFraction = flFraction;
		pBest = pEnt;
		*pOutType = eCandidateType;
		*pOutPosition = vecCandidatePosition;
		*pOutHostile = bCandidateHostile;
	}

	return pBest;
}

// Entry point from CTFPlayer::ClientCommand.
void TF3DPing_OnPlayerPingCommand( CTFPlayer *pSender, const CCommand &args )
{
	if ( !pSender )
		return;

	if ( !pSender->IsAlive() || pSender->IsObserver() )
		return;

	// A Spy undercover (cloaked, briefly after decloaking, or disguised as
	// the enemy) can't ping without breaking their own cover.
	if ( TF3DPing_IsStealthLocked( pSender ) )
		return;

	if ( pSender->m_Shared.InCond( TF_COND_DISGUISED ) &&
		pSender->m_Shared.GetDisguiseTeam() != pSender->GetTeamNumber() )
	{
		return;
	}

	// No pinging before the match starts or after it ends.
	CTFGameRules *pRules = TFGameRules();
	if ( pRules )
	{
		if ( pRules->IsInWaitingForPlayers() || pRules->InSetup() )
			return;

		gamerules_roundstate_t eState = pRules->State_Get();
		if ( eState == GR_STATE_TEAM_WIN || eState == GR_STATE_GAME_OVER )
			return;
	}

	int iSlot = pSender->entindex();
	if ( iSlot < 1 || iSlot > MAX_PLAYERS )
		return;

	float flNow = gpGlobals->curtime;
	if ( flNow < s_flTF3DPingNextAllowed[ iSlot ] )
		return;

	if ( flNow - s_flTF3DPingBurstWindowStart[ iSlot ] > tf_3dping_burst_window.GetFloat() )
	{
		s_flTF3DPingBurstWindowStart[ iSlot ] = flNow;
		s_iTF3DPingBurstCount[ iSlot ] = 0;
	}
	s_iTF3DPingBurstCount[ iSlot ]++;

	if ( s_iTF3DPingBurstCount[ iSlot ] >= tf_3dping_burst_count.GetInt() )
	{
		// This ping still goes out. The next one waits out the lockout.
		s_flTF3DPingNextAllowed[ iSlot ] = flNow + tf_3dping_burst_lockout.GetFloat();
		s_iTF3DPingBurstCount[ iSlot ] = 0;
		s_flTF3DPingBurstWindowStart[ iSlot ] = flNow;

		ClientPrint( pSender, HUD_PRINTCONSOLE, UTIL_VarArgs(
			"[3D Ping] Pinged too many times too quickly, locked out for %.0fs.\n", tf_3dping_burst_lockout.GetFloat() ) );
	}
	else
	{
		s_flTF3DPingNextAllowed[ iSlot ] = flNow + tf_3dping_cooldown.GetFloat();
	}

	Vector vecEyePos = pSender->EyePosition();
	Vector vecForward;
	AngleVectors( pSender->EyeAngles(), &vecForward );
	Vector vecTraceEnd = vecEyePos + vecForward * MAX_TRACE_LENGTH;

	trace_t tr;
	TF3DPing_TraceSeeThroughInvalidPlayers( pSender, vecEyePos, vecTraceEnd, &tr );

	ETF3DPingType eType;
	Vector vecPosition;
	bool bHostile;

	// Check the precise crosshair hit first, or a nearby player just
	// off-crosshair can steal the ping from something farther away.
	bool bGotTarget = tr.m_pEnt && TF3DPing_ClassifyEntity( pSender, tr.m_pEnt, &eType, &vecPosition, &bHostile );

	if ( !bGotTarget )
	{
		// Catches trigger-only pickups the solid trace missed.
		CBaseEntity *pTarget = TF3DPing_FindAimedTarget( pSender, vecEyePos, vecTraceEnd, tr.fraction, &eType, &vecPosition, &bHostile );

		if ( !pTarget )
		{
			if ( tr.fraction >= 1.0f || !tr.m_pEnt )
				return;

			TF3DPing_ClassifyTrace( pSender, tr, &eType, &vecPosition, &bHostile );
		}
	}

	if ( vecEyePos.DistTo( vecPosition ) > tf_3dping_max_range.GetFloat() )
		return;

	TF3DPing_SendToRecipients( pSender, eType, vecPosition, bHostile );
}
