//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Client-side Streamer Mode display helpers.
//
//=============================================================================

#include "cbase.h"
#include "tf_streamer_mode.h"
#include "cdll_util.h"
#include "c_tf_player.h"
#include "c_playerresource.h"
#include "c_tf_playerresource.h"
#include "tf_shareddefs.h"
#include "utlmap.h"
#include "vgui_avatarimage.h"
#include "steam/steam_api.h"
#include "econ_item_description.h"
#include "tf_party.h"
#include "tf_gc_client.h"
#include <game/client/iviewport.h>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static void TF_StreamerMode_OnPresentationConVarChanged( IConVar *pConVar, const char *pszOldValue, float flOldValue )
{
	if ( gViewPortInterface )
	{
		gViewPortInterface->UpdateAllPanels();
	}
}

ConVar tf_streamer_mode( "tf_streamer_mode", "0", FCVAR_ARCHIVE | FCVAR_CLIENTDLL,
	"Master switch for Streamer Mode. Hides or anonymises identifying info in your local display. Doesn't affect gameplay, networking, or what other players see.",
	TF_StreamerMode_OnPresentationConVarChanged );

ConVar tf_streamer_mode_name( "tf_streamer_mode_name", "You", FCVAR_ARCHIVE | FCVAR_CLIENTDLL,
	"Local display name used while Streamer Mode is enabled." );

ConVar tf_streamer_mode_name_style( "tf_streamer_mode_name_style", "0", FCVAR_ARCHIVE | FCVAR_CLIENTDLL,
	"Anonymous naming style for other players: 0 = Player Numbers, 1 = TF2 Bot Names.", true, 0.0f, true, (float)( TF_STREAMER_NAME_STYLE_COUNT - 1 ),
	TF_StreamerMode_OnPresentationConVarChanged );

ConVar tf_streamer_mode_anonymise_names( "tf_streamer_mode_anonymise_names", "1", FCVAR_ARCHIVE | FCVAR_CLIENTDLL,
	"Replace other players' names with an anonymous alias in passive UI (scoreboard, killfeed, target ID, etc).",
	TF_StreamerMode_OnPresentationConVarChanged );

ConVar tf_streamer_mode_anonymise_party_members( "tf_streamer_mode_anonymise_party_members", "0", FCVAR_ARCHIVE | FCVAR_CLIENTDLL,
	"Also anonymise your own party members' names and avatars.",
	TF_StreamerMode_OnPresentationConVarChanged );

ConVar tf_streamer_mode_default_avatars( "tf_streamer_mode_default_avatars", "1", FCVAR_ARCHIVE | FCVAR_CLIENTDLL,
	"Show the default Steam avatar instead of other players' real avatars in passive UI.",
	TF_StreamerMode_OnPresentationConVarChanged );

ConVar tf_streamer_mode_hide_chat( "tf_streamer_mode_hide_chat", "1", FCVAR_ARCHIVE | FCVAR_CLIENTDLL,
	"Suppress chat HUD messages (public, team, system/gift). Party chat has its own toggle below." );

ConVar tf_streamer_mode_hide_party_chat( "tf_streamer_mode_hide_party_chat", "1", FCVAR_ARCHIVE | FCVAR_CLIENTDLL,
	"Suppress party chat. Independent of tf_streamer_mode_hide_chat." );

ConVar tf_streamer_mode_hide_callouts( "tf_streamer_mode_hide_callouts", "0", FCVAR_ARCHIVE | FCVAR_CLIENTDLL,
	"Hide voice-menu callouts (Medic!, Incoming!, Sniper!, etc). Independent of tf_streamer_mode_hide_chat." );

ConVar tf_streamer_mode_disable_voice( "tf_streamer_mode_disable_voice", "1", FCVAR_ARCHIVE | FCVAR_CLIENTDLL,
	"Don't play incoming voice chat audio.",
	TF_StreamerMode_OnPresentationConVarChanged );

ConVar tf_streamer_mode_hide_server_info( "tf_streamer_mode_hide_server_info", "1", FCVAR_ARCHIVE | FCVAR_CLIENTDLL,
	"Hide server hostname/IP from local UI. Map name stays visible." );

