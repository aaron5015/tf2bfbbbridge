//========= BFBB collision mesh for TF2 player movement =====================
#include "cbase.h"
#include "bfbb_collision.h"

#include "vphysics_interface.h"
#include "utlvector.h"
#include "engine/IEngineTrace.h"
#include "filesystem.h"

#ifdef GAME_DLL
#include "debugoverlay_shared.h"
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

extern IPhysicsCollision* physcollision;
extern IPhysicsSurfaceProps* physprops;

// ---- settings (replicated so a listen-server client sees the host's values) ----

// Source units per BFBB unit. Shared with the server-side bridge.
ConVar bfbb_unit_scale("bfbb_unit_scale", "50", FCVAR_REPLICATED,
                       "Source units per BFBB world unit");
// Which BFBB scene is loaded. The server writes this from the bridge; the
// replicated value tells the client to load the same file.
ConVar bfbb_scene("bfbb_scene", "0", FCVAR_REPLICATED, "BFBB scene id (set by the bridge)");
ConVar bfbb_collision_dir("bfbb_collision_dir", "D:/bfbbpc/bfbb/bin/here", FCVAR_REPLICATED,
                          "Folder holding bfbb_collision_<scene>.bfcl (use forward slashes)");
// BFBB is left-handed and Source right-handed, so the axis mapping mirrors the
// mesh and flips every triangle's winding. This swaps it back. If walking on the
// mesh feels like walking on the inside of it, flip this.
ConVar bfbb_coll_flip("bfbb_coll_flip", "1", FCVAR_REPLICATED, "Swap triangle winding when loading");
ConVar bfbb_coll_enable("bfbb_coll_enable", "1", FCVAR_REPLICATED, "Collide player movement with the BFBB mesh");

#pragma pack(push, 1)
struct BfclTri
{
    float v[9];
    unsigned char flags;
    unsigned char platData;
    unsigned short matIndex;
};
#pragma pack(pop)

static CPhysCollide* s_pCollide = NULL;
static unsigned int s_loadedScene = 0;
static float s_loadedScale = 0.0f;
static CUtlVector<Vector> s_verts; // 3 per triangle, Source space

// BFBB (x right, y up, z forward, left-handed) -> Source (x forward, y left, z up).
static inline Vector BfbbToSource(float x, float y, float z, float s)
{
    return Vector(z * s, -x * s, y * s);
}

static void Unload()
{
    if (s_pCollide && physcollision)
        physcollision->DestroyCollide(s_pCollide);
    s_pCollide = NULL;
    s_loadedScene = 0;
    s_verts.Purge();
}

static void Load(unsigned int scene)
{
    Unload();
    s_loadedScale = bfbb_unit_scale.GetFloat();
    if (scene == 0 || !physcollision)
        return;

    char path[512];
    Q_snprintf(path, sizeof(path), "%s/bfbb_collision_%08x.bfcl", bfbb_collision_dir.GetString(), scene);

    FileHandle_t f = g_pFullFileSystem->Open(path, "rb");
    if (f == FILESYSTEM_INVALID_HANDLE)
    {
        Warning("[bfbb] cannot open %s (set bfbb_collision_dir)\n", path);
        return;
    }

    char magic[4];
    unsigned int version = 0, fileScene = 0, count = 0;
    float bounds[6];
    if (g_pFullFileSystem->Read(magic, sizeof(magic), f) != sizeof(magic) ||
        memcmp(magic, "BFCL", 4) != 0 ||
        g_pFullFileSystem->Read(&version, sizeof(version), f) != sizeof(version) ||
        version != 1 ||
        g_pFullFileSystem->Read(&fileScene, sizeof(fileScene), f) != sizeof(fileScene) ||
        g_pFullFileSystem->Read(&count, sizeof(count), f) != sizeof(count) ||
        g_pFullFileSystem->Read(bounds, sizeof(bounds), f) != sizeof(bounds))
    {
        Warning("[bfbb] %s is not a valid BFCL v1 file\n", path);
        g_pFullFileSystem->Close(f);
        return;
    }

    const float s = s_loadedScale;
    const bool flip = bfbb_coll_flip.GetBool();

    CPhysPolysoup* soup = physcollision->PolysoupCreate();
    unsigned int added = 0;
    s_verts.EnsureCapacity(count * 3);

    for (unsigned int i = 0; i < count; i++)
    {
        BfclTri t;
        if (g_pFullFileSystem->Read(&t, sizeof(t), f) != sizeof(t))
            break;

        Vector a = BfbbToSource(t.v[0], t.v[1], t.v[2], s);
        Vector b = BfbbToSource(t.v[3], t.v[4], t.v[5], s);
        Vector c = BfbbToSource(t.v[6], t.v[7], t.v[8], s);
        if (flip)
        {
            Vector tmp = b;
            b = c;
            c = tmp;
        }

        physcollision->PolysoupAddTriangle(soup, a, b, c, 0);
        s_verts.AddToTail(a);
        s_verts.AddToTail(b);
        s_verts.AddToTail(c);
        added++;
    }
    g_pFullFileSystem->Close(f);

    s_pCollide = physcollision->ConvertPolysoupToCollide(soup, true);
    physcollision->PolysoupDestroy(soup);

    if (!s_pCollide)
    {
        Warning("[bfbb] vphysics refused the mesh (%u triangles)\n", added);
        s_verts.Purge();
        return;
    }

    s_loadedScene = scene;
    Msg("[bfbb] loaded %u collision triangles for scene %08x (scale %.1f)\n", added, scene, s);
}

