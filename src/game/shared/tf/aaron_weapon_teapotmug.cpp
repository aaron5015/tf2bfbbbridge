//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================

#include "cbase.h"
/*#include "aaron_weapon_teapotmug.h"

// Networking / Registration
IMPLEMENT_NETWORKCLASS_ALIASED( AARONTeapotMug, DT_AARONTeapotMug )

BEGIN_NETWORK_TABLE( CTFAARONTeapotMug, DT_AARONTeapotMug )
	SendPropFloat( SENDINFO( m_flChargeMeter ) ),
	SendPropInt( SENDINFO( m_iStoredCrits ) ),
END_NETWORK_TABLE()

BEGIN_PREDICTION_DATA( CTFAARONTeapotMug )
	DEFINE_PRED_FIELD( m_flChargeMeter, FIELD_FLOAT, FTYPEDESC_INSENDTABLE ),
	DEFINE_PRED_FIELD( m_iStoredCrits, FIELD_INTEGER, FTYPEDESC_INSENDTABLE ),
END_PREDICTION_DATA()

LINK_ENTITY_TO_CLASS( tf_weapon_aaron_teapotmug, CTFAARONTeapotMug );
PRECACHE_WEAPON_REGISTER( tf_weapon_aaron_teapotmug );

// Implementation
CTFAARONTeapotMug::CTFAARONTeapotMug()
{
	m_flChargeMeter = 0.0f;
	m_iStoredCrits = 0;
}

void CTFAARONTeapotMug::ItemPreFrame( void )
{
	BaseClass::ItemPreFrame();

#ifndef CLIENT_DLL
	CTFPlayer *pOwner = ToTFPlayer( GetOwner() );
	if ( pOwner && pOwner->IsAlive() && pOwner->GetAbsVelocity().Length() > 10.0f )
	{
		m_flChargeMeter = MIN( m_flChargeMeter + ( 5.0f * gpGlobals->frametime ), 100.0f );
        
		if ( m_flChargeMeter >= 100.0f && m_iStoredCrits == 0 )
			m_iStoredCrits = 6;
	}
#endif
}

void CTFAARONTeapotMug::OnEntityHit( CBaseEntity *pEntity, CTakeDamageInfo &info )
{
	// Ensure we use the correct signature matching CTFWeaponBaseMelee
#ifndef CLIENT_DLL
	if ( m_iStoredCrits > 0 )
	{
		info.SetDamageType( info.GetDamageType() | DMG_CRITICAL );
		m_iStoredCrits--;
		if ( m_iStoredCrits <= 0 )
			m_flChargeMeter = 0.0f;
	}
#endif
	BaseClass::OnEntityHit( pEntity, info );
}
*/