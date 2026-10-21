//========= BFBB collision mesh for TF2 player movement =====================
#include "cbase.h"
#include "bfbb_collision.h"

#include "vphysics_interface.h"
#include "utlvector.h"
#include "engine/IEngineTrace.h"
#include "filesystem.h"
#include "tier0/platform.h"

#ifdef GAME_DLL
#include "debugoverlay_shared.h"
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

extern IPhysicsCollision* physcollision;
extern IPhysicsSurfaceProps* physprops;

// ---- settings (replicated so a listen-server client sees the host's values) ----

// Source units per BFBB unit. Shared with the server-side bridge.
ConVar bfbb_unit_scale("bfbb_unit_scale", "40", FCVAR_REPLICATED,
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
ConVar bfbb_coll_profile("bfbb_coll_profile", "0", FCVAR_REPLICATED,
    "Log timing for BFBB mesh movement traces once per second");

#pragma pack(push, 1)
struct BfclTri
{
    float v[9];
    unsigned char flags;
    unsigned char platData;
    unsigned short matIndex;
};
#pragma pack(pop)

// s_pCollide holds oversized triangles (or the whole-mesh fallback).
static CPhysCollide* s_pCollide = NULL;
static CUtlVector<CPhysCollide*> s_chunkCollides;
static CUtlVector<int> s_chunkGrid;
static int s_chunkGridWidth = 0;
static int s_chunkGridHeight = 0;
static float s_chunkGridOriginX = 0.0f;
static float s_chunkGridOriginY = 0.0f;
// Larger chunks reduce artificial open-edge collision artifacts from splitting
// continuous JSP surfaces into many independently cooked VPhysics meshes.
static const float kCollisionChunkSize = 8192.0f;
static const int kMaxCollisionGridCells = 65536;
static const int kMaxCellsPerTriangle = 64;
static unsigned int s_loadedScene = 0;
static float s_loadedScale = 0.0f;
static int s_loadedOptions = 0; // flip | twosided << 1, so changing either reloads
static CUtlVector<Vector> s_verts; // 3 per triangle, Source space
static Vector s_mins(0, 0, 0), s_maxs(0, 0, 0);
static int s_numUp = 0, s_numDown = 0, s_numSide = 0; // by (b-a)x(c-a), after the flip

// Diagnostic snapshot from the actual movement merge on this DLL.
static bool s_haveLastMovementTrace = false;
static Vector s_lastMoveStart(0, 0, 0), s_lastMoveEnd(0, 0, 0);
static Vector s_lastMoveMins(0, 0, 0), s_lastMoveMaxs(0, 0, 0);
static float s_lastMoveInputFraction = 1.0f, s_lastMoveMeshFraction = 1.0f;
static bool s_lastMoveStartSolid = false, s_lastMoveAllSolid = false;
static Vector s_lastMoveNormal(0, 0, 0), s_lastMoveHitPos(0, 0, 0);
static unsigned int s_lastMoveTraceCount = 0;
static const char* const kBfbbCollisionDiagnosticVersion = "diag-2026-10-20-a";

static void AddTriangleToSoup(CPhysPolysoup* soup, const Vector& a, const Vector& b, const Vector& c)
{
    physcollision->PolysoupAddTriangle(soup, a, b, c, 0);
    if (bfbb_coll_twosided.GetBool())
        physcollision->PolysoupAddTriangle(soup, a, c, b, 0);
}

static void DestroyCollisionChunks()
{
    for (int i = 0; i < s_chunkCollides.Count(); ++i)
    {
        if (s_chunkCollides[i] && physcollision)
            physcollision->DestroyCollide(s_chunkCollides[i]);
    }
    s_chunkCollides.Purge();
    s_chunkGrid.Purge();
    s_chunkGridWidth = s_chunkGridHeight = 0;
}

static bool BuildCollisionChunks()
{
    DestroyCollisionChunks();
    if (s_verts.Count() < 3 || !physcollision)
        return false;

    const int minCellX = (int)floor(s_mins.x / kCollisionChunkSize);
    const int minCellY = (int)floor(s_mins.y / kCollisionChunkSize);
    const int maxCellX = (int)floor(s_maxs.x / kCollisionChunkSize);
    const int maxCellY = (int)floor(s_maxs.y / kCollisionChunkSize);
    const int width = maxCellX - minCellX + 1;
    const int height = maxCellY - minCellY + 1;
    const bool useGrid = width > 0 && height > 0 && width <= kMaxCollisionGridCells / height;

    s_chunkGridOriginX = (float)minCellX * kCollisionChunkSize;
    s_chunkGridOriginY = (float)minCellY * kCollisionChunkSize;

    if (!useGrid)
    {
        // Extremely large scenes fall back to one collide rather than risking
        // an oversized dense lookup grid.
        CPhysPolysoup* soup = physcollision->PolysoupCreate();
        for (int i = 0; i + 2 < s_verts.Count(); i += 3)
            AddTriangleToSoup(soup, s_verts[i], s_verts[i + 1], s_verts[i + 2]);
        s_pCollide = physcollision->ConvertPolysoupToCollide(soup, true);
        physcollision->PolysoupDestroy(soup);
        return s_pCollide != NULL;
    }

    s_chunkGridWidth = width;
    s_chunkGridHeight = height;
    const int cellCount = width * height;
    s_chunkGrid.SetCount(cellCount);
    CUtlVector<CPhysPolysoup*> soups;
    soups.SetCount(cellCount);
    for (int i = 0; i < cellCount; ++i)
    {
        s_chunkGrid[i] = -1;
        soups[i] = NULL;
    }

    CPhysPolysoup* globalSoup = NULL;
    for (int i = 0; i + 2 < s_verts.Count(); i += 3)
    {
        const Vector& a = s_verts[i];
        const Vector& b = s_verts[i + 1];
        const Vector& c = s_verts[i + 2];
        // Keep nearby triangles in each chunk as a collision halo. A chunk
        // built from only triangles whose bounds touch its cell has open mesh
        // edges at cell boundaries; VPhysics can report those exposed edges as
        // false walls/floors even though the complete mesh has no collision.
        // The halo keeps adjacent surface triangles together around player hulls.
        const float chunkHalo = 64.0f;
        const float triMinX = MIN(a.x, MIN(b.x, c.x)) - chunkHalo;
        const float triMaxX = MAX(a.x, MAX(b.x, c.x)) + chunkHalo;
        const float triMinY = MIN(a.y, MIN(b.y, c.y)) - chunkHalo;
        const float triMaxY = MAX(a.y, MAX(b.y, c.y)) + chunkHalo;
        const int x0 = MAX(0, (int)floor(triMinX / kCollisionChunkSize) - minCellX);
        const int x1 = MIN(width - 1, (int)floor(triMaxX / kCollisionChunkSize) - minCellX);
        const int y0 = MAX(0, (int)floor(triMinY / kCollisionChunkSize) - minCellY);
        const int y1 = MIN(height - 1, (int)floor(triMaxY / kCollisionChunkSize) - minCellY);
        const long long cells = (long long)(x1 - x0 + 1) * (y1 - y0 + 1);

        if (cells <= 0 || cells > kMaxCellsPerTriangle)
        {
            if (!globalSoup)
                globalSoup = physcollision->PolysoupCreate();
            AddTriangleToSoup(globalSoup, a, b, c);
            continue;
        }

        for (int y = y0; y <= y1; ++y)
        {
            for (int x = x0; x <= x1; ++x)
            {
                const int cell = y * width + x;
                if (!soups[cell])
                    soups[cell] = physcollision->PolysoupCreate();
                AddTriangleToSoup(soups[cell], a, b, c);
            }
        }
    }

    bool ok = true;
    for (int i = 0; i < cellCount; ++i)
    {
        if (!soups[i])
            continue;
        CPhysCollide* collide = physcollision->ConvertPolysoupToCollide(soups[i], true);
        physcollision->PolysoupDestroy(soups[i]);
        soups[i] = NULL;
        if (!collide)
        {
            ok = false;
            break;
        }
        s_chunkGrid[i] = s_chunkCollides.AddToTail(collide);
    }

    if (globalSoup)
    {
        s_pCollide = physcollision->ConvertPolysoupToCollide(globalSoup, true);
        physcollision->PolysoupDestroy(globalSoup);
        if (!s_pCollide)
            ok = false;
    }

    for (int i = 0; i < cellCount; ++i)
    {
        if (soups[i])
            physcollision->PolysoupDestroy(soups[i]);
    }

    if (!ok)
    {
        // Preserve the old all-at-once collision path if a scene cannot be
        // represented as spatial chunks on this VPhysics build.
        DestroyCollisionChunks();
        if (s_pCollide)
        {
            physcollision->DestroyCollide(s_pCollide);
            s_pCollide = NULL;
        }
        CPhysPolysoup* fallbackSoup = physcollision->PolysoupCreate();
        for (int i = 0; i + 2 < s_verts.Count(); i += 3)
            AddTriangleToSoup(fallbackSoup, s_verts[i], s_verts[i + 1], s_verts[i + 2]);
        s_pCollide = physcollision->ConvertPolysoupToCollide(fallbackSoup, true);
        physcollision->PolysoupDestroy(fallbackSoup);
        return s_pCollide != NULL;
    }
    return s_pCollide != NULL || s_chunkCollides.Count() > 0;
}

static bool TraceSpatialChunksOnly(const Ray_t& ray, const Vector& start, const Vector& end,
    const Vector& mins, const Vector& maxs, trace_t& result,
    bool* encounteredStartSolid = NULL, trace_t* firstStartSolidTrace = NULL)
{
    result = trace_t();
    result.fraction = 1.0f;
    result.startpos = start;
    result.endpos = end;
    bool traced = false;
    bool hadStartSolid = false;
    trace_t startSolidResult;

    if (s_chunkGridWidth <= 0 || s_chunkGridHeight <= 0)
        return false;

    const float qMinX = MIN(start.x + mins.x, end.x + mins.x);
    const float qMaxX = MAX(start.x + maxs.x, end.x + maxs.x);
    const float qMinY = MIN(start.y + mins.y, end.y + mins.y);
    const float qMaxY = MAX(start.y + maxs.y, end.y + maxs.y);
    const int x0 = MAX(0, (int)floor((qMinX - s_chunkGridOriginX) / kCollisionChunkSize));
    const int x1 = MIN(s_chunkGridWidth - 1, (int)floor((qMaxX - s_chunkGridOriginX) / kCollisionChunkSize));
    const int y0 = MAX(0, (int)floor((qMinY - s_chunkGridOriginY) / kCollisionChunkSize));
    const int y1 = MIN(s_chunkGridHeight - 1, (int)floor((qMaxY - s_chunkGridOriginY) / kCollisionChunkSize));
    if (x0 > x1 || y0 > y1)
        return false;

    for (int y = y0; y <= y1; ++y)
    {
        for (int x = x0; x <= x1; ++x)
        {
            const int chunkIndex = s_chunkGrid[y * s_chunkGridWidth + x];
            if (chunkIndex < 0 || !s_chunkCollides[chunkIndex])
                continue;
            trace_t tr;
            physcollision->TraceBox(ray, s_chunkCollides[chunkIndex], vec3_origin, vec3_angle, &tr);
            traced = true;
            if (tr.startsolid || tr.allsolid)
            {
                if (!hadStartSolid)
                    startSolidResult = tr;
                hadStartSolid = true;
                continue;
            }
            if (tr.fraction < result.fraction)
                result = tr;
        }
    }
    if (result.fraction >= 1.0f && hadStartSolid)
        result = startSolidResult;
    if (encounteredStartSolid)
        *encounteredStartSolid = hadStartSolid;
    if (firstStartSolidTrace && hadStartSolid)
        *firstStartSolidTrace = startSolidResult;
    return traced;
}

static bool TraceCollisionChunks(const Vector& start, const Vector& end, const Vector& mins,
    const Vector& maxs, trace_t& result, bool* encounteredStartSolid = NULL,
    trace_t* firstStartSolidTrace = NULL)
{
    Ray_t ray;
    ray.Init(start, end, mins, maxs);

    result.fraction = 1.0f;
    result.startsolid = false;
    result.allsolid = false;
    result.startpos = start;
    result.endpos = end;

    bool traced = false;
    bool hadStartSolid = false;
    trace_t startSolidResult;
    if (s_pCollide)
    {
        trace_t tr;
        physcollision->TraceBox(ray, s_pCollide, vec3_origin, vec3_angle, &tr);
        traced = true;
        if (tr.startsolid || tr.allsolid)
        {
            hadStartSolid = true;
            startSolidResult = tr;
        }
        else
        {
            result = tr;
        }
    }

    if (s_chunkGridWidth <= 0 || s_chunkGridHeight <= 0)
    {
        if (hadStartSolid)
            result = startSolidResult;
        if (encounteredStartSolid)
            *encounteredStartSolid = hadStartSolid;
        if (firstStartSolidTrace && hadStartSolid)
            *firstStartSolidTrace = startSolidResult;
        return traced;
    }

    const float qMinX = MIN(start.x + mins.x, end.x + mins.x);
    const float qMaxX = MAX(start.x + maxs.x, end.x + maxs.x);
    const float qMinY = MIN(start.y + mins.y, end.y + mins.y);
    const float qMaxY = MAX(start.y + maxs.y, end.y + maxs.y);
    const int x0 = MAX(0, (int)floor((qMinX - s_chunkGridOriginX) / kCollisionChunkSize));
    const int x1 = MIN(s_chunkGridWidth - 1, (int)floor((qMaxX - s_chunkGridOriginX) / kCollisionChunkSize));
    const int y0 = MAX(0, (int)floor((qMinY - s_chunkGridOriginY) / kCollisionChunkSize));
    const int y1 = MIN(s_chunkGridHeight - 1, (int)floor((qMaxY - s_chunkGridOriginY) / kCollisionChunkSize));
    if (x0 > x1 || y0 > y1)
    {
        if (hadStartSolid)
            result = startSolidResult;
        if (encounteredStartSolid)
            *encounteredStartSolid = hadStartSolid;
        if (firstStartSolidTrace && hadStartSolid)
            *firstStartSolidTrace = startSolidResult;
        return traced;
    }

    for (int y = y0; y <= y1; ++y)
    {
        for (int x = x0; x <= x1; ++x)
        {
            const int chunkIndex = s_chunkGrid[y * s_chunkGridWidth + x];
            if (chunkIndex < 0)
                continue;
            trace_t tr;
            physcollision->TraceBox(ray, s_chunkCollides[chunkIndex], vec3_origin, vec3_angle, &tr);
            traced = true;
            if (tr.startsolid || tr.allsolid)
            {
                if (!hadStartSolid)
                    startSolidResult = tr;
                hadStartSolid = true;
                continue;
            }
            if (tr.fraction < result.fraction)
                result = tr;
        }
    }

    // A startsolid result from one chunk must not hide a valid sweep hit from
    // another chunk. Preserve the legacy escape behavior only when no chunk
    // produced a non-startsolid hit at all.
    if (result.fraction >= 1.0f && hadStartSolid)
        result = startSolidResult;
    if (encounteredStartSolid)
        *encounteredStartSolid = hadStartSolid;
    if (firstStartSolidTrace && hadStartSolid)
        *firstStartSolidTrace = startSolidResult;
    return traced;
}

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
    DestroyCollisionChunks();
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

    if (!BuildCollisionChunks())
    {
        Warning("[bfbb] vphysics refused the mesh or one of its spatial chunks (%u triangles)\n", added);
        Unload();
        s_verts.Purge();
        return;
    }

    s_loadedScene = scene;
    Msg("[bfbb] loaded %u collision triangles in %d spatial chunks for scene %08x (scale %.1f); run bfbb_coll_info\n",
        added, s_chunkCollides.Count() + (s_pCollide ? 1 : 0), scene, s);
}

