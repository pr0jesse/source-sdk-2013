//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Client-side rendering for the tf_3dping 3D world ping system.
//
//=============================================================================

#include "cbase.h"
#include "tf_hud_3dping.h"
#include "iclientmode.h"
#include "cdll_util.h"
#include "usermessages.h"
#include <vgui/ISurface.h>
#include "VGuiMatSurface/IMatSystemSurface.h"
#include "materialsystem/imaterial.h"
#include "view.h"
#include "engine/IEngineSound.h"
#include "c_tf_player.h"
#include <vgui_controls/Label.h>
#include <vgui_controls/PanelListPanel.h>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

using namespace vgui;

DECLARE_HUDELEMENT( CHud3DPing );

ConVar cl_3dping_enabled( TF_3DPING_ENABLED_CONVAR_NAME, "1", FCVAR_ARCHIVE,
	"Show 3D world pings placed by other players." );

ConVar cl_3dping_team_enabled( TF_3DPING_TEAM_CONVAR_NAME, "0", FCVAR_ARCHIVE | FCVAR_USERINFO,
	"Let your own 3D pings be seen by teammates outside your party." );

// Local receive toggle.
ConVar cl_3dping_team_receive( "cl_3dping_team_receive", "1", FCVAR_ARCHIVE,
	"See other teammates' 3D pings outside your party (if the sender and the server both allow team-wide pings)." );

#define TF_3DPING_CLICK_SOUND	"ui/buttonclick.wav"

ConVar cl_3dping_place_sound_volume( "cl_3dping_place_sound_volume", "100", FCVAR_ARCHIVE,
	"Volume of the click sound when you place a 3D ping. 0 = off, 100 = normal volume.", true, 0.0f, true, 100.0f );

enum ETF3DPingReceivedSoundMode
{
	TF_3DPING_RECEIVED_SOUND_OFF = 0,
	TF_3DPING_RECEIVED_SOUND_PARTY_ONLY,
	TF_3DPING_RECEIVED_SOUND_PARTY_AND_TEAM,
};

ConVar cl_3dping_received_sound_mode( "cl_3dping_received_sound_mode", "1", FCVAR_ARCHIVE,
	"Which other players' 3D pings play a sound: 0 = none, 1 = party members only, 2 = party and team.", true, 0.0f, true, 2.0f );

ConVar cl_3dping_received_sound_volume( "cl_3dping_received_sound_volume", "100", FCVAR_ARCHIVE,
	"Volume of the click sound for other players' 3D pings (party and/or team, per the mode above). 0 = off, 100 = normal volume.", true, 0.0f, true, 100.0f );

ConVar cl_3dping_color_default( "cl_3dping_color_default", "235 226 210", FCVAR_ARCHIVE,
	"RGB color (0-255 each, e.g. \"235 226 210\") of the plain world 3D ping." );

ConVar cl_3dping_color_friendly( "cl_3dping_color_friendly", "120 190 215", FCVAR_ARCHIVE,
	"RGB color (0-255 each) of the friendly player 3D ping." );

ConVar cl_3dping_color_building_friendly( "cl_3dping_color_building_friendly", "95 165 200", FCVAR_ARCHIVE,
	"RGB color (0-255 each) of the friendly building 3D ping." );

ConVar cl_3dping_color_enemy( "cl_3dping_color_enemy", "255 124 36", FCVAR_ARCHIVE,
	"RGB color (0-255 each) of the enemy player 3D ping." );

ConVar cl_3dping_color_building_enemy( "cl_3dping_color_building_enemy", "240 105 32", FCVAR_ARCHIVE,
	"RGB color (0-255 each) of the enemy building 3D ping." );

ConVar cl_3dping_color_health( "cl_3dping_color_health", "110 205 125", FCVAR_ARCHIVE,
	"RGB color (0-255 each) of the health pickup 3D ping." );

ConVar cl_3dping_color_ammo( "cl_3dping_color_ammo", "225 185 95", FCVAR_ARCHIVE,
	"RGB color (0-255 each) of the ammo pickup 3D ping." );

// Party/team color variation.
ConVar cl_3dping_party_hue_shift( "cl_3dping_party_hue_shift", "-20", FCVAR_ARCHIVE,
	"Hue shift for pings from party members, in degrees around the color wheel.", true, -45.0f, true, 45.0f );

ConVar cl_3dping_team_hue_shift( "cl_3dping_team_hue_shift", "20", FCVAR_ARCHIVE,
	"Hue shift for pings from teammates outside your party, in degrees around the color wheel.", true, -70.0f, true, 70.0f );

// Parses "R G B" or "R,G,B".
static Color TF_3DPing_ParseColorConVar( const ConVar &cvar )
{
	int r, g, b;
	if ( sscanf( cvar.GetString(), "%d , %d , %d", &r, &g, &b ) != 3 &&
		 sscanf( cvar.GetString(), "%d %d %d", &r, &g, &b ) != 3 )
	{
		return Color( 255, 255, 255, 255 );
	}

	return Color( clamp( r, 0, 255 ), clamp( g, 0, 255 ), clamp( b, 0, 255 ), 255 );
}

