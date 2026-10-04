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
// The axis mapping is a pure rotation, so triangle winding is unchanged. If the
// mesh turns out to be one-sided the wrong way (and bfbb_coll_twosided is 0),
// this swaps every triangle's winding.
ConVar bfbb_coll_flip("bfbb_coll_flip", "0", FCVAR_REPLICATED, "Swap triangle winding when loading");
// Add every triangle in both windings, so the mesh holds you from either side.
// Costs double the triangles, and removes any dependence on winding.
ConVar bfbb_coll_twosided("bfbb_coll_twosided", "1", FCVAR_REPLICATED, "Collide from both sides of every triangle");
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
static int s_loadedOptions = 0; // flip | twosided << 1, so changing either reloads
static CUtlVector<Vector> s_verts; // 3 per triangle, Source space
static Vector s_mins(0, 0, 0), s_maxs(0, 0, 0);
static int s_numUp = 0, s_numDown = 0, s_numSide = 0; // by (b-a)x(c-a), after the flip

// BFBB -> Source. librw flips X when it builds the view matrix, so BFBB's world
// is right-handed with +X toward screen-LEFT, +Y up, +Z forward; Source is x
// forward, y left, z up. They line up as a pure rotation, no mirroring:
//      Source = (bfbb.z, bfbb.x, bfbb.y) * scale
// (The first version used (z, -x, y), which mirrored the whole level.)
static inline Vector BfbbToSource(float x, float y, float z, float s)
{
    return Vector(z * s, x * s, y * s);
}

static void Unload()
{
    if (s_pCollide && physcollision)
        physcollision->DestroyCollide(s_pCollide);
    s_pCollide = NULL;
    s_loadedScene = 0;
    s_verts.Purge();
    s_numUp = s_numDown = s_numSide = 0;
}

static void Load(unsigned int scene)
{
    Unload();
    s_loadedScale = bfbb_unit_scale.GetFloat();
    s_loadedOptions = (bfbb_coll_flip.GetBool() ? 1 : 0) | (bfbb_coll_twosided.GetBool() ? 2 : 0);
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
    const bool twoSided = bfbb_coll_twosided.GetBool();

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
        if (twoSided)
            physcollision->PolysoupAddTriangle(soup, a, c, b, 0);

        Vector n = CrossProduct(b - a, c - a);
        VectorNormalize(n);
        if (n.z > 0.7f)
            s_numUp++;
        else if (n.z < -0.7f)
            s_numDown++;
        else
            s_numSide++;

        if (added == 0)
        {
            s_mins = s_maxs = a;
        }
        for (int k = 0; k < 3; k++)
        {
            const Vector& v = (k == 0) ? a : (k == 1) ? b : c;
            for (int ax = 0; ax < 3; ax++)
            {
                if (v[ax] < s_mins[ax]) s_mins[ax] = v[ax];
                if (v[ax] > s_maxs[ax]) s_maxs[ax] = v[ax];
            }
        }

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
    Msg("[bfbb] loaded %u collision triangles for scene %08x (scale %.1f); run bfbb_coll_info\n", added, scene, s);
}