// Reloads when the scene (or the scale) changed. Cheap when nothing did.
static bool GetCollisionSet()
{
    if (!bfbb_coll_enable.GetBool())
        return false;

    const unsigned int want = (unsigned int)bfbb_scene.GetInt();
    const int options = (bfbb_coll_flip.GetBool() ? 1 : 0) | (bfbb_coll_twosided.GetBool() ? 2 : 0);
    if (want != s_loadedScene ||
        ((s_pCollide || s_chunkCollides.Count() > 0) &&
            (s_loadedScale != bfbb_unit_scale.GetFloat() || s_loadedOptions != options)))
        Load(want);

    return s_pCollide != NULL || s_chunkCollides.Count() > 0;
}

unsigned int BFBBColl_LoadedScene()
{
    return s_loadedScene;
}

#ifdef GAME_DLL
static CPhysCollide* BuildMonolithicProbeCollide()
{
    if (!physcollision || s_verts.Count() < 3)
        return NULL;

    CPhysPolysoup* soup = physcollision->PolysoupCreate();
    if (!soup)
        return NULL;
    for (int i = 0; i + 2 < s_verts.Count(); i += 3)
        AddTriangleToSoup(soup, s_verts[i], s_verts[i + 1], s_verts[i + 2]);
    CPhysCollide* collide = physcollision->ConvertPolysoupToCollide(soup, true);
    physcollision->PolysoupDestroy(soup);
    return collide;
}

