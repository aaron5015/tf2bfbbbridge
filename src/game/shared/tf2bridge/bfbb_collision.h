#ifndef BFBB_COLLISION_H
#define BFBB_COLLISION_H
#ifdef _WIN32
#pragma once
#endif

// BFBB level collision inside TF2.
//
// Loads the triangle file BFBB writes (bfbb_collision_<scene>.bfcl, see
// zTF2Bridge.cpp in the BFBB tree), converts it to Source space, builds a
// vphysics collision mesh, and lets player movement collide with it.
//
// Compiled into BOTH client.dll and server.dll: player movement is predicted on
// the client, so both need the same mesh.

#include "mathlib/vector.h"
#include "engine/IEngineTrace.h"

// Which scene's file is loaded (0 = none).
unsigned int BFBBColl_LoadedScene();

// Call from CTFGameMovement::TracePlayerBBox AFTER the normal trace has filled
// `pm`: traces the hull against the BFBB mesh and, if that hits sooner, replaces
// `pm` with it. Does nothing when no mesh is loaded.
void BFBBColl_MergeHullTrace(const Vector& start, const Vector& end, const Vector& mins,
    const Vector& maxs, trace_t& pm);

// Traces a thin line against the mesh (no player hull). Returns true on a hit,
// with the hit position and the surface normal.
bool BFBBColl_TraceLine(const Vector& from, const Vector& to, Vector& hitPos, Vector& normal);

#ifdef GAME_DLL
// Server only: draws the triangles nearest `center` (within `radius`) as wire
// lines: green = faces up (walkable), red = faces down, yellow = walls.
void BFBBColl_DrawNear(const Vector& center, float radius, float duration);

// Server only: prints what is loaded (bounds, triangle orientation counts) and
// traces straight down and up from `pos`, to find out where the mesh really is
// relative to the player.
void BFBBColl_PrintInfo(const Vector& pos);
#endif

#endif
