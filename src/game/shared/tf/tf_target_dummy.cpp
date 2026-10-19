//========= Copyright Valve Corporation, All rights reserved. ============//
//
// TF Pumpkin Bomb
//
//=============================================================================
#include "cbase.h"

#include "particle_parse.h"
#include "tf_player.h"
#include "tf_gamerules.h"
#include "tf_target_dummy.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"
#include "tf_ammo_pack.h"

#define DUMMY_MODEL "models/props_training/target_engineer.mdl"
#define DUMMY_DEMO_MODEL "models/props_training/target_demoman.mdl"
//#define TEAM_PUMPKIN "models/props_halloween/pumpkin_explode_teamcolor.mdl"

LINK_ENTITY_TO_CLASS( tf_target_dummy, CTFTargetDummy);

IMPLEMENT_AUTO_LIST( ITFTargetDummy );

ConVar tf_target_dummy_health( "tf_target_dummy_health", "10000", FCVAR_GAMEDLL | FCVAR_DEVELOPMENTONLY ); // Debug target health

#ifdef GAME_DLL
extern ConVar tf_obj_gib_velocity_min;
extern ConVar tf_obj_gib_velocity_max;
extern ConVar tf_obj_gib_maxspeed;
#endif


//-----------------------------------------------------------------------------
// Static
CTFTargetDummy* CTFTargetDummy::Create( const Vector& vPosition, const QAngle& qAngles, CTFPlayer *pOwner )
{
	QAngle qProper = qAngles;
	qProper.z = 0;
	qProper.x = 0;
	CTFTargetDummy *pDummy = assert_cast<CTFTargetDummy*>( CBaseEntity::Create( "tf_target_dummy", vPosition, qProper, pOwner ) );

	if ( pDummy && pOwner )
	{
		pDummy->ChangeTeam( pOwner->GetTeamNumber() );
		pDummy->m_nSkin = pOwner->GetTeamNumber() == TF_TEAM_BLUE ? 1 : 2;
	}

	return pDummy;
}

//-----------------------------------------------------------------------------
CTFTargetDummy::CTFTargetDummy()
{
}
//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void CTFTargetDummy::Precache( void )
{
	PrecacheModel( DUMMY_MODEL );
	int iModel = PrecacheModel( DUMMY_DEMO_MODEL );
	PrecacheGibsForModel( iModel );

	PrecacheParticleSystem( "target_break" );
	PrecacheParticleSystem( "lowV_debrischunks" );
	BaseClass::Precache();
}
//-----------------------------------------------------------------------------
void CTFTargetDummy::Spawn()
{
	Precache();

	m_aGibs.Purge();
	// TODO: Model Selection
	if ( RandomInt( 0, 1 ) == 1 )
	{
		SetModel( DUMMY_DEMO_MODEL );
		BuildGibList( m_aGibs, GetModelIndex(), 1.0f, COLLISION_GROUP_NONE );
	}
	else
	{
		SetModel( DUMMY_MODEL );
	}
	// Set Team
	BaseClass::Spawn();

	SetMoveType( MOVETYPE_NONE );
	// Use explicit bbox collision: the training target models are not guaranteed
	// to provide a usable VPhysics collision mesh for weapon traces.
	SetSolid( SOLID_BBOX );
	SetCollisionBounds( Vector( -16, -16, 0 ), Vector( 16, 16, 72 ) );
	SetCollisionGroup( COLLISION_GROUP_NONE );

	//SetSolidFlags( FSOLID_TRIGGER );

	m_takedamage = DAMAGE_YES;
	SetHealth( tf_target_dummy_health.GetInt() );

	//DispatchParticleEffect( "merasmus_object_spawn", WorldSpaceCenter(), GetAbsAngles() );

	// Intentionally persistent: this is a reusable damage-measurement target.
}

//-----------------------------------------------------------------------------
void CTFTargetDummy::Event_Killed( const CTakeDamageInfo &info )
{
	EmitSound( "Halloween.Merasmus_Hiding_Explode" );

	BaseClass::Event_Killed( info );
}

