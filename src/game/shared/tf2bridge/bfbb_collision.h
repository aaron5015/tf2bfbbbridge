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

#ifdef GAME_DLL
// Server only: draws the triangles within `radius` of `center` as wire lines,
// for checking that the mesh is where you think it is.
void BFBBColl_DrawNear(const Vector& center, float radius, float duration);
#endif

#endif