// Brightness nudge so the shift stays visible on a low-saturation color.
#define TF_3DPING_HUE_SHIFT_VALUE_NUDGE 0.12f

// Reduces hue rotation on highly saturated colors.
#define TF_3DPING_HUE_SHIFT_SATURATION_DAMPING 0.75f

static Color TF_3DPing_ApplyHueShift( Color colIn, float flHueShiftDegrees )
{
	if ( flHueShiftDegrees == 0.0f )
		return colIn;

	Vector vecRGB( colIn[0] / 255.0f, colIn[1] / 255.0f, colIn[2] / 255.0f );
	Vector vecHSV;
	RGBtoHSV( vecRGB, vecHSV );

	float flSaturation = ( vecHSV.x >= 0.0f ) ? vecHSV.y : 0.0f;
	if ( vecHSV.x >= 0.0f )
	{
		float flEffectiveShift = flHueShiftDegrees * ( 1.0f - flSaturation * TF_3DPING_HUE_SHIFT_SATURATION_DAMPING );
		vecHSV.x = fmodf( fmodf( vecHSV.x + flEffectiveShift, 360.0f ) + 360.0f, 360.0f );
	}
	else
	{
		vecHSV.x = 0.0f;
	}

	float flNudgeDir = ( vecHSV.z >= 0.5f ) ? -1.0f : 1.0f;
	float flNudgeWeight = 1.0f - flSaturation;
	vecHSV.z = clamp( vecHSV.z + flNudgeDir * TF_3DPING_HUE_SHIFT_VALUE_NUDGE * flNudgeWeight, 0.0f, 1.0f );

	HSVtoRGB( vecHSV, vecRGB );

	int r = clamp( (int)( vecRGB.x * 255.0f + 0.5f ), 0, 255 );
	int g = clamp( (int)( vecRGB.y * 255.0f + 0.5f ), 0, 255 );
	int b = clamp( (int)( vecRGB.z * 255.0f + 0.5f ), 0, 255 );
	return Color( r, g, b, colIn[3] );
}

static Color GetColorForIcon( ETF3DPingIcon eIcon )
{
	switch ( eIcon )
	{
	case TF_3DPING_ICON_FRIENDLY_PLAYER:
		return TF_3DPing_ParseColorConVar( cl_3dping_color_friendly );
	case TF_3DPING_ICON_FRIENDLY_BUILDING:
		return TF_3DPing_ParseColorConVar( cl_3dping_color_building_friendly );
	case TF_3DPING_ICON_ENEMY_PLAYER:
		return TF_3DPing_ParseColorConVar( cl_3dping_color_enemy );
	case TF_3DPING_ICON_ENEMY_BUILDING:
		return TF_3DPing_ParseColorConVar( cl_3dping_color_building_enemy );
	case TF_3DPING_ICON_PICKUP_HEALTH:
		return TF_3DPing_ParseColorConVar( cl_3dping_color_health );
	case TF_3DPING_ICON_PICKUP_AMMO:
		return TF_3DPing_ParseColorConVar( cl_3dping_color_ammo );
	case TF_3DPING_ICON_DEFAULT:
	default:
		return TF_3DPing_ParseColorConVar( cl_3dping_color_default );
	}
}

ConVar cl_3dping_color_border( "cl_3dping_color_border", "0 0 0", FCVAR_ARCHIVE,
	"RGB color (0-255 each) of the 3D ping icon borders. One shared color for every ping type." );

struct TF3DPingIconMaterials_t
{
	const char *m_pszFillPath;
	const char *m_pszBorderPath;
};

static const TF3DPingIconMaterials_t s_TF3DPingIconMaterials[ TF_3DPING_ICON_COUNT ] =
{
	{ "vgui/hud/3dping_default_fill",			"vgui/hud/3dping_default_border" },		// TF_3DPING_ICON_DEFAULT
	{ "vgui/hud/3dping_default_fill",			"vgui/hud/3dping_default_border" },		// TF_3DPING_ICON_FRIENDLY_PLAYER: reuses the default shape
	{ "vgui/hud/3dping_building_fill",			"vgui/hud/3dping_building_border" },		// TF_3DPING_ICON_FRIENDLY_BUILDING
	{ "vgui/hud/3dping_enemy_fill",				"vgui/hud/3dping_enemy_border" },			// TF_3DPING_ICON_ENEMY_PLAYER
	{ "vgui/hud/3dping_building_fill",			"vgui/hud/3dping_building_border" },		// TF_3DPING_ICON_ENEMY_BUILDING: shares the building shape
	{ "vgui/hud/3dping_pickup_health_fill",		"vgui/hud/3dping_pickup_health_border" },	// TF_3DPING_ICON_PICKUP_HEALTH
	{ "vgui/hud/3dping_pickup_ammo_fill",		"vgui/hud/3dping_pickup_ammo_border" },	// TF_3DPING_ICON_PICKUP_AMMO
};