ConVar tf_streamer_mode_disable_join_presence( "tf_streamer_mode_disable_join_presence", "1", FCVAR_ARCHIVE | FCVAR_CLIENTDLL,
	"Reduce Steam Rich Presence to a generic status instead of a directly-joinable server location." );

ConVar tf_streamer_mode_hide_custom_item_text( "tf_streamer_mode_hide_custom_item_text", "1", FCVAR_ARCHIVE | FCVAR_CLIENTDLL,
	"Hide other players' custom item names and descriptions. Doesn't hide your own." );

ConVar tf_streamer_mode_hide_custom_images( "tf_streamer_mode_hide_custom_images", "1", FCVAR_ARCHIVE | FCVAR_CLIENTDLL,
	"Hide other players' sprays and custom images. Doesn't hide your own." );

ConVar tf_streamer_mode_hide_friends_list( "tf_streamer_mode_hide_friends_list", "1", FCVAR_ARCHIVE | FCVAR_CLIENTDLL,
	"Hide the main menu's Steam friends list and show a plain notice instead." );

// Stable per-session SteamID-to-serial mapping. Not persisted.
static CUtlMap< uint64, int > s_TFStreamerAliasSerials( DefLessFunc( uint64 ) );
static int s_nTFStreamerNextSerial = 1;

// Rotating buffers for nested display-name calls.
static const int TF_STREAMER_NAME_BUF_COUNT = 8;
static char s_szTFStreamerNameBufs[ TF_STREAMER_NAME_BUF_COUNT ][ MAX_PLAYER_NAME_LENGTH + 16 ];
static int s_nTFStreamerNameBufIndex = 0;

static char *TF_StreamerMode_NextBuf( void )
{
	char *p = s_szTFStreamerNameBufs[ s_nTFStreamerNameBufIndex ];
	s_nTFStreamerNameBufIndex = ( s_nTFStreamerNameBufIndex + 1 ) % TF_STREAMER_NAME_BUF_COUNT;
	return p;
}

//-----------------------------------------------------------------------------
bool TF_IsStreamerModeEnabled( void )
{
	return tf_streamer_mode.GetBool();
}

bool TF_ShouldAnonymisePlayerNames( void )	{ return TF_IsStreamerModeEnabled() && tf_streamer_mode_anonymise_names.GetBool(); }
bool TF_ShouldUseDefaultAvatar( void )		{ return TF_IsStreamerModeEnabled() && tf_streamer_mode_default_avatars.GetBool(); }
bool TF_ShouldHideChat( void )				{ return TF_IsStreamerModeEnabled() && tf_streamer_mode_hide_chat.GetBool(); }
bool TF_ShouldHidePartyChat( void )			{ return TF_IsStreamerModeEnabled() && tf_streamer_mode_hide_party_chat.GetBool(); }
bool TF_ShouldHideCallouts( void )			{ return TF_IsStreamerModeEnabled() && tf_streamer_mode_hide_callouts.GetBool(); }
bool TF_ShouldDisableVoice( void )			{ return TF_IsStreamerModeEnabled() && tf_streamer_mode_disable_voice.GetBool(); }
bool TF_ShouldHideServerInformation( void )	{ return TF_IsStreamerModeEnabled() && tf_streamer_mode_hide_server_info.GetBool(); }
bool TF_ShouldDisableDirectJoinPresence( void ) { return TF_IsStreamerModeEnabled() && tf_streamer_mode_disable_join_presence.GetBool(); }
bool TF_ShouldHideCustomItemText( uint32 unAccountID )
{
	if ( !TF_IsStreamerModeEnabled() || !tf_streamer_mode_hide_custom_item_text.GetBool() )
		return false;

	// Never hide your own items.
	if ( steamapicontext && steamapicontext->SteamUser() && unAccountID == steamapicontext->SteamUser()->GetSteamID().GetAccountID() )
		return false;

	return true;
}

