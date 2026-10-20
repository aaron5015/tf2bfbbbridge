//========= BFBB <-> TF2 bridge, server side ================================
//
// Runs inside the TF2 listen server. Every server tick it
//   * sends BFBB the TF2 host's position, velocity, eyes, view angles, buttons,
//     class and health (an "intent");
//   * receives BFBB's player state.
//
// Who owns the player's movement:
//   bfbb_sync_pos 0 (default)  TF2 runs the movement on the BFBB level mesh (see
//                              game/shared/tf2bridge/bfbb_collision.cpp). BFBB
//                              puts SpongeBob where TF2 says and looks through
//                              the TF2 eyes. When BFBB takes the player itself
//                              (cutscene, flythrough, respawn, warp) TF2 follows
//                              BFBB instead, then hands control back. On every
//                              new level TF2 is placed where BFBB's player starts.
//   bfbb_sync_pos 1            The original test mode: BFBB walks and TF2's player
//                              is teleported to BFBB's position every tick.
//
// Console variables / commands (server-side):
//   bfbb_bridge        1/0   master switch (default 1)
//   bfbb_sync_pos      1/0   see above
//   bfbb_unit_scale    float Source units per BFBB unit (default 50)
//   bfbb_bridge_debug  1/0   print what is sent/received once a second
//   bfbb_coll_draw     1/0   draw the BFBB collision triangles near you
//   bfbb_goto          (command) move the host to where BFBB's player is now,
//                      snapped onto the mesh floor
//   bfbb_coll_info     (command) print where the mesh is relative to you
//
//=============================================================================

#include "cbase.h"
#include "igamesystem.h"
#include "player.h"
#include "tf_player.h"
#include "tf_weaponbase.h"
#include "tf_weaponbase_gun.h"
#include "tf_projectile_rocket.h"
#include "tf_target_dummy.h"
#include "usercmd.h"

#include "tf2bridge_net.h"
#include "tf2bridge/bfbb_collision.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar bfbb_bridge("bfbb_bridge", "1", FCVAR_NONE, "Enable the BFBB bridge");
static ConVar bfbb_sync_pos("bfbb_sync_pos", "0", FCVAR_NONE,
    "0: TF2 runs the movement and BFBB follows. 1: BFBB moves, TF2 is teleported");
// Defined in game/shared/tf2bridge/bfbb_collision.cpp (shared with the client).
extern ConVar bfbb_unit_scale;
extern ConVar bfbb_scene;
static ConVar bfbb_coll_draw("bfbb_coll_draw", "0", FCVAR_NONE,
    "Draw BFBB collision triangles near the host (green up, red down, yellow walls)");
static ConVar bfbb_bridge_debug("bfbb_bridge_debug", "0", FCVAR_NONE, "Print bridge traffic");
void TF2Bridge_ApplyRocketImpact(int entIndex, const float origin[3])
{
    CBaseEntity* entity = UTIL_EntityByIndex(entIndex);
    CTFBaseRocket* rocket = dynamic_cast<CTFBaseRocket*>(entity);
    if (rocket == NULL)
        return;

    rocket->SetAbsOrigin(Vector(origin[0], origin[1], origin[2]));
    rocket->Destroy(false, false);
}

static ConVar bfbb_rocket_debug_lifetime("bfbb_rocket_debug_lifetime", "3.0", FCVAR_NONE,
    "Seconds to keep BFBB rocket impact/radius markers visible; 0 hides them (rockets still work)");

// Source's default run speed for the usercmd axes (cl_forwardspeed / cl_sidespeed).
static const float kCmdAxisMax = 450.0f;

 // Cache the launcher's multiplier per projectile so switching weapons in flight
 // does not retroactively change a rocket's impact tuning.
