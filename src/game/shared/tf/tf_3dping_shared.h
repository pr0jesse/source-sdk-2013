//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Shared definitions for the tf_3dping 3D world ping system.
//
//=============================================================================

#ifndef TF_3DPING_SHARED_H
#define TF_3DPING_SHARED_H
#ifdef _WIN32
#pragma once
#endif

#define TF_3DPING_COMMAND_NAME		"tf_ping"
#define TF_3DPING_USERMSG_NAME		"TF3DPing"

// Replicated so the server can read the sender's own choice. Never trusts
// a client-declared recipient list.
#define TF_3DPING_TEAM_CONVAR_NAME	"cl_3dping_team_enabled"

// Client-only, local display preference.
#define TF_3DPING_ENABLED_CONVAR_NAME	"cl_3dping_enabled"

enum ETF3DPingType
{
	TF_3DPING_WORLD = 0,			// world/location marker
	TF_3DPING_PLAYER,				// player position at ping time, not tracked afterward
	TF_3DPING_BUILDING,			// Engineer building
	TF_3DPING_PICKUP_HEALTH,		// health kit, always neutral
	TF_3DPING_PICKUP_AMMO,			// ammo pack, always neutral

	TF_3DPING_TYPE_COUNT
};

#define TF_3DPING_DEFAULT_COOLDOWN	"0.2"		// debounce only, see tf_3dping_burst_* for the real rate limit
#define TF_3DPING_DEFAULT_LIFETIME	"5.0"

// How long after uncloaking a Spy still can't ping. Hardcoded so it can't be
// shortened away as a cheat. Shared so the client gate and server check agree.
#define TF_3DPING_POST_CLOAK_LOCKOUT	2.0f

#endif // TF_3DPING_SHARED_H
