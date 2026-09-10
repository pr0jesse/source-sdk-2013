//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Client-side Streamer Mode display helpers.
//
//=============================================================================

#ifndef TF_STREAMER_MODE_H
#define TF_STREAMER_MODE_H
#ifdef _WIN32
#pragma once
#endif

class CSteamID;

enum ETFStreamerNameStyle
{
	TF_STREAMER_NAME_PLAYER_NUMBERS = 0,	// "Player 7"
	TF_STREAMER_NAME_BOT_NAMES,				// stable TF-bot-style alias per player

	TF_STREAMER_NAME_STYLE_COUNT
};

// Other TF_Should*() helpers already include this check.
bool TF_IsStreamerModeEnabled( void );

// Returns the real name when anonymisation is off. Local player uses
// TF_GetLocalStreamerDisplayName().
const char *TF_GetPlayerDisplayName( int nPlayerIndex );

// SteamID overload for contexts with no live player entity (lobby, party
// UI, mute menu).
const char *TF_GetPlayerDisplayName( const CSteamID &steamID, const char *pszFallbackRealName );

// Local player's configured display name (defaults to "You").
const char *TF_GetLocalStreamerDisplayName( void );

// Like TF_GetPlayerDisplayName(), but resolves the local player's own
// name when steamID matches them.
const char *TF_GetPartyMemberDisplayName( const CSteamID &steamID, const char *pszFallbackRealName );

bool TF_ShouldAnonymisePlayerNames( void );
bool TF_ShouldUseDefaultAvatar( void );
bool TF_ShouldHideChat( void );
// Independent of TF_ShouldHideChat().
bool TF_ShouldHidePartyChat( void );
// Independent of TF_ShouldHideChat(). Gameplay callouts stay visible by default.
bool TF_ShouldHideCallouts( void );
bool TF_ShouldDisableVoice( void );
bool TF_ShouldHideServerInformation( void );
bool TF_ShouldDisableDirectJoinPresence( void );
// unAccountID is the item's owning account. Your own is exempt.
bool TF_ShouldHideCustomItemText( uint32 unAccountID );
// nPlayerIndex is who placed the spray/image. Your own is exempt.
bool TF_ShouldHidePlayerCreatedImages( int nPlayerIndex );
// Works with no live player entity, unlike checking against a resolved CBasePlayer*.
bool TF_IsLocalAccountID( uint32 unAccountID );
// Hides the main menu's Steam friends list, replacing it with a notice.
bool TF_ShouldHideFriendsList( void );

// Call on disconnect / level shutdown so aliases don't persist across connections.
void TF_StreamerMode_ResetSession( void );

#endif // TF_STREAMER_MODE_H