//-----------------------------------------------------------------------------
int CTFTargetDummy::OnTakeDamage( const CTakeDamageInfo &info )
{
	DispatchParticleEffect( "lowV_debrischunks", info.GetDamagePosition(), GetAbsAngles() );

	CBaseEntity *pAttacker = info.GetAttacker();
	CBaseEntity *pInflictor = info.GetInflictor();
	Msg( "[TF2BridgeDummy] hit target=%s attacker=%s inflictor=%s damage=%.2f base=%.2f type=0x%08x pos=(%.1f %.1f %.1f) healthBefore=%d\n",
		GetClassname(),
		pAttacker ? pAttacker->GetClassname() : "<null>",
		pInflictor ? pInflictor->GetClassname() : "<null>",
		info.GetDamage(),
		info.GetBaseDamage(),
		info.GetDamageType(),
		info.GetDamagePosition().x, info.GetDamagePosition().y, info.GetDamagePosition().z,
		m_iHealth );

	// Do not apply the pumpkin target's custom damage multipliers or team filter.
	int iDamage = BaseClass::OnTakeDamage( info );

	if ( m_iHealth <= 0 )
	{
		Msg( "[TF2BridgeDummy] killed by damage event\n" );
		Destroy();
	}

	return iDamage;
}

//-----------------------------------------------------------------------------
void CTFTargetDummy::DestroyThink()
{
	// Blow us up.
	//CTakeDamageInfo info( this, this, vec3_origin, GetAbsOrigin(), 0, DMG_GENERIC );
	//Killed( info );
	Destroy();
	UTIL_Remove( this );
}

//-----------------------------------------------------------------------------
void CTFTargetDummy::Destroy()
{
	SpewGibs();
	DispatchParticleEffect( "target_break", WorldSpaceCenter(), GetAbsAngles() );
}

//-----------------------------------------------------------------------------
void CTFTargetDummy::SpewGibs()
{
	for ( int i = 0; i < m_aGibs.Count(); i++ )
	{
		CTFAmmoPack *pAmmoPack = CTFAmmoPack::Create( GetAbsOrigin() + m_aGibs[i].offset, GetAbsAngles(), this, m_aGibs[i].modelName );
		Assert( pAmmoPack );
		if ( pAmmoPack )
		{
			pAmmoPack->ActivateWhenAtRest();

			// Calculate the initial impulse on the weapon.
			Vector vecImpulse( random->RandomFloat( -0.5f, 0.5f ), random->RandomFloat( -0.5f, 0.5f ), random->RandomFloat( 0.75f, 1.25f ) );
			VectorNormalize( vecImpulse );
			vecImpulse *= random->RandomFloat( tf_obj_gib_velocity_min.GetFloat(), tf_obj_gib_velocity_max.GetFloat() );

			// Cap the impulse.
			float flSpeed = vecImpulse.Length();
			if ( flSpeed > tf_obj_gib_maxspeed.GetFloat() )
			{
				VectorScale( vecImpulse, tf_obj_gib_maxspeed.GetFloat() / flSpeed, vecImpulse );
			}

			if ( pAmmoPack->VPhysicsGetObject() )
			{
				AngularImpulse angImpulse( 0.f, random->RandomFloat( 0.f, 100.f ), 0.f );
				pAmmoPack->VPhysicsGetObject()->SetVelocityInstantaneous( &vecImpulse, &angImpulse );
			}

			pAmmoPack->SetInitialVelocity( vecImpulse );

			pAmmoPack->m_nSkin = ( GetTeamNumber() == TF_TEAM_RED ) ? 0 : 1;

			// Give the ammo pack some health, so that trains can destroy it.
			pAmmoPack->SetCollisionGroup( COLLISION_GROUP_DEBRIS );
			pAmmoPack->m_takedamage = DAMAGE_YES;
			pAmmoPack->SetHealth( 900 );
			pAmmoPack->m_bObjGib = true;

			pAmmoPack->GiveAmmo( 0, TF_AMMO_METAL );
		}
	}
}

//-----------------------------------------------------------------------------
bool CTFTargetDummy::ShouldCollide( int collisionGroup, int contentsMask ) const
{
	if ( collisionGroup == COLLISION_GROUP_PLAYER_MOVEMENT )
	{
		if ( GetCollisionGroup() == TFCOLLISION_GROUP_OBJECT_SOLIDTOPLAYERMOVEMENT )
		{
			return true;
		}

		switch ( GetTeamNumber() )
		{
		case TF_TEAM_RED:
			if ( !( contentsMask & CONTENTS_REDTEAM ) )
				return false;
			break;

		case TF_TEAM_BLUE:
			if ( !( contentsMask & CONTENTS_BLUETEAM ) )
				return false;
			break;
		}
	}

	return BaseClass::ShouldCollide( collisionGroup, contentsMask );
}

