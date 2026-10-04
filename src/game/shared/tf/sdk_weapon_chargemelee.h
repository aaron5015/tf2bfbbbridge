//========= Custom Weapon: Charge-based Melee =========//
//
// Purpose: Melee weapon that builds a charge meter the longer the wielder
//          stays alive and moving. Two charge tiers grant stored minicrit
//          or full-crit swings plus a small/large AoE burst on hit.
//
// NOTE: This assumes a base class roughly like CTFWeaponBaseMelee from the
// TF2 SDK (ItemPostFrame, AddMeleeDamage/PrimaryAttack overrides, etc).
// Your project's actual base class name and virtual signatures may differ
// slightly (e.g. CTFWeaponBaseMelee vs a custom intermediate class) --
// swap the base class / overridden signatures to match your tree. Anywhere
// I'm guessing at a method name, I've left a comment: // ADAPT: ...
//
//=============================================================================//

#ifndef SDK_WEAPON_CHARGEMELEE_H
#define SDK_WEAPON_CHARGEMELEE_H
#ifdef _WIN32
#pragma once
#endif

#include "tf_weaponbase_melee.h"   // ADAPT: replace with your melee base header

#if defined( CLIENT_DLL )
#define CTFChargeMelee C_TFChargeMelee
#endif

// ------------------------------------------------------------------------
// Tuning constants -- pull these out to a weapon script (per-item KeyValues)
// later if you want per-loadout-slot tuning instead of hardcoded values.
// ------------------------------------------------------------------------
namespace ChargeMeleeConfig
{
	// Meter thresholds (percent, 0-100)
	const float TIER1_THRESHOLD		= 30.0f;
	const float TIER2_THRESHOLD		= 100.0f;

	// How fast the meter fills while alive + moving (percent per second).
	// 100.0f / RAMP_SECONDS = percent/sec
	const float RAMP_SECONDS			= 20.0f; // full 0->100 in 20s of uptime
	const float METER_FILL_RATE		= 100.0f / RAMP_SECONDS;

	// Minimum speed (units/sec) to count as "moving". Prevents bunnyhop-in-place
	// or getting stuck at a wall from faking uptime. Tune per-class if needed.
	const float MIN_MOVE_SPEED			= 40.0f;

	// If the player stops moving, do we freeze the meter or drain it?
	// true  = meter drains back toward 0 while stationary
	// false = meter just pauses (freezes) while stationary
	const bool  DRAIN_WHILE_STATIONARY = true;
	const float DRAIN_RATE				= METER_FILL_RATE * 1.5f; // drains faster than it fills

	// Stored hit charges granted on entering each tier
	const int   TIER1_MINICRITS			= 3;
	const int   TIER2_CRITS				= 6;

	// Stat bonuses per tier (multipliers, 1.0 = no change)
	const float TIER0_SPEED_MOD			= 1.0f;
	const float TIER0_ATTACKSPEED_MOD		= 1.0f;

	const float TIER1_SPEED_MOD			= 1.10f;
	const float TIER1_ATTACKSPEED_MOD		= 1.15f;

	const float TIER2_SPEED_MOD			= 1.20f;
	const float TIER2_ATTACKSPEED_MOD		= 1.30f;

	// AoE burst radii/damage on a successful melee hit while charged
	const float TIER1_AOE_RADIUS			= 110.0f;  // "small rocket-blast-ish" radius
	const float TIER1_AOE_DAMAGE			= 20.0f;

	const float TIER2_AOE_RADIUS			= 175.0f;  // moderately bigger than tier1
	const float TIER2_AOE_DAMAGE			= 35.0f;
}

enum ChargeMeleeTier_t
{
	CHARGE_TIER_NONE = 0,
	CHARGE_TIER_MINICRIT,	// 30% - 99%
	CHARGE_TIER_FULLCRIT,	// 100%
};

class CTFChargeMelee : public CTFWeaponBaseMelee  // ADAPT: your melee base class
{
public:
	DECLARE_CLASS( CTFChargeMelee, CTFWeaponBaseMelee );
	DECLARE_NETWORKCLASS();
	DECLARE_PREDICTABLE();

	CTFChargeMelee();

	virtual void	Spawn( void );
	virtual void	ItemPostFrame( void );			// ADAPT: confirm this is virtual in your base
	virtual void	WeaponReset( void );			// called on death/round reset -- ADAPT to your respawn hook

	// Called by the melee hit-resolution code right before damage is applied.
	// ADAPT: hook this into wherever your base actually computes damage
	// (commonly something like CTFWeaponBaseMelee::Smack() or DoSwingTrace()).
	int				GetMeleeDamageType( void );		// returns DMG_CRITICAL / DMG_MINICRITICAL / 0 as appropriate
	void			OnMeleeHitEntity( CBaseEntity *pHitEntity, const Vector &vecHitPos ); // consumes a stored hit + fires AoE

	// Speed/attack-speed hooks -- ADAPT names to match how your base queries these
	float			GetMoveSpeedModifier( void ) const { return m_flCurrentSpeedMod; }
	float			GetAttackSpeedModifier( void ) const { return m_flCurrentAtkSpeedMod; }

	// HUD support
	float			GetChargeMeter( void ) const { return m_flChargeMeter; }
	int				GetChargeTier( void ) const { return m_iChargeTier; }
	int				GetStoredMinicrits( void ) const { return m_iStoredMinicrits; }
	int				GetStoredCrits( void ) const { return m_iStoredCrits; }

private:
	void			UpdateChargeMeter( void );
	void			SetTier( int nNewTier );
	void			ResetChargeToZero( void );
	void			ApplyAoEBurst( const Vector &vecOrigin, float flRadius, float flDamage );
	bool			IsOwnerMoving( void ) const;

	CNetworkVar( float, m_flChargeMeter );		// 0-100, replicated for HUD
	CNetworkVar( int,   m_iChargeTier );		// ChargeMeleeTier_t, replicated for HUD/FX
	CNetworkVar( int,   m_iStoredMinicrits );
	CNetworkVar( int,   m_iStoredCrits );

	float			m_flCurrentSpeedMod;
	float			m_flCurrentAtkSpeedMod;
	float			m_flLastThinkTime;

	CTFChargeMelee( const CTFChargeMelee & ) = delete;
};

#endif // SDK_WEAPON_CHARGEMELEE_H