static void PrintCollisionTraceSources(const Vector& start, const Vector& end,
    const Vector& mins, const Vector& maxs, const char* label)
{
    Ray_t ray;
    ray.Init(start, end, mins, maxs);
    if (s_pCollide)
    {
        trace_t tr;
        memset(&tr, 0, sizeof(tr));
        tr.fraction = 1.0f;
        physcollision->TraceBox(ray, s_pCollide, vec3_origin, vec3_angle, &tr);
        if (tr.startsolid || tr.allsolid || tr.fraction < 1.0f)
        {
            Msg("[bfbb] %s source=oversized-triangle collide f=%.3f ss=%d as=%d n=(%.2f %.2f %.2f)\n",
                label, tr.fraction, tr.startsolid ? 1 : 0, tr.allsolid ? 1 : 0,
                tr.plane.normal.x, tr.plane.normal.y, tr.plane.normal.z);
        }
    }

    if (s_chunkGridWidth <= 0 || s_chunkGridHeight <= 0)
        return;
    const float qMinX = MIN(start.x + mins.x, end.x + mins.x);
    const float qMaxX = MAX(start.x + maxs.x, end.x + maxs.x);
    const float qMinY = MIN(start.y + mins.y, end.y + mins.y);
    const float qMaxY = MAX(start.y + maxs.y, end.y + maxs.y);
    const int x0 = MAX(0, (int)floor((qMinX - s_chunkGridOriginX) / kCollisionChunkSize));
    const int x1 = MIN(s_chunkGridWidth - 1, (int)floor((qMaxX - s_chunkGridOriginX) / kCollisionChunkSize));
    const int y0 = MAX(0, (int)floor((qMinY - s_chunkGridOriginY) / kCollisionChunkSize));
    const int y1 = MIN(s_chunkGridHeight - 1, (int)floor((qMaxY - s_chunkGridOriginY) / kCollisionChunkSize));
    for (int y = y0; y <= y1; ++y)
    {
        for (int x = x0; x <= x1; ++x)
        {
            const int chunkIndex = s_chunkGrid[y * s_chunkGridWidth + x];
            if (chunkIndex < 0)
                continue;
            trace_t tr;
            memset(&tr, 0, sizeof(tr));
            tr.fraction = 1.0f;
            physcollision->TraceBox(ray, s_chunkCollides[chunkIndex], vec3_origin, vec3_angle, &tr);
            if (tr.startsolid || tr.allsolid || tr.fraction < 1.0f)
            {
                Msg("[bfbb] %s source=chunk cell=(%d,%d) collide=%d f=%.3f ss=%d as=%d n=(%.2f %.2f %.2f)\n",
                    label, x + (int)floor(s_chunkGridOriginX / kCollisionChunkSize),
                    y + (int)floor(s_chunkGridOriginY / kCollisionChunkSize), chunkIndex,
                    tr.fraction, tr.startsolid ? 1 : 0, tr.allsolid ? 1 : 0,
                    tr.plane.normal.x, tr.plane.normal.y, tr.plane.normal.z);
            }
        }
    }
}