//-----------------------------------------------------------------------------
CON_COMMAND_F( tf2bridge_trace_dummy, "Report TF2 bridge dummy collision state and compare shot vs broad-mask traces.", FCVAR_CHEAT )
{
	CBasePlayer *pBasePlayer = UTIL_GetCommandClient();
	CTFPlayer *pPlayer = ToTFPlayer( pBasePlayer );
	if ( !pPlayer )
	{
		Msg( "tf2bridge_trace_dummy must be run by a player in-game.\n" );
		return;
	}

	CTFTargetDummy *pDummy = static_cast<CTFTargetDummy *>( gEntList.FindEntityByClassname( NULL, "tf_target_dummy" ) );
	if ( !pDummy )
	{
		Msg( "[TF2BridgeTrace] no tf_target_dummy entity found. Spawn one first.\n" );
		return;
	}

	const Vector vecOrigin = pDummy->GetAbsOrigin();
	const Vector vecMins = pDummy->CollisionProp()->OBBMins();
	const Vector vecMaxs = pDummy->CollisionProp()->OBBMaxs();
	const Vector vecCenter = pDummy->WorldSpaceCenter();
	Msg( "[TF2BridgeTrace] dummy=%s origin=(%.1f %.1f %.1f) center=(%.1f %.1f %.1f) mins=(%.1f %.1f %.1f) maxs=(%.1f %.1f %.1f) solid=%d solidFlags=0x%x collisionGroup=%d health=%d\n",
		pDummy->GetClassname(),
		vecOrigin.x, vecOrigin.y, vecOrigin.z,
		vecCenter.x, vecCenter.y, vecCenter.z,
		vecMins.x, vecMins.y, vecMins.z,
		vecMaxs.x, vecMaxs.y, vecMaxs.z,
		(int)pDummy->GetSolid(), pDummy->GetSolidFlags(), pDummy->GetCollisionGroup(),
		pDummy->GetHealth() );

	CBaseEntity *pNearby[128];
	const Vector vecBoxMins = vecOrigin - Vector( 32, 32, 32 );
	const Vector vecBoxMaxs = vecOrigin + Vector( 32, 32, 80 );
	const int nNearby = UTIL_EntitiesInBox( pNearby, ARRAYSIZE( pNearby ), vecBoxMins, vecBoxMaxs, 0 );
	bool bFoundInBox = false;
	for ( int i = 0; i < nNearby; ++i )
	{
		if ( pNearby[i] == pDummy )
			bFoundInBox = true;
	}
	Msg( "[TF2BridgeTrace] nearby box entities=%d dummyFound=%d boxMins=(%.1f %.1f %.1f) boxMaxs=(%.1f %.1f %.1f)\n",
		nNearby, bFoundInBox ? 1 : 0,
		vecBoxMins.x, vecBoxMins.y, vecBoxMins.z,
		vecBoxMaxs.x, vecBoxMaxs.y, vecBoxMaxs.z );
	for ( int i = 0; i < nNearby && i < 12; ++i )
	{
		Msg( "[TF2BridgeTrace] nearby[%d]=%s origin=(%.1f %.1f %.1f) solid=%d\n",
			i, pNearby[i] ? pNearby[i]->GetClassname() : "<null>",
			pNearby[i] ? pNearby[i]->GetAbsOrigin().x : 0.0f,
			pNearby[i] ? pNearby[i]->GetAbsOrigin().y : 0.0f,
			pNearby[i] ? pNearby[i]->GetAbsOrigin().z : 0.0f,
			pNearby[i] ? (int)pNearby[i]->GetSolid() : -1 );
	}

	Vector vecForward;
	AngleVectors( pPlayer->EyeAngles(), &vecForward );
	const Vector vecStart = pPlayer->EyePosition();
	const Vector vecEnd = vecStart + vecForward * 4096.0f;
	trace_t trViewShot;
	UTIL_TraceLine( vecStart, vecEnd, MASK_SHOT, pPlayer, COLLISION_GROUP_NONE, &trViewShot );
	Msg( "[TF2BridgeTrace] view MASK_SHOT fraction=%.4f startsolid=%d allsolid=%d hit=%s hitWorld=%d hitpos=(%.1f %.1f %.1f)\n",
		trViewShot.fraction, trViewShot.startsolid ? 1 : 0, trViewShot.allsolid ? 1 : 0,
		trViewShot.m_pEnt ? trViewShot.m_pEnt->GetClassname() : "<none>", trViewShot.DidHitWorld() ? 1 : 0,
		trViewShot.endpos.x, trViewShot.endpos.y, trViewShot.endpos.z );

	trace_t trViewBroad;
	UTIL_TraceLine( vecStart, vecEnd, MASK_ALL, pPlayer, COLLISION_GROUP_NONE, &trViewBroad );
	Msg( "[TF2BridgeTrace] view MASK_ALL fraction=%.4f startsolid=%d allsolid=%d hit=%s hitWorld=%d hitpos=(%.1f %.1f %.1f)\n",
		trViewBroad.fraction, trViewBroad.startsolid ? 1 : 0, trViewBroad.allsolid ? 1 : 0,
		trViewBroad.m_pEnt ? trViewBroad.m_pEnt->GetClassname() : "<none>", trViewBroad.DidHitWorld() ? 1 : 0,
		trViewBroad.endpos.x, trViewBroad.endpos.y, trViewBroad.endpos.z );

	trace_t trDirectShot;
	UTIL_TraceLine( vecStart, vecCenter, MASK_SHOT, pPlayer, COLLISION_GROUP_NONE, &trDirectShot );
	Msg( "[TF2BridgeTrace] direct MASK_SHOT fraction=%.4f startsolid=%d allsolid=%d hit=%s hitWorld=%d hitpos=(%.1f %.1f %.1f)\n",
		trDirectShot.fraction, trDirectShot.startsolid ? 1 : 0, trDirectShot.allsolid ? 1 : 0,
		trDirectShot.m_pEnt ? trDirectShot.m_pEnt->GetClassname() : "<none>", trDirectShot.DidHitWorld() ? 1 : 0,
		trDirectShot.endpos.x, trDirectShot.endpos.y, trDirectShot.endpos.z );

	trace_t trDirectBroad;
	UTIL_TraceLine( vecStart, vecCenter, MASK_ALL, pPlayer, COLLISION_GROUP_NONE, &trDirectBroad );
	Msg( "[TF2BridgeTrace] direct MASK_ALL fraction=%.4f startsolid=%d allsolid=%d hit=%s hitWorld=%d hitpos=(%.1f %.1f %.1f)\n",
		trDirectBroad.fraction, trDirectBroad.startsolid ? 1 : 0, trDirectBroad.allsolid ? 1 : 0,
		trDirectBroad.m_pEnt ? trDirectBroad.m_pEnt->GetClassname() : "<none>", trDirectBroad.DidHitWorld() ? 1 : 0,
		trDirectBroad.endpos.x, trDirectBroad.endpos.y, trDirectBroad.endpos.z );
}

CON_COMMAND_F( tf2bridge_spawn_dummy, "Spawn a persistent TF2 bridge damage-test dummy in front of the issuing player.", FCVAR_CHEAT )
{
	CBasePlayer *pBasePlayer = UTIL_GetCommandClient();
	CTFPlayer *pPlayer = ToTFPlayer( pBasePlayer );
	if ( !pPlayer )
	{
		Msg( "tf2bridge_spawn_dummy must be run by a player in-game.\n" );
		return;
	}

	Vector vecForward;
	AngleVectors( pPlayer->EyeAngles(), &vecForward );
	const Vector vecOrigin = pPlayer->EyePosition() + vecForward * 128.0f;
	CTFTargetDummy *pDummy = CTFTargetDummy::Create( vecOrigin, pPlayer->EyeAngles(), pPlayer );
	if ( pDummy )
	{
		Msg( "[TF2BridgeDummy] spawned at (%.1f %.1f %.1f), health=%d\n", vecOrigin.x, vecOrigin.y, vecOrigin.z, pDummy->GetHealth() );
	}
}
