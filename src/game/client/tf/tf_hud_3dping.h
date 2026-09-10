//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Client-side rendering for the tf_3dping 3D world ping system.
//
//=============================================================================

#ifndef TF_HUD_3DPING_H
#define TF_HUD_3DPING_H
#ifdef _WIN32
#pragma once
#endif

#include "hudelement.h"
#include <vgui_controls/Panel.h>
#include "tf_3dping_shared.h"

// Separate from ETF3DPingType: icon selection only cares about
// friend/foe/resource, plus player vs. building.
enum ETF3DPingIcon
{
	TF_3DPING_ICON_DEFAULT = 0,
	TF_3DPING_ICON_FRIENDLY_PLAYER,
	TF_3DPING_ICON_FRIENDLY_BUILDING,
	TF_3DPING_ICON_ENEMY_PLAYER,
	TF_3DPING_ICON_ENEMY_BUILDING,
	TF_3DPING_ICON_PICKUP_HEALTH,
	TF_3DPING_ICON_PICKUP_AMMO,

	TF_3DPING_ICON_COUNT
};

class CHud3DPing : public CHudElement, public vgui::Panel
{
	DECLARE_CLASS_SIMPLE( CHud3DPing, vgui::Panel );

public:
	CHud3DPing( const char *pElementName );
	virtual ~CHud3DPing();

	virtual void ApplySchemeSettings( vgui::IScheme *pScheme );
	virtual bool ShouldDraw( void );
	virtual void Paint( void );
	virtual void OnThink( void );

	// True while cloaked or still inside the post-cloak lockout window.
	bool IsLocalPlayerStealthLocked( void ) const;

	// bFromPartyScope reflects which of the (up to two) server-sent
	// messages this came from, and picks the notification sound.
	void OnPingReceived( ETF3DPingType eType, int iSenderEntIndex, const Vector &vecPosition, float flLifetime, bool bHostile, bool bFromPartyScope );

private:
	struct ActivePing_t
	{
		bool				bActive;
		ETF3DPingType		eType;
		int					iSenderEntIndex;
		Vector				vecPosition;
		float				flSpawnTime;
		float				flExpireTime;
		bool				bHostile;			// selects friendly/enemy icon
		bool				bFromPartyScope;
		float				flOcclusionAmount;
	};

	void LoadIcons( void );
	ETF3DPingIcon GetIconForPing( const ActivePing_t &ping ) const;
	void DrawPing( ActivePing_t &ping, bool bDying );

	ActivePing_t	m_Pings[ MAX_PLAYERS + 1 ];

	// bActive here means still fading out. flExpireTime is reused as the
	// fade start time.
	ActivePing_t	m_DyingPings[ MAX_PLAYERS + 1 ];

	int				m_iFillTextureID[ TF_3DPING_ICON_COUNT ];
	int				m_iBorderTextureID[ TF_3DPING_ICON_COUNT ];
	bool			m_bFillValid[ TF_3DPING_ICON_COUNT ];
	bool			m_bBorderValid[ TF_3DPING_ICON_COUNT ];

	bool			m_bWasStealthed;
	float			m_flLastUnstealthTime;

	// TEAM_UNASSIGNED means not currently spectating.
	int				m_iLastObservedTeam;
};

CHud3DPing *GetHud3DPing( void );

namespace vgui { class PanelListPanel; class Label; }

#define TF_3DPING_PREVIEW_COUNT	7

// Fixed-size strip of all 7 ping icons with their live colors, shown above
// the 3D Ping settings. Reuses the same materials/color ConVars as the HUD.
class CTF3DPingIconPreview : public vgui::Panel
{
	DECLARE_CLASS_SIMPLE( CTF3DPingIconPreview, vgui::Panel );

public:
	CTF3DPingIconPreview( vgui::Panel *pParent, const char *pElementName );

	virtual void ApplySchemeSettings( vgui::IScheme *pScheme );
	virtual void PerformLayout();
	virtual void Paint();

private:
	int				m_iFillTextureID[ TF_3DPING_ICON_COUNT ];
	int				m_iBorderTextureID[ TF_3DPING_ICON_COUNT ];
	bool			m_bFillValid[ TF_3DPING_ICON_COUNT ];
	bool			m_bBorderValid[ TF_3DPING_ICON_COUNT ];
	vgui::Label		*m_pLabels[ TF_3DPING_PREVIEW_COUNT ];
};

// Adds the preview strip as the first row of pList when pszCategoryPrompt
// is the 3D Ping category. No-op for any other category.
void TF3DPing_MaybeInsertPreviewRow( vgui::PanelListPanel *pList, const char *pszCategoryPrompt );

#endif // TF_HUD_3DPING_H