// Half-size, in reference (640-wide) pixels, of the icon at close range.
#define TF_3DPING_ICON_HALFSIZE	7.5f

ConVar cl_3dping_icon_size( "cl_3dping_icon_size", "100", FCVAR_ARCHIVE,
	"Size of the 3D ping marker icon, as a percentage of the default size.", true, 50.0f, true, 400.0f );

ConVar cl_3dping_alpha( "cl_3dping_alpha", "100", FCVAR_ARCHIVE,
	"Overall opacity of the 3D ping marker, as a percentage (100 = fully opaque, 5 = 95% transparent).", true, 5.0f, true, 100.0f );

// Per-scope opacity, on top of cl_3dping_alpha.
ConVar cl_3dping_party_alpha( "cl_3dping_party_alpha", "100", FCVAR_ARCHIVE,
	"Opacity multiplier for pings from party members, on top of cl_3dping_alpha above.", true, 0.0f, true, 100.0f );

ConVar cl_3dping_team_alpha( "cl_3dping_team_alpha", "100", FCVAR_ARCHIVE,
	"Opacity multiplier for pings from teammates outside your party, on top of cl_3dping_alpha above.", true, 0.0f, true, 100.0f );

// Fade-out window.
#define TF_3DPING_LIFETIME_FADE_WINDOW	0.6f

// Spawn fade and size overshoot.
#define TF_3DPING_SPAWN_FADE_IN_WINDOW	0.15f
#define TF_3DPING_SPAWN_POP_SCALE		1.3f

#define TF_3DPING_ALPHA_FADE_START_DIST	500.0f
#define TF_3DPING_ALPHA_FADE_END_DIST		3200.0f

ConVar cl_3dping_min_opacity( "cl_3dping_min_opacity", "5", FCVAR_ARCHIVE,
	"The lowest opacity distance fade alone can bring a 3D ping down to, as a percentage (100 = fully opaque, 0 = fully transparent).", true, 0.0f, true, 100.0f );

#define TF_3DPING_SIZE_FADE_START_DIST	500.0f
#define TF_3DPING_SIZE_FADE_END_DIST	1600.0f
#define TF_3DPING_SIZE_MIN_SCALE		0.55f

ConVar cl_3dping_occluded_visibility( "cl_3dping_occluded_visibility", "25", FCVAR_ARCHIVE,
	"How opaque a 3D ping stays when there's no clear view of it (e.g. behind a wall or another player), as a percentage (100 = no extra dimming, 0 = as transparent as cl_3dping_min_opacity allows).", true, 0.0f, true, 100.0f );

#define TF_3DPING_OCCLUSION_FADE_RATE	8.0f

static CHud3DPing *s_pHud3DPing = NULL;

CHud3DPing *GetHud3DPing( void )
{
	return s_pHud3DPing;
}

// Left-to-right order for the strip below, independent of ETF3DPingIcon's grouping.
static const ETF3DPingIcon s_TF3DPingPreviewOrder[ TF_3DPING_PREVIEW_COUNT ] =
{
	TF_3DPING_ICON_DEFAULT,
	TF_3DPING_ICON_ENEMY_PLAYER,
	TF_3DPING_ICON_ENEMY_BUILDING,
	TF_3DPING_ICON_FRIENDLY_PLAYER,
	TF_3DPING_ICON_FRIENDLY_BUILDING,
	TF_3DPING_ICON_PICKUP_AMMO,
	TF_3DPING_ICON_PICKUP_HEALTH,
};

static const char *s_pszTF3DPingPreviewLabels[ TF_3DPING_PREVIEW_COUNT ] =
{
	"World", "Enemy", "Enemy Building", "Friendly", "Friendly Building", "Ammo", "Health",
};

// Used before the parent list has been laid out.
#define TF_3DPING_PREVIEW_FALLBACK_WIDE	350
#define TF_3DPING_PREVIEW_ICON_AREA_TALL	128
#define TF_3DPING_PREVIEW_LABEL_TALL		20
#define TF_3DPING_PREVIEW_ICON_HALFSIZE	60

//-----------------------------------------------------------------------------
CTF3DPingIconPreview::CTF3DPingIconPreview( vgui::Panel *pParent, const char *pElementName )
	: BaseClass( pParent, pElementName )
{
	SetPaintBackgroundEnabled( false );
	SetSize( TF_3DPING_PREVIEW_FALLBACK_WIDE, TF_3DPING_PREVIEW_ICON_AREA_TALL + TF_3DPING_PREVIEW_LABEL_TALL );

	for ( int i = 0; i < TF_3DPING_ICON_COUNT; i++ )
	{
		m_iFillTextureID[i] = -1;
		m_iBorderTextureID[i] = -1;
		m_bFillValid[i] = false;
		m_bBorderValid[i] = false;
	}

	for ( int i = 0; i < TF_3DPING_PREVIEW_COUNT; i++ )
	{
		m_pLabels[i] = new vgui::Label( this, "TF3DPingPreviewLabel", s_pszTF3DPingPreviewLabels[i] );
		m_pLabels[i]->SetContentAlignment( vgui::Label::a_north );
	}
}