bool TF_ShouldHidePlayerCreatedImages( int nPlayerIndex )
{
	if ( !TF_IsStreamerModeEnabled() || !tf_streamer_mode_hide_custom_images.GetBool() )
		return false;

	if ( nPlayerIndex == GetLocalPlayerIndex() )
		return false;

	return true;
}

bool TF_ShouldHideFriendsList( void )		{ return TF_IsStreamerModeEnabled() && tf_streamer_mode_hide_friends_list.GetBool(); }

bool TF_IsLocalAccountID( uint32 unAccountID )
{
	return steamapicontext && steamapicontext->SteamUser() &&
		unAccountID == steamapicontext->SteamUser()->GetSteamID().GetAccountID();
}

static bool TF_IsLocalPartyMember( const CSteamID &steamID )
{
	CTFParty *pParty = GTFGCClientSystem()->GetParty();
	return pParty && pParty->GetMemberIndexBySteamID( steamID ) != -1;
}

//-----------------------------------------------------------------------------
const char *TF_GetLocalStreamerDisplayName( void )
{
	const char *pszName = tf_streamer_mode_name.GetString();
	return ( pszName && pszName[0] ) ? pszName : "You";
}

//-----------------------------------------------------------------------------
static int TF_StreamerMode_GetOrAssignSerial( uint64 ullSteamID64 )
{
	int idx = s_TFStreamerAliasSerials.Find( ullSteamID64 );
	if ( s_TFStreamerAliasSerials.IsValidIndex( idx ) )
		return s_TFStreamerAliasSerials[ idx ];

	int nSerial = s_nTFStreamerNextSerial++;
	s_TFStreamerAliasSerials.Insert( ullSteamID64, nSerial );
	return nSerial;
}

// Placeholder pool. TF2's real bot names (tf_bot.cpp) are server-only.
static const char *TF_StreamerMode_GetBotAlias( int nSerial )
{
	static const char *s_szBotAliases[] =
	{
		"Blitzkrieg", "Boston", "Bruiser", "Buzzkill", "Chief", "Cobalt", "Crash",
		"Dakota", "Deadeye", "Digger", "Fitzgerald", "Flint", "Foxtrot", "Grim",
		"Grizzly", "Harvey", "Hawkins", "Hollis", "Hoss", "Jasper", "Kaiser",
		"Lucky", "Marlowe", "Maverick", "Mercer", "Mongoose", "Nomad", "Otis",
		"Pierce", "Ranger", "Sarge", "Shadow", "Sledge", "Slim", "Sparks",
		"Sully", "Tex", "Titan", "Tomahawk", "Wolf",
	};

	return s_szBotAliases[ nSerial % ARRAYSIZE( s_szBotAliases ) ];
}

static void TF_StreamerMode_FormatAlias( int nSerial, char *pszBuf, int nBufSize )
{
	switch ( tf_streamer_mode_name_style.GetInt() )
	{
	case TF_STREAMER_NAME_BOT_NAMES:
		V_snprintf( pszBuf, nBufSize, "%s", TF_StreamerMode_GetBotAlias( nSerial ) );
		break;

	case TF_STREAMER_NAME_PLAYER_NUMBERS:
	default:
		V_snprintf( pszBuf, nBufSize, "Player %d", nSerial );
		break;
	}
}