// Reloads when the scene (or the scale) changed. Cheap when nothing did.
static CPhysCollide* GetCollide()
{
    if (!bfbb_coll_enable.GetBool())
        return NULL;

    const unsigned int want = (unsigned int)bfbb_scene.GetInt();
    const int options = (bfbb_coll_flip.GetBool() ? 1 : 0) | (bfbb_coll_twosided.GetBool() ? 2 : 0);
    if (want != s_loadedScene ||
        (s_pCollide && (s_loadedScale != bfbb_unit_scale.GetFloat() || s_loadedOptions != options)))
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

bool BFBBColl_TraceLine(const Vector& from, const Vector& to, Vector& hitPos, Vector& normal)
{
    CPhysCollide* pCollide = GetCollide();
    if (!pCollide)
        return false;

    Ray_t ray;
    ray.Init(from, to);

    trace_t tr;
    physcollision->TraceBox(ray, pCollide, vec3_origin, vec3_angle, &tr);
    if (tr.fraction >= 1.0f)
        return false;

    hitPos = from + (to - from) * tr.fraction;
    normal = tr.plane.normal;
    return true;
}

#ifdef GAME_DLL
struct NearTri_t
{
    float dist2;
    int index; // first vertex in s_verts
};

static int NearTriCompare(const void* a, const void* b)
{
    const float da = ((const NearTri_t*)a)->dist2;
    const float db = ((const NearTri_t*)b)->dist2;
    return (da < db) ? -1 : (da > db) ? 1 : 0;
}

void BFBBColl_DrawNear(const Vector& center, float radius, float duration)
{
    if (!GetCollide())
        return;

    // Collect every triangle in range, then draw the NEAREST ones. (Drawing
    // the first N in file order made the picture depend on which way you faced.)
    const float r2 = radius * radius;
    CUtlVector<NearTri_t> nearTris;
    for (int i = 0; i + 2 < s_verts.Count(); i += 3)
    {
        const float d = MIN(MIN((s_verts[i] - center).LengthSqr(), (s_verts[i + 1] - center).LengthSqr()),
            (s_verts[i + 2] - center).LengthSqr());
        if (d > r2)
            continue;
        NearTri_t t;
        t.dist2 = d;
        t.index = i;
        nearTris.AddToTail(t);
    }

    if (nearTris.Count() > 1)
        qsort(nearTris.Base(), nearTris.Count(), sizeof(NearTri_t), NearTriCompare);

    const int kMaxDraw = 450;
    for (int k = 0; k < nearTris.Count() && k < kMaxDraw; k++)
    {
        const int i = nearTris[k].index;
        const Vector& a = s_verts[i];
        const Vector& b = s_verts[i + 1];
        const Vector& c = s_verts[i + 2];

        Vector n = CrossProduct(b - a, c - a);
        VectorNormalize(n);

        int r = 255, g = 200, bl = 0; // walls: yellow
        if (n.z > 0.7f)
        {
            r = 0; g = 255; bl = 0; // faces up: green
        }
        else if (n.z < -0.7f)
        {
            r = 255; g = 40; bl = 40; // faces down: red
        }

        NDebugOverlay::Line(a, b, r, g, bl, true, duration);
        NDebugOverlay::Line(b, c, r, g, bl, true, duration);
        NDebugOverlay::Line(c, a, r, g, bl, true, duration);
    }
}

void BFBBColl_PrintInfo(const Vector& pos)
{
    if (!GetCollide())
    {
        Msg("[bfbb] no mesh loaded (scene cvar = %08x, dir = %s)\n", (unsigned)bfbb_scene.GetInt(),
            bfbb_collision_dir.GetString());
        return;
    }

    Msg("[bfbb] scene %08x: %d triangles, scale %.1f, flip %d, twosided %d\n", s_loadedScene,
        s_verts.Count() / 3, s_loadedScale, (int)bfbb_coll_flip.GetBool(), (int)bfbb_coll_twosided.GetBool());
    Msg("[bfbb] mesh bounds: (%.0f %.0f %.0f) to (%.0f %.0f %.0f)\n", s_mins.x, s_mins.y, s_mins.z,
        s_maxs.x, s_maxs.y, s_maxs.z);
    Msg("[bfbb] triangle normals: %d face up, %d face down, %d walls\n", s_numUp, s_numDown, s_numSide);
    Msg("[bfbb] you are at (%.0f %.0f %.0f)\n", pos.x, pos.y, pos.z);

    Vector hit, n;
    if (BFBBColl_TraceLine(pos, pos - Vector(0, 0, 6000), hit, n))
        Msg("[bfbb] mesh BELOW you: %.0f units down (z=%.0f), normal (%.2f %.2f %.2f)\n", pos.z - hit.z,
            hit.z, n.x, n.y, n.z);
    else
        Msg("[bfbb] mesh BELOW you: none within 6000 units\n");

    if (BFBBColl_TraceLine(pos, pos + Vector(0, 0, 6000), hit, n))
        Msg("[bfbb] mesh ABOVE you: %.0f units up (z=%.0f), normal (%.2f %.2f %.2f)\n", hit.z - pos.z,
            hit.z, n.x, n.y, n.z);
    else
        Msg("[bfbb] mesh ABOVE you: none within 6000 units\n");
}
#endif