// Preview remains available while ping rendering is disabled.
void CTF3DPingIconPreview::ApplySchemeSettings( vgui::IScheme *pScheme )
{
	BaseClass::ApplySchemeSettings( pScheme );

	vgui::HFont hLabelFont = pScheme->GetFont( "ScoreboardSmall", true );
	Color colLabel = pScheme->GetColor( "TanLight", Color( 235, 226, 210, 255 ) );

	for ( int i = 0; i < TF_3DPING_PREVIEW_COUNT; i++ )
	{
		m_pLabels[i]->SetFont( hLabelFont );
		m_pLabels[i]->SetFgColor( colLabel );
	}

	for ( int i = 0; i < TF_3DPING_ICON_COUNT; i++ )
	{
		if ( m_iFillTextureID[i] == -1 )
		{
			m_iFillTextureID[i] = vgui::surface()->CreateNewTextureID();
		}
		if ( m_iBorderTextureID[i] == -1 )
		{
			m_iBorderTextureID[i] = vgui::surface()->CreateNewTextureID();
		}

		IMaterial *pFillMaterial = materials->FindMaterial( s_TF3DPingIconMaterials[i].m_pszFillPath, TEXTURE_GROUP_VGUI, false );
		m_bFillValid[i] = !IsErrorMaterial( pFillMaterial );
		if ( m_bFillValid[i] && g_pMatSystemSurface )
		{
			g_pMatSystemSurface->DrawSetTextureMaterial( m_iFillTextureID[i], pFillMaterial );
		}

		IMaterial *pBorderMaterial = materials->FindMaterial( s_TF3DPingIconMaterials[i].m_pszBorderPath, TEXTURE_GROUP_VGUI, false );
		m_bBorderValid[i] = !IsErrorMaterial( pBorderMaterial );
		if ( m_bBorderValid[i] && g_pMatSystemSurface )
		{
			g_pMatSystemSurface->DrawSetTextureMaterial( m_iBorderTextureID[i], pBorderMaterial );
		}
	}
}

void CTF3DPingIconPreview::PerformLayout()
{
	BaseClass::PerformLayout();

	vgui::Panel *pParent = GetParent();
	int iParentWide = pParent ? pParent->GetWide() : 0;
	SetWide( iParentWide > 0 ? iParentWide : TF_3DPING_PREVIEW_FALLBACK_WIDE );

	int iColumnWide = GetWide() / TF_3DPING_PREVIEW_COUNT;

	for ( int i = 0; i < TF_3DPING_PREVIEW_COUNT; i++ )
	{
		m_pLabels[i]->SetBounds( iColumnWide * i, TF_3DPING_PREVIEW_ICON_AREA_TALL, iColumnWide, TF_3DPING_PREVIEW_LABEL_TALL );
	}
}

// Fixed preview size, ignores cl_3dping_icon_size.
void CTF3DPingIconPreview::Paint()
{
	int iColumnWide = GetWide() / TF_3DPING_PREVIEW_COUNT;
	int iCenterY = TF_3DPING_PREVIEW_ICON_AREA_TALL / 2;

	Color colBorder = TF_3DPing_ParseColorConVar( cl_3dping_color_border );
	colBorder[3] = 255;

	for ( int i = 0; i < TF_3DPING_PREVIEW_COUNT; i++ )
	{
		ETF3DPingIcon eIcon = s_TF3DPingPreviewOrder[i];
		int iCenterX = iColumnWide * i + iColumnWide / 2;

		Color colFill = GetColorForIcon( eIcon );
		colFill[3] = 255;

		if ( m_bFillValid[ eIcon ] )
		{
			surface()->DrawSetColor( colFill );
			surface()->DrawSetTexture( m_iFillTextureID[ eIcon ] );
			surface()->DrawTexturedRect( iCenterX - TF_3DPING_PREVIEW_ICON_HALFSIZE, iCenterY - TF_3DPING_PREVIEW_ICON_HALFSIZE,
				iCenterX + TF_3DPING_PREVIEW_ICON_HALFSIZE, iCenterY + TF_3DPING_PREVIEW_ICON_HALFSIZE );
		}

		if ( m_bBorderValid[ eIcon ] )
		{
			surface()->DrawSetColor( colBorder );
			surface()->DrawSetTexture( m_iBorderTextureID[ eIcon ] );
			surface()->DrawTexturedRect( iCenterX - TF_3DPING_PREVIEW_ICON_HALFSIZE, iCenterY - TF_3DPING_PREVIEW_ICON_HALFSIZE,
				iCenterX + TF_3DPING_PREVIEW_ICON_HALFSIZE, iCenterY + TF_3DPING_PREVIEW_ICON_HALFSIZE );
		}
	}
}