static void PrintPlayerHullProbe(const Vector& pos, const Vector& mins, const Vector& maxs,
    const char* label, CPhysCollide* monolithicCollide)
{
    Ray_t restRay;
    restRay.Init(pos, pos, mins, maxs);
    trace_t tr;
    memset(&tr, 0, sizeof(tr));
    tr.fraction = 1.0f;
    const bool traced = TraceSpatialChunksOnly(restRay, pos, pos, mins, maxs, tr);
    trace_t combinedRest;
    memset(&combinedRest, 0, sizeof(combinedRest));
    combinedRest.fraction = 1.0f;
    const bool combinedTraced = TraceCollisionChunks(pos, pos, mins, maxs, combinedRest);
    trace_t wholeRest;
    memset(&wholeRest, 0, sizeof(wholeRest));
    wholeRest.fraction = 1.0f;
    if (monolithicCollide)
        physcollision->TraceBox(restRay, monolithicCollide, vec3_origin, vec3_angle, &wholeRest);
    Msg("[bfbb] %s hull at rest: LIVE-SPATIAL traced=%d ss=%d as=%d f=%.3f; COMBINED traced=%d ss=%d as=%d f=%.3f; WHOLE ss=%d as=%d f=%.3f\n",
        label, traced ? 1 : 0, tr.startsolid ? 1 : 0, tr.allsolid ? 1 : 0, tr.fraction,
        combinedTraced ? 1 : 0, combinedRest.startsolid ? 1 : 0, combinedRest.allsolid ? 1 : 0,
        combinedRest.fraction, wholeRest.startsolid ? 1 : 0, wholeRest.allsolid ? 1 : 0, wholeRest.fraction);

    static const Vector dirs[] = {
        Vector(1, 0, 0), Vector(-1, 0, 0), Vector(0, 1, 0), Vector(0, -1, 0),
        Vector(0.7071068f, 0.7071068f, 0), Vector(0.7071068f, -0.7071068f, 0),
        Vector(-0.7071068f, 0.7071068f, 0), Vector(-0.7071068f, -0.7071068f, 0),
        Vector(0, 0, -1), Vector(0, 0, 1)
    };
    static const char* names[] = { "+x", "-x", "+y", "-y", "+x+y", "+x-y", "-x+y", "-x-y", "-z", "+z" };
    const float distance = 16.0f;
    for (int i = 0; i < ARRAYSIZE(dirs); ++i)
    {
        const Vector end = pos + dirs[i] * distance;
        Ray_t ray;
        ray.Init(pos, end, mins, maxs);
        trace_t sweep;
        memset(&sweep, 0, sizeof(sweep));
        sweep.fraction = 1.0f;
        const bool chunkTraced = TraceSpatialChunksOnly(ray, pos, end, mins, maxs, sweep);
        trace_t combinedSweep;
        memset(&combinedSweep, 0, sizeof(combinedSweep));
        combinedSweep.fraction = 1.0f;
        const bool combinedTraced = TraceCollisionChunks(pos, end, mins, maxs, combinedSweep);
        trace_t wholeSweep;
        memset(&wholeSweep, 0, sizeof(wholeSweep));
        wholeSweep.fraction = 1.0f;
        if (monolithicCollide)
            physcollision->TraceBox(ray, monolithicCollide, vec3_origin, vec3_angle, &wholeSweep);
        if (!chunkTraced && !monolithicCollide)
            continue;
        if (sweep.startsolid || sweep.allsolid || sweep.fraction < 1.0f ||
            combinedSweep.startsolid || combinedSweep.allsolid || combinedSweep.fraction < 1.0f ||
            wholeSweep.startsolid || wholeSweep.allsolid || wholeSweep.fraction < 1.0f)
        {
            const Vector chunkHit = pos + (end - pos) * sweep.fraction;
            const Vector wholeHit = pos + (end - pos) * wholeSweep.fraction;
            Msg("[bfbb] %s sweep %s: LIVE-SPATIAL f=%.4f ss=%d as=%d hit=(%.1f %.1f %.1f) n=(%.2f %.2f %.2f); COMBINED traced=%d f=%.4f ss=%d as=%d; WHOLE f=%.4f ss=%d as=%d hit=(%.1f %.1f %.1f) n=(%.2f %.2f %.2f)\n",
                label, names[i], sweep.fraction, sweep.startsolid ? 1 : 0, sweep.allsolid ? 1 : 0,
                chunkHit.x, chunkHit.y, chunkHit.z, sweep.plane.normal.x, sweep.plane.normal.y,
                sweep.plane.normal.z, combinedTraced ? 1 : 0, combinedSweep.fraction,
                combinedSweep.startsolid ? 1 : 0, combinedSweep.allsolid ? 1 : 0,
                wholeSweep.fraction, wholeSweep.startsolid ? 1 : 0, wholeSweep.allsolid ? 1 : 0,
                wholeHit.x, wholeHit.y, wholeHit.z, wholeSweep.plane.normal.x,
                wholeSweep.plane.normal.y, wholeSweep.plane.normal.z);
            // Cyan = probe path; red = live spatial hit/normal; green = monolithic hit;
            // magenta = disagreement between the live chunk path and whole-mesh reference.
            NDebugOverlay::Line(pos, end, 0, 200, 255, true, 12.0f);
            if (sweep.startsolid || sweep.allsolid || sweep.fraction < 1.0f) {
                NDebugOverlay::Cross(chunkHit, 5.0f, 255, 40, 40, true, 12.0f);
                NDebugOverlay::Line(chunkHit, chunkHit + sweep.plane.normal * 24.0f, 255, 40, 40, true, 12.0f);
            }
            if (wholeSweep.startsolid || wholeSweep.allsolid || wholeSweep.fraction < 1.0f) {
                NDebugOverlay::Cross(wholeHit, 5.0f, 40, 255, 40, true, 12.0f);
                NDebugOverlay::Line(wholeHit, wholeHit + wholeSweep.plane.normal * 24.0f, 40, 255, 40, true, 12.0f);
            }
            if ((sweep.fraction < 1.0f) != (wholeSweep.fraction < 1.0f) ||
                fabsf(sweep.fraction - wholeSweep.fraction) > 0.02f)
                NDebugOverlay::Line(chunkHit, wholeHit, 255, 0, 255, true, 12.0f);
            PrintCollisionTraceSources(pos, end, mins, maxs, label);
        }
    }
}
#endif

