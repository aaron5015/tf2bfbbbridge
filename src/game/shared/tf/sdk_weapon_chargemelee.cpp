//========= Custom Weapon: Charge-based Melee =========//
#include "cbase.h"
#include "sdk_weapon_chargemelee.h"
#include "tf_gamerules.h"
#include "in_buttons.h"
#include "takedamageinfo.h"

#if defined( GAME_DLL )
#include "tf_player.h"
#include "util.h"			// RadiusDamage
#include "te_effect_dispatch.h"
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

IMPLEMENT_NETWORKCLASS_ALIASED( TFChargeMelee, DT_TFChargeMelee )

BEGIN_NETWORK_TABLE( CTFChargeMelee, DT_TFChargeMelee )
#if defined( CLIENT_DLL )
	RecvPropFloat( RECVINFO( m_flChargeMeter ) ),
	RecvPropInt( RECVINFO( m_iChargeTier ) ),
	RecvPropInt( RECVINFO( m_iStoredMinicrits ) ),
	RecvPropInt( RECVINFO( m_iStoredCrits ) ),
#else
	SendPropFloat( SENDINFO( m_flChargeMeter ), 8, SPROP_ROUNDDOWN, 0.0f, 100.0f ),
	SendPropInt( SENDINFO( m_iChargeTier ), 2, SPROP_UNSIGNED ),
	SendPropInt( SENDINFO( m_iStoredMinicrits ), 4, SPROP_UNSIGNED ),
	SendPropInt( SENDINFO( m_iStoredCrits ), 4, SPROP_UNSIGNED ),
#endif
END_NETWORK_TABLE()

BEGIN_PREDICTION_DATA( CTFChargeMelee )
END_PREDICTION_DATA()

LINK_ENTITY_TO_CLASS( tf_weapon_chargemelee, CTFChargeMelee );
PRECACHE_WEAPON_REGISTER( tf_weapon_chargemelee );

CTFChargeMelee::CTFChargeMelee()
{
	m_flChargeMeter        = 0.0f;
	m_iChargeTier          = CHARGE_TIER_NONE;
	m_iStoredMinicrits     = 0;
	m_iStoredCrits         = 0;
	m_flCurrentSpeedMod    = ChargeMeleeConfig::TIER0_SPEED_MOD;
	m_flCurrentAtkSpeedMod = ChargeMeleeConfig::TIER0_ATTACKSPEED_MOD;
	m_flLastThinkTime      = 0.0f;
}

void CTFChargeMelee::Spawn( void )
{
	BaseClass::Spawn();
	ResetChargeToZero();
}

// Call this from your respawn / death hook (e.g. CTFPlayer::TFPlayerClassInit,
// or wherever weapons are handed to a freshly-spawned player). ADAPT the hookup site.
void CTFChargeMelee::WeaponReset( void )
{
	ResetChargeToZero();
}

void CTFChargeMelee::ResetChargeToZero( void )
{
	m_flChargeMeter    = 0.0f;
	m_iStoredMinicrits = 0;
	m_iStoredCrits     = 0;
	SetTier( CHARGE_TIER_NONE );
}

void CTFChargeMelee::SetTier( int nNewTier )
{
	if ( m_iChargeTier == nNewTier )
		return;

	m_iChargeTier = nNewTier;

	switch ( nNewTier )
	{
	case CHARGE_TIER_NONE:
		m_flCurrentSpeedMod    = ChargeMeleeConfig::TIER0_SPEED_MOD;
		m_flCurrentAtkSpeedMod = ChargeMeleeConfig::TIER0_ATTACKSPEED_MOD;
		break;

	case CHARGE_TIER_MINICRIT:
		m_flCurrentSpeedMod    = ChargeMeleeConfig::TIER1_SPEED_MOD;
		m_flCurrentAtkSpeedMod = ChargeMeleeConfig::TIER1_ATTACKSPEED_MOD;
		// Minicrits are only (re)granted the moment we cross into this tier
		// from below -- if we're arriving here because crits just ran out
		// and the meter reset elsewhere, ResetChargeToZero already zeroed
		// things out, so this path only fires on 0->30 crossing.
		m_iStoredMinicrits = ChargeMeleeConfig::TIER1_MINICRITS;
		break;

	case CHARGE_TIER_FULLCRIT:
		m_flCurrentSpeedMod    = ChargeMeleeConfig::TIER2_SPEED_MOD;
		m_flCurrentAtkSpeedMod = ChargeMeleeConfig::TIER2_ATTACKSPEED_MOD;
		// Entering full-crit phase replaces any leftover minicrits outright.
		m_iStoredMinicrits = 0;
		m_iStoredCrits     = ChargeMeleeConfig::TIER2_CRITS;
		break;
	}

#if defined( GAME_DLL )
	// ADAPT: fire a sound/particle cue here for the tier-up moment, e.g.:
	// CPASAttenuationFilter filter( GetOwner() );
	// EmitSound( filter, GetOwner()->entindex(), "Weapon_ChargeMelee.TierUp" );
#endif
}

bool CTFChargeMelee::IsOwnerMoving( void ) const
{
	CBaseCombatCharacter *pOwner = GetOwner();
	if ( !pOwner )
		return false;

	// ADAPT: if your base class exposes the TF player type directly, cast
	// to CTFPlayer* instead of using the generic combat character velocity.
	float flSpeed = pOwner->GetAbsVelocity().Length2D();
	return flSpeed >= ChargeMeleeConfig::MIN_MOVE_SPEED;
}