static int sRocketMultiplierEnt[BRIDGE_MAX_ROCKETS] = {};
static float sRocketMultiplierValue[BRIDGE_MAX_ROCKETS] = {};
static bool sRocketMultiplierSeen[BRIDGE_MAX_ROCKETS] = {};
static int sRocketMeshEnt[BRIDGE_MAX_ROCKETS] = {};
static Vector sRocketMeshPrevious[BRIDGE_MAX_ROCKETS];
static bool sRocketMeshSeen[BRIDGE_MAX_ROCKETS] = {};

static void TF2Bridge_BeginRocketMeshFrame()
{
	memset( sRocketMeshSeen, 0, sizeof( sRocketMeshSeen ) );
}

static bool TF2Bridge_TraceRocketAgainstBfbb( CTFBaseRocket *pRocket )
{
	if ( !pRocket || !pRocket->IsSolid() || !BFBBColl_LoadedScene() )
		return false;

	int iSlot = -1;
	for ( int i = 0; i < BRIDGE_MAX_ROCKETS; ++i )
	{
		if ( sRocketMeshEnt[i] == pRocket->entindex() )
		{
			iSlot = i;
			break;
		}
		if ( iSlot < 0 && sRocketMeshEnt[i] == 0 )
			iSlot = i;
	}
	if ( iSlot < 0 )
		return false;

	const Vector vecEnd = pRocket->GetAbsOrigin();
	Vector vecStart = vecEnd - pRocket->GetAbsVelocity() * gpGlobals->frametime;
	if ( sRocketMeshEnt[iSlot] == pRocket->entindex() )
		vecStart = sRocketMeshPrevious[iSlot];
	sRocketMeshEnt[iSlot] = pRocket->entindex();
	sRocketMeshPrevious[iSlot] = vecEnd;
	sRocketMeshSeen[iSlot] = true;

	if ( ( vecEnd - vecStart ).LengthSqr() <= 0.0001f )
		return false;

	Vector vecHit, vecNormal;
	if ( !BFBBColl_TraceLine( vecStart, vecEnd, vecHit, vecNormal ) )
		return false;

	trace_t tr;
	memset( &tr, 0, sizeof( tr ) );
	tr.startpos = vecStart;
	tr.endpos = vecHit;
	tr.fraction = ( vecHit - vecStart ).Length() / ( vecEnd - vecStart ).Length();
	tr.fraction = clamp( tr.fraction, 0.0f, 1.0f );
	tr.plane.normal = vecNormal;
	tr.plane.dist = DotProduct( vecNormal, vecHit );
	tr.plane.type = 3;
	tr.contents = CONTENTS_SOLID;
	tr.surface.surfaceProps = 0;
	tr.m_pEnt = CBaseEntity::Instance( INDEXENT( 0 ) );

	pRocket->Explode( &tr, tr.m_pEnt );
	sRocketMeshEnt[iSlot] = 0;
	return true;
}

static void TF2Bridge_EndRocketMeshFrame()
{
	for ( int i = 0; i < BRIDGE_MAX_ROCKETS; ++i )
	{
		if ( sRocketMeshEnt[i] != 0 && !sRocketMeshSeen[i] )
			sRocketMeshEnt[i] = 0;
	}
}

static float TF2Bridge_GetRocketMultiplier(int entIndex, float activeMultiplier)
{
    for (int i = 0; i < BRIDGE_MAX_ROCKETS; ++i)
    {
        if (sRocketMultiplierEnt[i] == entIndex)
        {
            sRocketMultiplierSeen[i] = true;
            return sRocketMultiplierValue[i];
        }
    }
    for (int i = 0; i < BRIDGE_MAX_ROCKETS; ++i)
    {
        if (sRocketMultiplierEnt[i] == 0)
        {
            sRocketMultiplierEnt[i] = entIndex;
            sRocketMultiplierValue[i] = activeMultiplier;
            sRocketMultiplierSeen[i] = true;
            return activeMultiplier;
        }
    }
    return activeMultiplier;
}


// BFBB (right-handed, +X toward screen-left, +Y up, +Z forward) to Source
// (x forward, y left, z up): a pure rotation, no mirroring. See iTF2BridgeProto.h.
static Vector BfbbToSource(float bx, float by, float bz, float scale)
{
    return Vector(bz * scale, bx * scale, by * scale);
}