//-----------------------------------------------------------------------------
void TF3DPing_MaybeInsertPreviewRow( vgui::PanelListPanel *pList, const char *pszCategoryPrompt )
{
	if ( !pList || !pszCategoryPrompt || Q_stricmp( pszCategoryPrompt, "3D Ping" ) != 0 )
		return;

	CTF3DPingIconPreview *pPreview = new CTF3DPingIconPreview( pList, "TF3DPingIconPreview" );
	pList->AddItem( NULL, pPreview );
}

//-----------------------------------------------------------------------------
CHud3DPing::CHud3DPing( const char *pElementName )
	: CHudElement( pElementName ), BaseClass( NULL, "Hud3DPing" )
{
	vgui::Panel *pParent = g_pClientMode->GetViewport();
	SetParent( pParent );

	SetPaintBackgroundEnabled( false );
	SetMouseInputEnabled( false );
	SetKeyBoardInputEnabled( false );

	SetHiddenBits( HIDEHUD_MISCSTATUS );

	memset( m_Pings, 0, sizeof( m_Pings ) );
	memset( m_DyingPings, 0, sizeof( m_DyingPings ) );

	for ( int i = 0; i < TF_3DPING_ICON_COUNT; i++ )
	{
		m_iFillTextureID[i] = -1;
		m_iBorderTextureID[i] = -1;
		m_bFillValid[i] = false;
		m_bBorderValid[i] = false;
	}

	m_bWasStealthed = false;
	m_flLastUnstealthTime = 0.0f;
	m_iLastObservedTeam = TEAM_UNASSIGNED;

	s_pHud3DPing = this;
}

//-----------------------------------------------------------------------------
CHud3DPing::~CHud3DPing()
{
	if ( s_pHud3DPing == this )
	{
		s_pHud3DPing = NULL;
	}
}

// Re-resolves materials on each scheme reload.
void CHud3DPing::LoadIcons( void )
{
	for ( int i = 0; i < TF_3DPING_ICON_COUNT; i++ )
	{
		if ( m_iFillTextureID[i] == -1 )
		{
			m_iFillTextureID[i] = vgui::surface()->CreateNewTextureID();
		}
		if ( m_iBorderTextureID[i] == -1 )
		{
			m_iBorderTextureID[i] = vgui::surface()->CreateNewTextureID();
		}

		IMaterial *pFillMaterial = materials->FindMaterial( s_TF3DPingIconMaterials[i].m_pszFillPath, TEXTURE_GROUP_VGUI, false );
		m_bFillValid[i] = !IsErrorMaterial( pFillMaterial );
		if ( m_bFillValid[i] && g_pMatSystemSurface )
		{
			g_pMatSystemSurface->DrawSetTextureMaterial( m_iFillTextureID[i], pFillMaterial );
		}

		IMaterial *pBorderMaterial = materials->FindMaterial( s_TF3DPingIconMaterials[i].m_pszBorderPath, TEXTURE_GROUP_VGUI, false );
		m_bBorderValid[i] = !IsErrorMaterial( pBorderMaterial );
		if ( m_bBorderValid[i] && g_pMatSystemSurface )
		{
			g_pMatSystemSurface->DrawSetTextureMaterial( m_iBorderTextureID[i], pBorderMaterial );
		}
	}
}

//-----------------------------------------------------------------------------
void CHud3DPing::ApplySchemeSettings( vgui::IScheme *pScheme )
{
	BaseClass::ApplySchemeSettings( pScheme );

	SetBounds( 0, 0, ScreenWidth(), ScreenHeight() );

	LoadIcons();
}

//-----------------------------------------------------------------------------
bool CHud3DPing::ShouldDraw( void )
{
	if ( !cl_3dping_enabled.GetBool() )
		return false;

	return CHudElement::ShouldDraw();
}

// Runs every frame regardless of ShouldDraw()/cl_3dping_enabled, so the
// post-cloak lockout timer keeps working even with pings hidden.
void CHud3DPing::OnThink( void )
{
	BaseClass::OnThink();

	C_TFPlayer *pLocalPlayer = C_TFPlayer::GetLocalTFPlayer();
	bool bStealthed = pLocalPlayer && pLocalPlayer->m_Shared.IsStealthed();

	if ( m_bWasStealthed && !bStealthed )
	{
		m_flLastUnstealthTime = gpGlobals->curtime;
	}
	m_bWasStealthed = bStealthed;

	// Drop markers on an observed-team switch. They were scoped to the team just left.
	if ( pLocalPlayer && pLocalPlayer->GetTeamNumber() == TEAM_SPECTATOR )
	{
		CBaseEntity *pObserverTarget = pLocalPlayer->GetObserverTarget();
		int iObservedTeam = pObserverTarget ? pObserverTarget->GetTeamNumber() : TEAM_UNASSIGNED;

		if ( m_iLastObservedTeam != TEAM_UNASSIGNED && iObservedTeam != m_iLastObservedTeam )
		{
			memset( m_Pings, 0, sizeof( m_Pings ) );
			memset( m_DyingPings, 0, sizeof( m_DyingPings ) );
		}

		m_iLastObservedTeam = iObservedTeam;
	}
	else
	{
		m_iLastObservedTeam = TEAM_UNASSIGNED;
	}
}