// A hull can report allsolid because it overlaps a wall or overhang while
// gravity is moving it down. Only synthesize a floor stop when a nearby
// downward point trace confirms a walkable face beneath the player's feet.
static bool HasWalkableFloorNear(const Vector& pos, Vector& floorPos, Vector& floorNormal)
{
    const Vector rayStart = pos + Vector(0.0f, 0.0f, 8.0f);
    const Vector rayEnd = pos - Vector(0.0f, 0.0f, 24.0f);
    trace_t floorTrace;
    memset(&floorTrace, 0, sizeof(floorTrace));
    floorTrace.fraction = 1.0f;
    if (s_pCollide)
    {
        // In whole-mesh isolation mode, floor corroboration must use the same
        // monolithic mesh as movement. Consulting chunk meshes here can still
        // manufacture a false floor from an artificial chunk boundary.
        Ray_t floorRay;
        floorRay.Init(rayStart, rayEnd, vec3_origin, vec3_origin);
        physcollision->TraceBox(floorRay, s_pCollide, vec3_origin, vec3_angle, &floorTrace);
    }
    else if (!TraceCollisionChunks(rayStart, rayEnd, vec3_origin, vec3_origin, floorTrace))
    {
        return false;
    }
    if (floorTrace.startsolid || floorTrace.allsolid || floorTrace.fraction >= 1.0f)
        return false;

    floorPos = rayStart + (rayEnd - rayStart) * floorTrace.fraction;
    floorNormal = floorTrace.plane.normal;
    return floorNormal.z >= 0.7f && floorPos.z >= pos.z - 8.0f && floorPos.z <= pos.z + 4.0f;
}

