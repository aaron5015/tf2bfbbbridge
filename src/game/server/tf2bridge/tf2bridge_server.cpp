//========= BFBB <-> TF2 bridge, server side ================================
//
// Runs inside the TF2 listen server. Every server tick it
//   * reads the listen-server host's usercmd (movement axes, buttons, view
//     angles) plus class and health, and sends that to BFBB as an "intent";
//   * receives BFBB's player state and (optionally) teleports the TF2 player
//     there, so TF2 knows where the BFBB player really is.
//
// Console variables (all server-side):
//   bfbb_bridge        1/0   master switch (default 1)
//   bfbb_sync_pos      1/0   teleport the TF2 host to BFBB's position (default 1).
//                            Set 0 to walk with TF2 movement on the BFBB mesh instead.
//   bfbb_unit_scale    float Source units per BFBB unit (default 50)
//   bfbb_bridge_debug  1/0   print what is sent/received once a second
//   bfbb_coll_draw     1/0   draw the BFBB collision triangles near you
//   bfbb_goto          (command) move the host to where BFBB's player is now
//
//=============================================================================

#include "cbase.h"
#include "igamesystem.h"
#include "player.h"
#include "tf_player.h"
#include "usercmd.h"

#include "tf2bridge_net.h"
#include "tf2bridge/bfbb_collision.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static ConVar bfbb_bridge("bfbb_bridge", "1", FCVAR_NONE, "Enable the BFBB bridge");
static ConVar bfbb_sync_pos("bfbb_sync_pos", "1", FCVAR_NONE,
                            "Teleport the TF2 host to the BFBB player's position");
// Defined in game/shared/tf2bridge/bfbb_collision.cpp (shared with the client).
extern ConVar bfbb_unit_scale;
extern ConVar bfbb_scene;
static ConVar bfbb_coll_draw("bfbb_coll_draw", "0", FCVAR_NONE,
                             "Draw BFBB collision triangles near the host as yellow wires");
static ConVar bfbb_bridge_debug("bfbb_bridge_debug", "0", FCVAR_NONE, "Print bridge traffic");

// Source's default run speed for the usercmd axes (cl_forwardspeed / cl_sidespeed).
static const float kCmdAxisMax = 450.0f;

class CTF2BridgeSystem : public CAutoGameSystemPerFrame
{
public:
    CTF2BridgeSystem()
        : CAutoGameSystemPerFrame("TF2BridgeSystem"), m_haveState(false), m_nextDebug(0.0f), m_nextDraw(0.0f)
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
            m_state = st;
            m_haveState = true;

            // Tell both halves of the game (replicated) which level's collision to use.
            if ((st.flags & BRIDGE_STATE_GAMEPLAY) && st.sceneId != 0 &&
                (unsigned int)bfbb_scene.GetInt() != st.sceneId)
            {
                bfbb_scene.SetValue((int)st.sceneId);
            }
        }

        CTFPlayer* pPlayer = ToTFPlayer(UTIL_GetListenServerHost());
        if (!pPlayer || !pPlayer->IsAlive())
            return;

        // --- TF2 -> BFBB: intent ---------------------------------------------
        const CUserCmd* cmd = pPlayer->GetLastUserCommand();
        if (cmd)
        {
            BridgeIntentPacket in;
            memset(&in, 0, sizeof(in));
            in.forward = clamp(cmd->forwardmove / kCmdAxisMax, -1.0f, 1.0f);
            in.side = clamp(cmd->sidemove / kCmdAxisMax, -1.0f, 1.0f);
            in.pitch = cmd->viewangles.x;
            in.yaw = cmd->viewangles.y;
            in.buttons = (uint32_t)cmd->buttons; // IN_* bits match BRIDGE_IN_*
            in.tfclass = pPlayer->GetPlayerClass() ? pPlayer->GetPlayerClass()->GetClassIndex() : 0;
            in.tfhealth = pPlayer->GetHealth();
            TF2Bridge_NetSendIntent(&in);
        }

        // --- BFBB -> TF2: where the player really is ---------------------------
        if (!bfbb_sync_pos.GetBool() && pPlayer->GetMoveType() == MOVETYPE_NONE)
        {
            // Hand movement back to TF2 after a period of BFBB owning it.
            pPlayer->SetMoveType(MOVETYPE_WALK);
        }

        if (bfbb_coll_draw.GetBool() && gpGlobals->curtime >= m_nextDraw)
        {
            m_nextDraw = gpGlobals->curtime + 0.5f;
            BFBBColl_DrawNear(pPlayer->GetAbsOrigin(), 1500.0f, 0.55f);
        }

        if (m_haveState && bfbb_sync_pos.GetBool() && (m_state.flags & BRIDGE_STATE_GAMEPLAY))
        {
            // BFBB: x right, y up, z forward (left-handed). Source: x forward,
            // y left, z up. The non-mirroring match is (bz, -bx, by); see
            // zTF2Bridge.cpp, which uses the same mapping for directions.
            const float s = bfbb_unit_scale.GetFloat();
            Vector pos(m_state.z * s, -m_state.x * s, m_state.y * s);

            // BFBB owns movement; TF2 must not also move the player.
            pPlayer->SetMoveType(MOVETYPE_NONE);
            pPlayer->Teleport(&pos, NULL, &vec3_origin);
        }

        if (bfbb_bridge_debug.GetBool() && gpGlobals->curtime >= m_nextDebug)
        {
            m_nextDebug = gpGlobals->curtime + 1.0f;
            Msg("[bfbb] state: have=%d pos=(%.1f %.1f %.1f) yaw=%.0f hp=%d scene=%08x flags=%u coll=%08x\n",
                (int)m_haveState, m_state.x, m_state.y, m_state.z, m_state.yaw, m_state.health,
                (unsigned)m_state.sceneId, (unsigned)m_state.flags, BFBBColl_LoadedScene());
        }
    }

    // Where BFBB's player is right now, in Source space (false if unknown).
    bool GetBfbbPosition(Vector& out) const
    {
        if (!m_haveState || !(m_state.flags & BRIDGE_STATE_GAMEPLAY))
            return false;
        const float s = bfbb_unit_scale.GetFloat();
        out.Init(m_state.z * s, -m_state.x * s, m_state.y * s);
        return true;
    }

private:
    BridgeStatePacket m_state;
    bool m_haveState;
    float m_nextDebug;
    float m_nextDraw;
};

static CTF2BridgeSystem g_TF2BridgeSystem;

// bfbb_goto: stand where BFBB's player is, then walk with TF2 movement.
// Usually used with  bfbb_sync_pos 0.
CON_COMMAND(bfbb_goto, "Teleport the host to BFBB's player position")
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

    pos.z += 8.0f; // a little above the floor so the hull starts clear of it
    pPlayer->SetMoveType(MOVETYPE_WALK);
    pPlayer->Teleport(&pos, NULL, &vec3_origin);
    Msg("[bfbb] moved to (%.0f %.0f %.0f)\n", pos.x, pos.y, pos.z);
}
