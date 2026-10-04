//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================

#define AARON_WEAPON_TEAPOTMUG_H
/*#ifndef AARON_WEAPON_TEAPOTMUG_H
#ifdef _WIN32
#pragma once
#endif

#include "tf_weaponbase_melee.h"

#ifdef CLIENT_DLL
#define CTFAARONTeapotMug C_AARONTeapotMug
#endif

class CTFAARONTeapotMug : public CTFWeaponBaseMelee
{
public:
    DECLARE_CLASS( CTFAARONTeapotMug, CTFWeaponBaseMelee );
    DECLARE_NETWORKCLASS(); 
    DECLARE_PREDICTABLE();

    CTFAARONTeapotMug();
    virtual int GetWeaponID( void ) const { return AARON_WEAPON_TEAPOTMUG; }

    virtual void    ItemPreFrame( void ) OVERRIDE;
    virtual void    OnEntityHit( CBaseEntity *pEntity, CTakeDamageInfo &info ) OVERRIDE;

private:
    CTFAARONTeapotMug( const CTFAARONTeapotMug & ) {}
    // Networking
    CNetworkVar(float, m_flChargeMeter);
    CNetworkVar(int, m_iStoredCrits);
};

#endif
*/