void BFBBColl_MergeHullTrace(const Vector& start, const Vector& end, const Vector& mins,
    const Vector& maxs, trace_t& pm)
{
    if (!GetCollisionSet())
        return;

    trace_t tr;
    const bool profile = bfbb_coll_profile.GetBool();
    const double traceStart = profile ? Plat_FloatTime() : 0.0;
    bool encounteredStartSolid = false;
    trace_t startSolidTrace;

    // Diagnostic isolation mode: use the original monolithic JSP collision for
    // movement traces. This bypasses independently cooked spatial chunks while
    // preserving the zero-length start-solid behavior needed by CanUnduck().
    Ray_t spatialRay;
    spatialRay.Init(start, end, mins, maxs);
    const bool traced = TraceSpatialChunksOnly(spatialRay, start, end, mins, maxs, tr,
        &encounteredStartSolid, &startSolidTrace);

    // Whole-mesh-only isolation mode intentionally skips chunk-hit verification.

    if (profile)
    {
        static double windowStart = 0.0;
        static double windowTotalMs = 0.0;
        static double windowMaxMs = 0.0;
        static unsigned int windowCalls = 0;
        static unsigned int windowHits = 0;
        static unsigned int windowStartSolid = 0;
        const double now = Plat_FloatTime();
        const double elapsedMs = (now - traceStart) * 1000.0;
        if (windowStart == 0.0)
            windowStart = now;
        windowTotalMs += elapsedMs;
        if (elapsedMs > windowMaxMs)
            windowMaxMs = elapsedMs;
        ++windowCalls;
        if (traced && (tr.startsolid || tr.allsolid || tr.fraction < 1.0f))
            ++windowHits;
        if (encounteredStartSolid)
            ++windowStartSolid;

        if (now - windowStart >= 1.0)
        {
#ifdef CLIENT_DLL
            const char* realm = "client";
#else
            const char* realm = "server";
#endif
            Msg("[bfbb] %s mesh trace: %u calls, %u contacts, %u startsolid, %.3f ms total, %.3f ms avg, %.3f ms max (1s)\n",
                realm, windowCalls, windowHits, windowStartSolid, windowTotalMs,
                windowCalls ? windowTotalMs / windowCalls : 0.0, windowMaxMs);
            windowStart = now;
            windowTotalMs = 0.0;
            windowMaxMs = 0.0;
            windowCalls = 0;
            windowHits = 0;
            windowStartSolid = 0;
        }

        static double lastStartSolidSampleTime = 0.0;
        static Vector lastNonStartSolidStart(0.0f, 0.0f, 0.0f);
        if (encounteredStartSolid && now - lastStartSolidSampleTime >= 1.0)
        {
#ifdef CLIENT_DLL
            const char* realm = "client";
#else
            const char* realm = "server";
#endif
            Msg("[bfbb] %s startsolid sample: allsolid=%d fraction=%.3f left=%.3f last-clear-start=(%.1f %.1f %.1f) start=(%.1f %.1f %.1f) end=(%.1f %.1f %.1f)\n",
                realm, (int)startSolidTrace.allsolid, startSolidTrace.fraction,
                startSolidTrace.fractionleftsolid, lastNonStartSolidStart.x, lastNonStartSolidStart.y,
                lastNonStartSolidStart.z, start.x, start.y, start.z, end.x, end.y, end.z);
            lastStartSolidSampleTime = now;
        }
        if (!encounteredStartSolid)
            lastNonStartSolidStart = start;
    }

    const Vector move = end - start;
    s_haveLastMovementTrace = true;
    ++s_lastMoveTraceCount;
    s_lastMoveStart = start;
    s_lastMoveEnd = end;
    s_lastMoveMins = mins;
    s_lastMoveMaxs = maxs;
    s_lastMoveInputFraction = pm.fraction;
    s_lastMoveMeshFraction = tr.fraction;
    s_lastMoveStartSolid = tr.startsolid;
    s_lastMoveAllSolid = tr.allsolid;
    s_lastMoveNormal = tr.plane.normal;
    s_lastMoveHitPos = start + move * tr.fraction;

    // CanUnduck and other position-clearance checks use a zero-length trace.
    // Preserve the JSP overlap on the merged trace so the standing hull cannot
    // expand into overhead geometry while the player is crouched.
    if ((tr.startsolid || tr.allsolid) && move.LengthSqr() <= 0.0001f)
    {
        pm.startsolid = true;
        pm.allsolid = tr.allsolid;
        pm.fraction = 0.0f;
        pm.startpos = start;
        pm.endpos = start;
        pm.contents = CONTENTS_SOLID;
        return;
    }

    // A hull fully embedded in the mesh has no usable entry plane. Ignore
    // stationary and lateral overlaps so the player can escape, but prevent a
    // downward move from continuing through the geometry.
    Vector corroboratingFloorPos;
    Vector corroboratingFloorNormal;
    if (tr.allsolid && move.z < -0.01f && pm.fraction > 0.0f &&
        HasWalkableFloorNear(start, corroboratingFloorPos, corroboratingFloorNormal))
    {
        // The hull trace can be allsolid because of an overlapping wall or
        // overhang, while the point trace identifies the supporting face.
        // Use that face's slope and the actual crossing fraction; reporting
        // an immediate horizontal stop creates an invisible lip.
        const float planeDist = DotProduct(corroboratingFloorNormal, corroboratingFloorPos);
        float hullSupport = 0.0f;
        hullSupport += corroboratingFloorNormal.x * (corroboratingFloorNormal.x >= 0.0f ? mins.x : maxs.x);
        hullSupport += corroboratingFloorNormal.y * (corroboratingFloorNormal.y >= 0.0f ? mins.y : maxs.y);
        hullSupport += corroboratingFloorNormal.z * (corroboratingFloorNormal.z >= 0.0f ? mins.z : maxs.z);
        const float startDistance = DotProduct(corroboratingFloorNormal, start) + hullSupport - planeDist;
        const float endDistance = DotProduct(corroboratingFloorNormal, end) + hullSupport - planeDist;
        if (startDistance <= 0.0f || endDistance >= 0.0f)
            return;
        const float floorFraction = startDistance / (startDistance - endDistance);
        if (floorFraction < 0.0f || floorFraction > 1.0f)
            return;

        static double lastFallbackLogTime = 0.0;
        const double now = Plat_FloatTime();
        if (now - lastFallbackLogTime >= 1.0)
        {
            Msg("[bfbb] synthetic floor fallback: start=(%.1f %.1f %.1f) end=(%.1f %.1f %.1f) confirmed-floor=(%.1f %.1f %.1f)\n",
                start.x, start.y, start.z, end.x, end.y, end.z,
                corroboratingFloorPos.x, corroboratingFloorPos.y, corroboratingFloorPos.z);
            lastFallbackLogTime = now;
        }
        pm = tr;
        pm.startsolid = false;
        pm.allsolid = false;
        pm.fraction = floorFraction;
        pm.fractionleftsolid = 0.0f;
        pm.startpos = start;
        pm.endpos = start + move * floorFraction;
        pm.plane.normal = corroboratingFloorNormal;
        pm.plane.dist = planeDist;
        pm.plane.type = corroboratingFloorNormal.z == 1.0f ? 2 : 3;
        pm.plane.signbits = (corroboratingFloorNormal.x < 0.0f ? 1 : 0) |
            (corroboratingFloorNormal.y < 0.0f ? 2 : 0) |
            (corroboratingFloorNormal.z < 0.0f ? 4 : 0);
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
        return;
    }

    // A hull that starts partly inside the mesh has no reliable entry plane;
    // let the normal trace decide unless another chunk provided a clean hit.
    if (tr.startsolid || tr.allsolid || tr.fraction >= pm.fraction)
        return;

    Vector normal = tr.plane.normal;
    // With two-sided triangles, VPhysics returns the normal of the side it
    // actually hit. Do not reverse it to oppose movement: doing so can turn a
    // cave roof or wall into an upward-facing, walkable plane.
    if (!bfbb_coll_twosided.GetBool() && DotProduct(normal, move) > 0.0f)
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
    if (!GetCollisionSet())
        return false;

    trace_t tr;
    if (!TraceCollisionChunks(from, to, vec3_origin, vec3_origin, tr) || tr.fraction >= 1.0f)
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
    if (!GetCollisionSet())
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
    if (!GetCollisionSet())
    {
        Msg("[bfbb] no mesh loaded (scene cvar = %08x, dir = %s)\n", (unsigned)bfbb_scene.GetInt(),
            bfbb_collision_dir.GetString());
        return;
    }

    Msg("[bfbb] COLLISION DIAGNOSTICS %s | live merges on this DLL=%u | chunks=%d grid=%dx%d chunkSize=%.0f oversizedCollide=%s\\n",
        kBfbbCollisionDiagnosticVersion, s_lastMoveTraceCount, s_chunkCollides.Count(),
        s_chunkGridWidth, s_chunkGridHeight, kCollisionChunkSize, s_pCollide ? "yes" : "no");
#ifdef CLIENT_DLL
    Msg("[bfbb] diagnostic realm=CLIENT (predicted movement)\\n");
#else
    Msg("[bfbb] diagnostic realm=SERVER (authoritative movement)\\n");
#endif
    if (s_haveLastMovementTrace)
        Msg("[bfbb] LAST LIVE MERGE start=(%.2f %.2f %.2f) end=(%.2f %.2f %.2f) mins=(%.1f %.1f %.1f) maxs=(%.1f %.1f %.1f) inputFrac=%.4f meshFrac=%.4f meshSS=%d meshAS=%d meshHit=(%.2f %.2f %.2f) meshNormal=(%.3f %.3f %.3f)\\n",
            s_lastMoveStart.x, s_lastMoveStart.y, s_lastMoveStart.z,
            s_lastMoveEnd.x, s_lastMoveEnd.y, s_lastMoveEnd.z,
            s_lastMoveMins.x, s_lastMoveMins.y, s_lastMoveMins.z,
            s_lastMoveMaxs.x, s_lastMoveMaxs.y, s_lastMoveMaxs.z,
            s_lastMoveInputFraction, s_lastMoveMeshFraction,
            s_lastMoveStartSolid ? 1 : 0, s_lastMoveAllSolid ? 1 : 0,
            s_lastMoveHitPos.x, s_lastMoveHitPos.y, s_lastMoveHitPos.z,
            s_lastMoveNormal.x, s_lastMoveNormal.y, s_lastMoveNormal.z);
    else
        Msg("[bfbb] no live movement merge recorded on this DLL yet\\n");

    Msg("[bfbb] scene %08x: %d triangles, scale %.1f, flip %d, twosided %d\n", s_loadedScene,
        s_verts.Count() / 3, s_loadedScale, (int)bfbb_coll_flip.GetBool(), (int)bfbb_coll_twosided.GetBool());
    Msg("[bfbb] mesh bounds: (%.0f %.0f %.0f) to (%.0f %.0f %.0f)\n", s_mins.x, s_mins.y, s_mins.z,
        s_maxs.x, s_maxs.y, s_maxs.z);
    Msg("[bfbb] triangle normals: %d face up, %d face down, %d walls\n", s_numUp, s_numDown, s_numSide);
    Msg("[bfbb] you are at (%.0f %.0f %.0f)\n", pos.x, pos.y, pos.z);
    if (s_chunkGridWidth > 0 && s_chunkGridHeight > 0)
    {
        const int cellX = (int)floor((pos.x - s_chunkGridOriginX) / kCollisionChunkSize);
        const int cellY = (int)floor((pos.y - s_chunkGridOriginY) / kCollisionChunkSize);
        const float cellMinX = s_chunkGridOriginX + cellX * kCollisionChunkSize;
        const float cellMaxX = cellMinX + kCollisionChunkSize;
        const float cellMinY = s_chunkGridOriginY + cellY * kCollisionChunkSize;
        const float cellMaxY = cellMinY + kCollisionChunkSize;
        Msg("[bfbb] PLAYER GRID CELL local=(%d,%d) arrayIndex=%d chunkIndex=%d boundsX=[%.0f,%.0f] boundsY=[%.0f,%.0f] edgeDistances=(xMin %.1f xMax %.1f yMin %.1f yMax %.1f)\\n",
            cellX + (int)floor(s_chunkGridOriginX / kCollisionChunkSize),
            cellY + (int)floor(s_chunkGridOriginY / kCollisionChunkSize),
            cellY * s_chunkGridWidth + cellX,
            (cellX >= 0 && cellX < s_chunkGridWidth && cellY >= 0 && cellY < s_chunkGridHeight)
                ? s_chunkGrid[cellY * s_chunkGridWidth + cellX] : -1,
            cellMinX, cellMaxX, cellMinY, cellMaxY,
            pos.x - cellMinX, cellMaxX - pos.x, pos.y - cellMinY, cellMaxY - pos.y);
    }

    // A vertical point ray can't reveal the side/overhang contacts that can
    // prevent a standing player hull from moving. Probe both stock TF2 hulls
    // and short horizontal sweeps so snag reports include a useful plane.
    CPhysCollide* monolithicProbe = BuildMonolithicProbeCollide();
    Msg("[bfbb] diagnostic whole-mesh collide: %s\n", monolithicProbe ? "ready" : "unavailable");
    PrintPlayerHullProbe(pos, Vector(-24, -24, 0), Vector(24, 24, 82), "standing", monolithicProbe);
    PrintPlayerHullProbe(pos, Vector(-24, -24, 0), Vector(24, 24, 62), "crouched", monolithicProbe);
    if (monolithicProbe)
        physcollision->DestroyCollide(monolithicProbe);

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