//-----------------------------------------------------------------------------
const char *TF_GetPlayerDisplayName( int nPlayerIndex )
{
	// GetPlayerInfo may be empty immediately after connect.
	const char *pszRealName = ( g_PR && nPlayerIndex >= 1 && nPlayerIndex <= MAX_PLAYERS )
		? g_PR->GetPlayerName( nPlayerIndex )
		: NULL;

	player_info_t info;
	bool bHaveInfo = engine->GetPlayerInfo( nPlayerIndex, &info );

	if ( ( !pszRealName || !pszRealName[0] ) && bHaveInfo && info.name[0] != '\0' )
	{
		pszRealName = info.name;
	}

	char *pszFilteredBuf = TF_StreamerMode_NextBuf();
	if ( pszRealName && pszRealName[0] )
	{
		V_strncpy( pszFilteredBuf, pszRealName, MAX_PLAYER_NAME_LENGTH + 16 );
		pszRealName = UTIL_GetFilteredPlayerName( nPlayerIndex, pszFilteredBuf );
	}
	else
	{
		pszRealName = NULL;
	}

	if ( !TF_ShouldAnonymisePlayerNames() )
	{
		return pszRealName ? pszRealName : "";
	}

	if ( nPlayerIndex == GetLocalPlayerIndex() )
	{
		return TF_GetLocalStreamerDisplayName();
	}

	char *pszBuf = TF_StreamerMode_NextBuf();
	int nBufSize = MAX_PLAYER_NAME_LENGTH + 16;

	bool bHaveSteamID = bHaveInfo && info.friendsID != 0;
	CSteamID steamID;
	if ( bHaveSteamID )
	{
		steamID = CSteamID( info.friendsID, 1, GetUniverse(), k_EAccountTypeIndividual );

		// Party members are exempt by default (tf_streamer_mode_anonymise_party_members).
		if ( !tf_streamer_mode_anonymise_party_members.GetBool() && TF_IsLocalPartyMember( steamID ) )
		{
			return pszRealName ? pszRealName : "";
		}
	}

	// Falls back to player index if no SteamID has resolved yet.
	int nSerial = bHaveSteamID ? TF_StreamerMode_GetOrAssignSerial( steamID.ConvertToUint64() ) : nPlayerIndex;

	TF_StreamerMode_FormatAlias( nSerial, pszBuf, nBufSize );

	return pszBuf;
}

//-----------------------------------------------------------------------------
const char *TF_GetPlayerDisplayName( const CSteamID &steamID, const char *pszFallbackRealName )
{
	if ( !TF_ShouldAnonymisePlayerNames() )
		return pszFallbackRealName ? pszFallbackRealName : "";

	if ( !tf_streamer_mode_anonymise_party_members.GetBool() && TF_IsLocalPartyMember( steamID ) )
		return pszFallbackRealName ? pszFallbackRealName : "";

	char *pszBuf = TF_StreamerMode_NextBuf();
	int nSerial = TF_StreamerMode_GetOrAssignSerial( steamID.ConvertToUint64() );
	TF_StreamerMode_FormatAlias( nSerial, pszBuf, MAX_PLAYER_NAME_LENGTH + 16 );
	return pszBuf;
}

//-----------------------------------------------------------------------------
const char *TF_GetPartyMemberDisplayName( const CSteamID &steamID, const char *pszFallbackRealName )
{
	if ( steamapicontext && steamapicontext->SteamUser() && steamID == steamapicontext->SteamUser()->GetSteamID() )
	{
		return TF_ShouldAnonymisePlayerNames() ? TF_GetLocalStreamerDisplayName() : pszFallbackRealName;
	}
	return TF_GetPlayerDisplayName( steamID, pszFallbackRealName );
}

//-----------------------------------------------------------------------------
void TF_StreamerMode_ResetSession( void )
{
	s_TFStreamerAliasSerials.RemoveAll();
	s_nTFStreamerNextSerial = 1;
}

static CSteamID TF_StreamerMode_FilterAvatarSteamID( CSteamID steamIDReal )
{
	if ( !TF_ShouldUseDefaultAvatar() )
		return steamIDReal;

	if ( steamapicontext && steamapicontext->SteamUser() &&
		 steamIDReal == steamapicontext->SteamUser()->GetSteamID() )
	{
		return steamIDReal;
	}

	if ( !tf_streamer_mode_anonymise_party_members.GetBool() && TF_IsLocalPartyMember( steamIDReal ) )
	{
		return steamIDReal;
	}

	// SetPlayer() falls back to the default avatar for an invalid SteamID.
	return k_steamIDNil;
}

namespace
{
	struct CTFStreamerModeAvatarHookInstaller
	{
		CTFStreamerModeAvatarHookInstaller()
		{
			g_pfnAvatarDisplaySteamIDFilter = TF_StreamerMode_FilterAvatarSteamID;
			g_pfnEconShouldHideCustomItemText = TF_ShouldHideCustomItemText;
		}
	};
	CTFStreamerModeAvatarHookInstaller s_TFStreamerModeAvatarHookInstaller;
}