static bool BridgeStateHasNpc(const BridgeStatePacket& state, unsigned int id)
{
    const uint32_t count = MIN(state.npcCount, (uint32_t)BRIDGE_MAX_NPC_TARGETS);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (state.npcs[i].id == id)
            return true;
    }
    return false;
}

static void TF2Bridge_UpdateNpcProxies(const BridgeStatePacket& state, float scale)
{
    if (!(state.flags & BRIDGE_STATE_GAMEPLAY) || !(state.flags & BRIDGE_STATE_NPC_TARGETS))
    {
        CBaseEntity* entity = NULL;
        while ((entity = gEntList.FindEntityByClassname(entity, "tf_target_dummy")) != NULL)
        {
            CTFTargetDummy* dummy = dynamic_cast<CTFTargetDummy*>(entity);
            if (dummy != NULL && dummy->IsBridgeNpcProxy())
                UTIL_Remove(dummy);
        }
        return;
    }

    const uint32_t count = MIN(state.npcCount, (uint32_t)BRIDGE_MAX_NPC_TARGETS);
    for (uint32_t i = 0; i < count; ++i)
    {
        const BridgeNpcTarget& target = state.npcs[i];
        if (target.id == 0)
            continue;

        CTFTargetDummy* proxy = NULL;
        CBaseEntity* entity = NULL;
        while ((entity = gEntList.FindEntityByClassname(entity, "tf_target_dummy")) != NULL)
        {
            CTFTargetDummy* candidate = dynamic_cast<CTFTargetDummy*>(entity);
            if (candidate != NULL && candidate->GetBridgeNpcId() == target.id)
            {
                proxy = candidate;
                break;
            }
        }

        const Vector origin = BfbbToSource(target.x, target.y, target.z, scale);
        if (proxy == NULL)
        {
            proxy = CTFTargetDummy::Create(origin, QAngle(0, 0, 0), NULL);
            if (proxy != NULL)
            {
                proxy->SetBridgeNpcId(target.id, state.sceneId);
                Msg("[bfbb] created NPC proxy id=%u type=%d at (%.1f %.1f %.1f)\n",
                    target.id, target.type, origin.x, origin.y, origin.z);
            }
        }
        if (proxy != NULL)
        {
            proxy->SetBridgeNpcId(target.id, state.sceneId);
            proxy->SetAbsOrigin(origin);
        }
    }

    CBaseEntity* entity = NULL;
    while ((entity = gEntList.FindEntityByClassname(entity, "tf_target_dummy")) != NULL)
    {
        CTFTargetDummy* dummy = dynamic_cast<CTFTargetDummy*>(entity);
        if (dummy != NULL && dummy->IsBridgeNpcProxy() &&
            !BridgeStateHasNpc(state, dummy->GetBridgeNpcId()))
        {
            Msg("[bfbb] removing stale NPC proxy id=%u\n", dummy->GetBridgeNpcId());
            UTIL_Remove(dummy);
        }
    }
}

// Finds the mesh floor near `pos` and returns a spot standing on it. If there is
// no mesh near, returns `pos` unchanged.
static Vector SnapToMeshFloor(const Vector& pos, bool verbose)
{
    Vector hit, n;
    if (BFBBColl_TraceLine(pos + Vector(0, 0, 150), pos - Vector(0, 0, 1500), hit, n))
    {
        if (verbose)
            Msg("[bfbb] floor found %.0f units %s BFBB's reported height\n", fabsf(hit.z - pos.z),
                hit.z < pos.z ? "below" : "above");
        return hit;
    }

    if (BFBBColl_TraceLine(pos, pos + Vector(0, 0, 1500), hit, n))
    {
        if (verbose)
            Warning("[bfbb] no floor below, but mesh %.0f units ABOVE -- placing on top of it\n", hit.z - pos.z);
        return hit;
    }

    if (verbose)
        Warning("[bfbb] no mesh near BFBB's position; using it as-is (try bfbb_coll_info)\n");
    return pos;
}