void CTFChargeMelee::ItemPostFrame( void )
{
	BaseClass::ItemPostFrame();

#if defined( GAME_DLL )
	CBaseCombatCharacter *pOwner = GetOwner();
	if ( !pOwner || !pOwner->IsAlive() )
	{
		// Dead: nothing accrues, and death should already have called
		// WeaponReset() via your respawn hook. This is a safety net.
		m_flLastThinkTime = gpGlobals->curtime;
		return;
	}

	UpdateChargeMeter();
#endif
}

void CTFChargeMelee::UpdateChargeMeter( void )
{
	float flNow = gpGlobals->curtime;
	if ( m_flLastThinkTime <= 0.0f )
	{
		m_flLastThinkTime = flNow;
		return;
	}

	float flDelta = flNow - m_flLastThinkTime;
	m_flLastThinkTime = flNow;

	if ( flDelta <= 0.0f )
		return;

	bool bMoving = IsOwnerMoving();

	if ( bMoving )
	{
		m_flChargeMeter += ChargeMeleeConfig::METER_FILL_RATE * flDelta;
	}
	else if ( ChargeMeleeConfig::DRAIN_WHILE_STATIONARY )
	{
		m_flChargeMeter -= ChargeMeleeConfig::DRAIN_RATE * flDelta;
	}
	// else: frozen while stationary, no change

	m_flChargeMeter = clamp( m_flChargeMeter, 0.0f, ChargeMeleeConfig::TIER2_THRESHOLD );

	// Tier transitions based purely on meter position. Note we only ever
	// SetTier() upward here -- downward transitions from meter draining to
	// 0 are handled the same way (crossing back below TIER1_THRESHOLD drops
	// you to NONE), but running out of *stored hits* is a separate, explicit
	// reset call from OnMeleeHitEntity(), not driven by the meter directly.
	if ( m_flChargeMeter >= ChargeMeleeConfig::TIER2_THRESHOLD )
	{
		if ( m_iChargeTier != CHARGE_TIER_FULLCRIT )
			SetTier( CHARGE_TIER_FULLCRIT );
	}
	else if ( m_flChargeMeter >= ChargeMeleeConfig::TIER1_THRESHOLD )
	{
		if ( m_iChargeTier == CHARGE_TIER_NONE )
			SetTier( CHARGE_TIER_MINICRIT );
		// If we were TIER_FULLCRIT and drained back under 100, we intentionally
		// do NOT downgrade mid-stash -- keep spending crits until they run out,
		// per "both phases once depleted... will reset back to 0%". Remove this
		// comment block and add a downgrade path here if you'd rather have the
		// meter draining under 100% immediately knock crits down to minicrits.
	}
	else
	{
		if ( m_iChargeTier == CHARGE_TIER_MINICRIT && m_iStoredMinicrits <= 0 )
		{
			// Meter drained below 30 with nothing left stored anyway -- just
			// let it fall through to NONE naturally.
			SetTier( CHARGE_TIER_NONE );
		}
	}
}

int CTFChargeMelee::GetMeleeDamageType( void )
{
	if ( m_iChargeTier == CHARGE_TIER_FULLCRIT && m_iStoredCrits > 0 )
		return DMG_CRITICAL;

	if ( m_iChargeTier == CHARGE_TIER_MINICRIT && m_iStoredMinicrits > 0 )
		return CTakeDamageInfo::CRIT_MINI;

	return 0; // normal damage
}

// ADAPT: call this from your swing-resolution code (wherever the melee
// trace confirms a hit and damage is about to be dealt), passing the
// entity that got hit and the world-space impact position.
void CTFChargeMelee::OnMeleeHitEntity( CBaseEntity *pHitEntity, const Vector &vecHitPos )
{
	bool bDepleted = false;

	if ( m_iChargeTier == CHARGE_TIER_FULLCRIT && m_iStoredCrits > 0 )
	{
		m_iStoredCrits--;
		ApplyAoEBurst( vecHitPos, ChargeMeleeConfig::TIER2_AOE_RADIUS, ChargeMeleeConfig::TIER2_AOE_DAMAGE );

		if ( m_iStoredCrits <= 0 )
			bDepleted = true;
	}
	else if ( m_iChargeTier == CHARGE_TIER_MINICRIT && m_iStoredMinicrits > 0 )
	{
		m_iStoredMinicrits--;
		ApplyAoEBurst( vecHitPos, ChargeMeleeConfig::TIER1_AOE_RADIUS, ChargeMeleeConfig::TIER1_AOE_DAMAGE );

		if ( m_iStoredMinicrits <= 0 )
			bDepleted = true;
	}

	// "both phases once depleted of the stored minicrits or stored crits
	// will reset back to 0%, as long as the minicrits or crits have been
	// used up regardless [of current meter %]."
	if ( bDepleted )
	{
		ResetChargeToZero();
	}
}

void CTFChargeMelee::ApplyAoEBurst( const Vector &vecOrigin, float flRadius, float flDamage )
{
#if defined( GAME_DLL )
	CBaseCombatCharacter *pOwner = GetOwner();
	if ( !pOwner )
		return;

	CTakeDamageInfo info( this, pOwner, flDamage, DMG_BLAST );
	// ADAPT: swap RADIUS_DAMAGE_HALF_LIVES_TO_PLAYER (or your project's
	// equivalent falloff flag) for whatever radius-damage helper your SDK
	// fork exposes -- some forks wrap this in TFGameRules()->RadiusDamage().
	RadiusDamage( info, vecOrigin, flRadius, CLASS_NONE, pOwner );

	// ADAPT: dispatch a small explosion effect at vecOrigin for visual
	// feedback, sized to match flRadius, e.g. via te->Explosion() or a
	// custom particle. Left out here since it depends on your FX budget
	// and precached particle names.
#else
	NOTE_UNUSED( vecOrigin );
	NOTE_UNUSED( flRadius );
	NOTE_UNUSED( flDamage );
#endif
}