// True while cloaked or still inside the post-cloak lockout window.
bool CHud3DPing::IsLocalPlayerStealthLocked( void ) const
{
	C_TFPlayer *pLocalPlayer = C_TFPlayer::GetLocalTFPlayer();
	if ( !pLocalPlayer )
		return false;

	if ( pLocalPlayer->m_Shared.IsStealthed() )
		return true;

	return ( gpGlobals->curtime - m_flLastUnstealthTime ) < TF_3DPING_POST_CLOAK_LOCKOUT;
}

//-----------------------------------------------------------------------------
void CHud3DPing::OnPingReceived( ETF3DPingType eType, int iSenderEntIndex, const Vector &vecPosition, float flLifetime, bool bHostile, bool bFromPartyScope )
{
	if ( iSenderEntIndex < 1 || iSenderEntIndex > MAX_PLAYERS )
		return;

	// Team-wide pings (not party, not your own) are locally opt-out via
	// cl_3dping_team_receive.
	const bool bIsSelfPing = ( iSenderEntIndex == GetLocalPlayerIndex() );
	if ( !bFromPartyScope && !bIsSelfPing && !cl_3dping_team_receive.GetBool() )
		return;

	// One live marker per sender. The one it replaces plays a quick
	// fade-out via m_DyingPings instead of just vanishing.
	if ( m_Pings[ iSenderEntIndex ].bActive )
	{
		ActivePing_t &dying = m_DyingPings[ iSenderEntIndex ];
		dying = m_Pings[ iSenderEntIndex ];
		dying.flExpireTime = gpGlobals->curtime;	// repurposed: when this dying copy started fading
	}

	ActivePing_t &ping = m_Pings[ iSenderEntIndex ];
	ping.bActive = true;
	ping.eType = eType;
	ping.iSenderEntIndex = iSenderEntIndex;
	ping.vecPosition = vecPosition;
	ping.flSpawnTime = gpGlobals->curtime;
	ping.flExpireTime = gpGlobals->curtime + flLifetime;
	ping.bHostile = bHostile;
	ping.bFromPartyScope = bFromPartyScope;
	ping.flOcclusionAmount = 0.0f;	// don't inherit whatever this slot's previous ping last faded to

	// A self-received ping confirms it passed server-side validation.
	const bool bIsSelf = bIsSelfPing;

	int iVolumePercent;
	if ( bIsSelf )
	{
		iVolumePercent = cl_3dping_place_sound_volume.GetInt();
	}
	else
	{
		bool bModeAllows = bFromPartyScope
			? ( cl_3dping_received_sound_mode.GetInt() >= TF_3DPING_RECEIVED_SOUND_PARTY_ONLY )
			: ( cl_3dping_received_sound_mode.GetInt() >= TF_3DPING_RECEIVED_SOUND_PARTY_AND_TEAM );
		iVolumePercent = bModeAllows ? cl_3dping_received_sound_volume.GetInt() : 0;
	}

	if ( iVolumePercent > 0 )
	{
		enginesound->EmitAmbientSound( TF_3DPING_CLICK_SOUND, iVolumePercent / 100.0f );
	}
}

// bHostile is resolved server-side. This only maps it to an icon.
ETF3DPingIcon CHud3DPing::GetIconForPing( const ActivePing_t &ping ) const
{
	switch ( ping.eType )
	{
	case TF_3DPING_PICKUP_HEALTH:
		return TF_3DPING_ICON_PICKUP_HEALTH;
	case TF_3DPING_PICKUP_AMMO:
		return TF_3DPING_ICON_PICKUP_AMMO;
	case TF_3DPING_BUILDING:
		return ping.bHostile ? TF_3DPING_ICON_ENEMY_BUILDING : TF_3DPING_ICON_FRIENDLY_BUILDING;
	case TF_3DPING_PLAYER:
		return ping.bHostile ? TF_3DPING_ICON_ENEMY_PLAYER : TF_3DPING_ICON_FRIENDLY_PLAYER;
	case TF_3DPING_WORLD:
	default:
		return TF_3DPING_ICON_DEFAULT;
	}
}

//-----------------------------------------------------------------------------
void CHud3DPing::Paint( void )
{
	for ( int i = 1; i <= MAX_PLAYERS; i++ )
	{
		ActivePing_t &dying = m_DyingPings[i];
		if ( dying.bActive )
		{
			// flExpireTime is repurposed here as the fade start time.
			if ( gpGlobals->curtime >= dying.flExpireTime + TF_3DPING_SPAWN_FADE_IN_WINDOW )
			{
				dying.bActive = false;
			}
			else
			{
				DrawPing( dying, true );
			}
		}

		ActivePing_t &ping = m_Pings[i];
		if ( !ping.bActive )
			continue;

		if ( gpGlobals->curtime >= ping.flExpireTime )
		{
			ping.bActive = false;
			continue;
		}

		DrawPing( ping, false );
	}
}

