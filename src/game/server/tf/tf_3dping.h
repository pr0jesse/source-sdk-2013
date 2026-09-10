//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Server-authoritative handling of the tf_3dping 3D world ping system.
//
//=============================================================================

#ifndef TF_3DPING_H
#define TF_3DPING_H
#ifdef _WIN32
#pragma once
#endif

class CTFPlayer;
class CCommand;

// Entry point from CTFPlayer::ClientCommand for TF_3DPING_COMMAND_NAME ("tf_ping").
void TF3DPing_OnPlayerPingCommand( CTFPlayer *pSender, const CCommand &args );

#endif // TF_3DPING_H