class CTF2BridgeSystem : public CAutoGameSystemPerFrame
{
public:
    CTF2BridgeSystem()
        : CAutoGameSystemPerFrame("TF2BridgeSystem"),
        m_haveState(false),
        m_stateReceivedAt(0.0f),
        m_nextDebug(0.0f),
        m_nextDraw(0.0f),
        m_lastScene(0),
        m_pendingSnap(false)
    {
        memset(&m_state, 0, sizeof(m_state));
    }

    virtual bool Init()
    {
        if (TF2Bridge_NetOpen())
            Msg("[bfbb] bridge listening on 127.0.0.1:%d\n", BRIDGE_PORT_TF2);
        else
            Warning("[bfbb] bridge could not bind 127.0.0.1:%d (another TF2 running?)\n",
                BRIDGE_PORT_TF2);
        return true;
    }

    virtual void Shutdown()
    {
        TF2Bridge_NetClose();
    }

    virtual void FrameUpdatePostEntityThink()
    {
        if (!bfbb_bridge.GetBool() || !TF2Bridge_NetIsOpen())
            return;

        BridgeStatePacket st;
        if (TF2Bridge_NetRecvState(&st))
        {
            if (m_haveState && st.sessionId != m_state.sessionId)
            {
                Msg("[bfbb] sender session changed (%08x -> %08x); refreshing scene state\n",
                    (unsigned)m_state.sessionId, (unsigned)st.sessionId);
                m_lastScene = 0;
                m_pendingSnap = true;
            }
            m_state = st;
            m_haveState = true;
            m_stateReceivedAt = gpGlobals->curtime;

            const bool gameplay = (st.flags & BRIDGE_STATE_GAMEPLAY) != 0 && st.sceneId != 0;

            // Tell both halves of the game (replicated) which level's collision to use.
            if (gameplay && (unsigned int)bfbb_scene.GetInt() != st.sceneId)
                bfbb_scene.SetValue((int)st.sceneId);

            // A new level: put the TF2 player where BFBB's player starts.
            if (gameplay && st.sceneId != m_lastScene)
            {
                m_lastScene = st.sceneId;
                m_pendingSnap = true;
            }
        }

        if (m_haveState)
        {
            if (gpGlobals->curtime - m_stateReceivedAt <= 0.25f)
                TF2Bridge_UpdateNpcProxies(m_state, bfbb_unit_scale.GetFloat());
            else
            {
                BridgeStatePacket staleState = {};
                TF2Bridge_UpdateNpcProxies(staleState, bfbb_unit_scale.GetFloat());
            }
        }

        CTFPlayer* pPlayer = ToTFPlayer(UTIL_GetListenServerHost());
        if (!pPlayer || !pPlayer->IsAlive())
            return;

        const bool owns = !bfbb_sync_pos.GetBool(); // TF2 runs the movement
        const bool gameplay = m_haveState && (m_state.flags & BRIDGE_STATE_GAMEPLAY);

        // --- BFBB -> TF2: placement --------------------------------------------
        if (owns)
        {
            const bool bfbbHasControl = gameplay && (m_state.flags & BRIDGE_STATE_CONTROL_OFF);

            if (gameplay && (bfbbHasControl || m_pendingSnap))
            {
                Vector pos;
                GetBfbbPosition(pos);
                if (m_pendingSnap && !bfbbHasControl)
                    pos = SnapToMeshFloor(pos, true);
                pos.z += 4.0f; // just clear of the surface

                pPlayer->SetMoveType(bfbbHasControl ? MOVETYPE_NONE : MOVETYPE_WALK);
                pPlayer->Teleport(&pos, NULL, &vec3_origin);
                m_pendingSnap = false;
            }
            else if (pPlayer->GetMoveType() == MOVETYPE_NONE)
            {
                // BFBB handed the player back.
                pPlayer->SetMoveType(MOVETYPE_WALK);
            }
        }
        else if (pPlayer->GetMoveType() == MOVETYPE_NONE && !gameplay)
        {
            pPlayer->SetMoveType(MOVETYPE_WALK);
        }

        if (!owns && gameplay)
        {
            // The original test mode: BFBB owns movement; TF2 only mirrors it.
            Vector pos;
            GetBfbbPosition(pos);
            pPlayer->SetMoveType(MOVETYPE_NONE);
            pPlayer->Teleport(&pos, NULL, &vec3_origin);
        }

        // --- TF2 -> BFBB: intent -----------------------------------------------
        const CUserCmd* cmd = pPlayer->GetLastUserCommand();
        if (cmd)
        {
            BridgeIntentPacket in;
            memset(&in, 0, sizeof(in));
            in.flags = owns ? BRIDGE_INTENT_OWNS_MOVE : 0;
            in.scale = bfbb_unit_scale.GetFloat();
            in.forward = clamp(cmd->forwardmove / kCmdAxisMax, -1.0f, 1.0f);
            in.side = clamp(cmd->sidemove / kCmdAxisMax, -1.0f, 1.0f);
            in.pitch = cmd->viewangles.x;
            in.yaw = cmd->viewangles.y;
            in.buttons = (uint32_t)cmd->buttons; // IN_* bits match BRIDGE_IN_*
            in.tfclass = pPlayer->GetPlayerClass() ? pPlayer->GetPlayerClass()->GetClassIndex() : 0;
            in.tfhealth = pPlayer->GetHealth();
            CTFWeaponBase* activeWeapon = pPlayer->GetActiveTFWeapon();
            in.knockbackMultiplier = activeWeapon != NULL ? activeWeapon->GetBridgeKnockbackMultiplier() : 1.5f;
            in.weaponDamage = activeWeapon != NULL ? activeWeapon->GetBridgeDamageEstimate() : 0.0f;
            if (activeWeapon != NULL && activeWeapon->IsMeleeWeapon())
                in.weaponflags |= BRIDGE_WEAPON_MELEE;
            CTFWeaponBaseGun* activeGun = dynamic_cast<CTFWeaponBaseGun*>(activeWeapon);
            if (activeGun != NULL && activeGun->GetWeaponProjectileType() == TF_PROJECTILE_ROCKET)
                in.weaponflags |= BRIDGE_WEAPON_ROCKET;

            const Vector origin = pPlayer->GetAbsOrigin();
            const Vector vel = pPlayer->GetAbsVelocity();
            const Vector eye = pPlayer->EyePosition();
            in.px = origin.x; in.py = origin.y; in.pz = origin.z;
            in.vx = vel.x;    in.vy = vel.y;    in.vz = vel.z;
            in.ex = eye.x;    in.ey = eye.y;    in.ez = eye.z;

            // Rocket bridge. BFBB JSP collision is not an engine world entity, so
            // sweep live rockets against the imported mesh here and run TF2's native
            // explosion at impact. NPC damage returns through proxy hit packets.
            // bfbb_rocket_debug_lifetime now only controls how long BFBB's markers
            // stay up (0 = none); rockets are reported either way.
            in.rocketDebugLifetime = MAX(0.0f, bfbb_rocket_debug_lifetime.GetFloat());
            memset(sRocketMultiplierSeen, 0, sizeof(sRocketMultiplierSeen));
            TF2Bridge_BeginRocketMeshFrame();
            {
                CBaseEntity* rocket = NULL;
                while ((rocket = gEntList.FindEntityByClassname(rocket, "tf_projectile_rocket")) != NULL)
                {
                    CTFBaseRocket* baseRocket = dynamic_cast<CTFBaseRocket*>(rocket);
                    if (baseRocket == NULL || !baseRocket->IsSolid())
                        continue;

                    if (TF2Bridge_TraceRocketAgainstBfbb(baseRocket))
                        continue;

                    const Vector pos = baseRocket->GetAbsOrigin();
                    const Vector attackerOrigin = pPlayer->WorldSpaceCenter();
                    const float rocketMultiplier = TF2Bridge_GetRocketMultiplier(
                        baseRocket->entindex(),
                        activeWeapon != NULL ? activeWeapon->GetBridgeKnockbackMultiplier() : 1.5f);
                    TF2Bridge_NotifyRocket(
                        baseRocket->entindex(),
                        &pos.x,
                        &attackerOrigin.x,
                        baseRocket->GetRadius(),
                        baseRocket->GetDamage(),
                        rocketMultiplier);
                }
            }
            TF2Bridge_EndRocketMeshFrame();
            for (int i = 0; i < BRIDGE_MAX_ROCKETS; ++i)
            {
                if (sRocketMultiplierEnt[i] != 0 && !sRocketMultiplierSeen[i])
                {
                    sRocketMultiplierEnt[i] = 0;
                    sRocketMultiplierValue[i] = 0.0f;
                }
            }

            TF2Bridge_NetSendIntent(&in);
        }

        if (bfbb_coll_draw.GetBool() && gpGlobals->curtime >= m_nextDraw)
        {
            m_nextDraw = gpGlobals->curtime + 0.5f;
            BFBBColl_DrawNear(pPlayer->GetAbsOrigin(), 2500.0f, 0.55f);
        }

        if (bfbb_bridge_debug.GetBool() && gpGlobals->curtime >= m_nextDebug)
        {
            m_nextDebug = gpGlobals->curtime + 1.0f;
            Msg("[bfbb] state: have=%d pos=(%.1f %.1f %.1f) yaw=%.0f hp=%d scene=%08x flags=%u coll=%08x owns=%d\n",
                (int)m_haveState, m_state.x, m_state.y, m_state.z, m_state.yaw, m_state.health,
                (unsigned)m_state.sceneId, (unsigned)m_state.flags, BFBBColl_LoadedScene(), (int)owns);
        }
    }