// MASK_OPAQUE ignores players. Test player bodies separately so a
// teammate standing in the way still occludes the ping.
static bool TF3DPing_PlayerBlocksLine( const Vector &vecStart, const Vector &vecTarget )
{
	C_TFPlayer *pLocalPlayer = C_TFPlayer::GetLocalTFPlayer();

	Vector vecDir = vecTarget - vecStart;
	float flRayLength = VectorNormalize( vecDir );
	if ( flRayLength <= 0.0f )
		return false;

	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		C_TFPlayer *pPlayer = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( !pPlayer || pPlayer == pLocalPlayer || !pPlayer->IsAlive() )
			continue;

		// A cloaked Spy has no visible silhouette to occlude against.
		if ( pPlayer->m_Shared.IsStealthed() )
			continue;

		const Vector &vecCenter = pPlayer->WorldSpaceCenter();

		// Likely the ping's own target (a player-type ping), not an occluder.
		if ( vecCenter.DistToSqr( vecTarget ) < ( 48.0f * 48.0f ) )
			continue;

		Vector vecToPlayer = vecCenter - vecStart;
		float flAlongRay = DotProduct( vecToPlayer, vecDir );

		if ( flAlongRay <= 0.0f || flAlongRay >= flRayLength )
			continue;

		Vector vecClosestPointOnRay = vecStart + vecDir * flAlongRay;
		float flRadius = pPlayer->BoundingRadius();
		if ( vecClosestPointOnRay.DistToSqr( vecCenter ) <= flRadius * flRadius )
			return true;
	}

	return false;
}