// Reloads when the scene (or the scale) changed. Cheap when nothing did.
static CPhysCollide* GetCollide()
{
    if (!bfbb_coll_enable.GetBool())
        return NULL;

    const unsigned int want = (unsigned int)bfbb_scene.GetInt();
    if (want != s_loadedScene || (s_pCollide && s_loadedScale != bfbb_unit_scale.GetFloat()))
        Load(want);

    return s_pCollide;
}

unsigned int BFBBColl_LoadedScene()
{
    return s_loadedScene;
}

void BFBBColl_MergeHullTrace(const Vector& start, const Vector& end, const Vector& mins,
                             const Vector& maxs, trace_t& pm)
{
    CPhysCollide* pCollide = GetCollide();
    if (!pCollide)
        return;

    Ray_t ray;
    ray.Init(start, end, mins, maxs);

    trace_t tr;
    physcollision->TraceBox(ray, pCollide, vec3_origin, vec3_angle, &tr);

    // A hull that already starts inside the mesh would pin the player forever;
    // let the normal trace decide in that case.
    if (tr.startsolid || tr.allsolid || tr.fraction >= pm.fraction)
        return;

    Vector normal = tr.plane.normal;
    const Vector move = end - start;
    // The hit plane must face the player. The winding of an imported mesh is
    // easy to get backwards, so trust the direction of travel instead.
    if (DotProduct(normal, move) > 0.0f)
        normal = -normal;

    pm = tr;
    pm.startpos = start;
    pm.endpos = start + move * pm.fraction;
    pm.plane.normal = normal;
    pm.plane.dist = DotProduct(normal, pm.endpos);
    pm.plane.type = 3;
    pm.plane.signbits = 0;
    pm.contents = CONTENTS_SOLID;
    pm.hitgroup = 0;
    pm.hitbox = 0;
    pm.physicsbone = 0;
    pm.surface.surfaceProps = physprops ? physprops->GetSurfaceIndex("default") : 0;
    pm.surface.flags = 0;
#ifdef CLIENT_DLL
    pm.m_pEnt = ClientEntityList().GetBaseEntity(0);
#else
    pm.m_pEnt = CBaseEntity::Instance(INDEXENT(0));
#endif
}

#ifdef GAME_DLL
void BFBBColl_DrawNear(const Vector& center, float radius, float duration)
{
    if (!GetCollide())
        return;

    const float r2 = radius * radius;
    int drawn = 0;
    for (int i = 0; i + 2 < s_verts.Count() && drawn < 250; i += 3)
    {
        const Vector& a = s_verts[i];
        const Vector& b = s_verts[i + 1];
        const Vector& c = s_verts[i + 2];
        if ((a - center).LengthSqr() > r2 && (b - center).LengthSqr() > r2 && (c - center).LengthSqr() > r2)
            continue;

        NDebugOverlay::Line(a, b, 255, 200, 0, true, duration);
        NDebugOverlay::Line(b, c, 255, 200, 0, true, duration);
        NDebugOverlay::Line(c, a, 255, 200, 0, true, duration);
        drawn++;
    }
}
#endif