    // Where BFBB's player is right now, in Source space (false if unknown).
    bool GetBfbbPosition(Vector& out) const
    {
        if (!m_haveState || !(m_state.flags & BRIDGE_STATE_GAMEPLAY))
            return false;
        out = BfbbToSource(m_state.x, m_state.y, m_state.z, bfbb_unit_scale.GetFloat());
        return true;
    }

private:
    BridgeStatePacket m_state;
    bool m_haveState;
    float m_stateReceivedAt;
    float m_nextDebug;
    float m_nextDraw;
    unsigned int m_lastScene;
    bool m_pendingSnap;
};

static CTF2BridgeSystem g_TF2BridgeSystem;

// bfbb_goto: stand where BFBB's player is (snapped onto the mesh floor).
CON_COMMAND(bfbb_goto, "Teleport the host to BFBB's player position, snapped onto the mesh floor")
{
    CTFPlayer* pPlayer = ToTFPlayer(UTIL_GetListenServerHost());
    if (!pPlayer)
        return;

    Vector pos;
    if (!g_TF2BridgeSystem.GetBfbbPosition(pos))
    {
        Warning("[bfbb] no BFBB state yet -- is BFBB running in a level with BFBB_TF2BRIDGE set?\n");
        return;
    }

    pos = SnapToMeshFloor(pos, true);
    pos.z += 4.0f;
    pPlayer->SetMoveType(MOVETYPE_WALK);
    pPlayer->Teleport(&pos, NULL, &vec3_origin);
    Msg("[bfbb] moved to (%.0f %.0f %.0f)\n", pos.x, pos.y, pos.z);
}

CON_COMMAND(bfbb_coll_info, "Print where the BFBB mesh is relative to you")
{
    CTFPlayer* pPlayer = ToTFPlayer(UTIL_GetListenServerHost());
    if (!pPlayer)
        return;
    BFBBColl_PrintInfo(pPlayer->GetAbsOrigin());
}