// bDying: a marker just replaced by a new ping from the same sender.
void CHud3DPing::DrawPing( ActivePing_t &ping, bool bDying )
{
	int iX, iY;
	// Off-screen pings are not drawn.
	if ( !GetVectorInHudSpace( ping.vecPosition, iX, iY ) )
		return;

	float flTimeFadeScale;
	float flSpawnPopScale = 1.0f;

	if ( bDying )
	{
		float flTimeSinceDeath = gpGlobals->curtime - ping.flExpireTime;
		flTimeFadeScale = RemapValClamped( flTimeSinceDeath, 0.0f, TF_3DPING_SPAWN_FADE_IN_WINDOW, 1.0f, 0.0f );
	}
	else
	{
		float flTimeLeft = ping.flExpireTime - gpGlobals->curtime;
		float flLifetimeScale = ( flTimeLeft < TF_3DPING_LIFETIME_FADE_WINDOW )
			? RemapValClamped( flTimeLeft, 0.0f, TF_3DPING_LIFETIME_FADE_WINDOW, 0.0f, 1.0f )
			: 1.0f;

		float flTimeSinceSpawn = gpGlobals->curtime - ping.flSpawnTime;
		float flSpawnFadeInScale = ( flTimeSinceSpawn < TF_3DPING_SPAWN_FADE_IN_WINDOW )
			? RemapValClamped( flTimeSinceSpawn, 0.0f, TF_3DPING_SPAWN_FADE_IN_WINDOW, 0.0f, 1.0f )
			: 1.0f;

		flTimeFadeScale = flLifetimeScale * flSpawnFadeInScale;

		flSpawnPopScale = ( flTimeSinceSpawn < TF_3DPING_SPAWN_FADE_IN_WINDOW )
			? RemapValClamped( flTimeSinceSpawn, 0.0f, TF_3DPING_SPAWN_FADE_IN_WINDOW, TF_3DPING_SPAWN_POP_SCALE, 1.0f )
			: 1.0f;
	}

	Vector vecViewOrigin = MainViewOrigin();
	float flDist = ( ping.vecPosition - vecViewOrigin ).Length();
	float flMinOpacityScale = cl_3dping_min_opacity.GetFloat() / 100.0f;
	float flDistScale = RemapValClamped( flDist, TF_3DPING_ALPHA_FADE_START_DIST, TF_3DPING_ALPHA_FADE_END_DIST, 1.0f, flMinOpacityScale );

	// MASK_OPAQUE ignores players. Test player bodies separately.
	trace_t tr;
	UTIL_TraceLine( vecViewOrigin, ping.vecPosition, MASK_OPAQUE, NULL, COLLISION_GROUP_NONE, &tr );
	float flOcclusionTarget = RemapValClamped( tr.fraction, 0.85f, 0.98f, 1.0f, 0.0f );

	if ( TF3DPing_PlayerBlocksLine( vecViewOrigin, ping.vecPosition ) )
	{
		flOcclusionTarget = 1.0f;
	}

	// Eases toward this frame's result instead of snapping.
	ping.flOcclusionAmount = Approach( flOcclusionTarget, ping.flOcclusionAmount, TF_3DPING_OCCLUSION_FADE_RATE * gpGlobals->frametime );

	// Subtracts from distance alpha, not time alpha, so a ping doesn't
	// vanish in one frame at end of life.
	float flDistOcclusionAlpha = flDistScale * ( cl_3dping_alpha.GetFloat() / 100.0f );
	float flOcclusionReduction = ( 1.0f - ( cl_3dping_occluded_visibility.GetFloat() / 100.0f ) ) * ping.flOcclusionAmount;
	float flFlooredDistOcclusionAlpha = clamp( flDistOcclusionAlpha - flOcclusionReduction, flMinOpacityScale, 1.0f );
	float flFinalAlphaScale = flTimeFadeScale * flFlooredDistOcclusionAlpha;

	if ( ping.iSenderEntIndex != GetLocalPlayerIndex() )
	{
		float flScopeAlpha = ping.bFromPartyScope ? cl_3dping_party_alpha.GetFloat() : cl_3dping_team_alpha.GetFloat();
		flFinalAlphaScale *= ( flScopeAlpha / 100.0f );
	}

	int iAlpha = (int)( 255.0f * flFinalAlphaScale );

	ETF3DPingIcon eIcon = GetIconForPing( ping );

	float flSizeScale = RemapValClamped( flDist, TF_3DPING_SIZE_FADE_START_DIST, TF_3DPING_SIZE_FADE_END_DIST, 1.0f, TF_3DPING_SIZE_MIN_SCALE );

	float flUserSizeScale = cl_3dping_icon_size.GetFloat() / 100.0f;
	int iHalfSize = (int)( YRES( TF_3DPING_ICON_HALFSIZE ) * flSizeScale * flSpawnPopScale * flUserSizeScale );
	if ( iHalfSize < 2 )
		iHalfSize = 2;

	Color colFillTint = GetColorForIcon( eIcon );

	if ( ping.iSenderEntIndex != GetLocalPlayerIndex() )
	{
		float flHueShift = ping.bFromPartyScope ? cl_3dping_party_hue_shift.GetFloat() : cl_3dping_team_hue_shift.GetFloat();
		colFillTint = TF_3DPing_ApplyHueShift( colFillTint, flHueShift );
	}

	colFillTint[3] = iAlpha;

	if ( m_bFillValid[ eIcon ] )
	{
		surface()->DrawSetColor( colFillTint );
		surface()->DrawSetTexture( m_iFillTextureID[ eIcon ] );
		surface()->DrawTexturedRect( iX - iHalfSize, iY - iHalfSize, iX + iHalfSize, iY + iHalfSize );
	}
	else
	{
		// Placeholder marker if the fill art is missing.
		surface()->DrawSetColor( colFillTint );
		surface()->DrawOutlinedCircle( iX, iY, iHalfSize, 16 );
		surface()->DrawFilledRect( iX - 1, iY - 1, iX + 1, iY + 1 );
	}

	if ( m_bBorderValid[ eIcon ] )
	{
		Color colBorderTint = TF_3DPing_ParseColorConVar( cl_3dping_color_border );
		colBorderTint[3] = iAlpha;

		surface()->DrawSetColor( colBorderTint );
		surface()->DrawSetTexture( m_iBorderTextureID[ eIcon ] );
		surface()->DrawTexturedRect( iX - iHalfSize, iY - iHalfSize, iX + iHalfSize, iY + iHalfSize );
	}
}

USER_MESSAGE( TF3DPing )
{
	int nType = msg.ReadByte();
	int iSenderEntIndex = msg.ReadByte();

	Vector vecPosition;
	msg.ReadBitVec3Coord( vecPosition );

	float flLifetime = msg.ReadFloat();
	bool bHostile = msg.ReadByte() != 0;
	bool bFromPartyScope = msg.ReadByte() != 0;

	if ( GetHud3DPing() )
	{
		GetHud3DPing()->OnPingReceived( (ETF3DPingType)nType, iSenderEntIndex, vecPosition, flLifetime, bHostile, bFromPartyScope );
	}
}

// Local pre-check only. The server (TF3DPing_OnPlayerPingCommand) re-checks.
CON_COMMAND( tf_ping, "Place a 3D ping at whatever you're aiming at." )
{
	C_TFPlayer *pLocalPlayer = C_TFPlayer::GetLocalTFPlayer();
	if ( !pLocalPlayer )
		return;

	if ( GetHud3DPing() && GetHud3DPing()->IsLocalPlayerStealthLocked() )
		return;

	if ( pLocalPlayer->m_Shared.InCond( TF_COND_DISGUISED ) &&
		pLocalPlayer->m_Shared.GetDisguiseTeam() != pLocalPlayer->GetTeamNumber() )
	{
		return;
	}

	engine->ServerCmd( TF_3DPING_COMMAND_NAME );
}
