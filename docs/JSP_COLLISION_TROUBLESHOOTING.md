# JSP Collision Troubleshooting Notes

**Repository:** [aaron5015/tf2bfbbbridge](https://github.com/aaron5015/tf2bfbbbridge)  
**Working branch:** `tf2-knockback-prototype`  
**History checkpoint:** [`35f468d`](https://github.com/aaron5015/tf2bfbbbridge/commit/35f468d6af1396a59f4be43e4a04826447fece40)

## Why this note exists

Keep the JSP collision investigation and relevant changes easy to recover if a future change causes collision problems. Treat commits **through and including `35f468d`** as important reference history when investigating future JSP/collision regressions. Do not assume every commit in this range is necessarily required, and do not casually discard or rewrite this history.

The invisible cave/lip collision issue was eventually found to be absent after building both the client and server DLLs explicitly in Visual Studio 2022. Earlier testing may have used stale binaries because the usual Ctrl+B shortcut was not necessarily rebuilding both targets. Therefore, some of the attempted collision fixes may have been responding to a non-current build rather than a reproducible source defect.

## Known-good observations at the time of writing

- The invisible lip was no longer present after explicitly building both client and server.
- The tree crouch/uncrouch clearance behavior was tested and appeared to work.
- Current behavior appeared functional, so no blanket revert was warranted.
- This is a troubleshooting checkpoint, **not proof that every intermediate patch was necessary or independently validated**.

## Important diagnostic history

The cave diagnostics reported JSP scene `48423131`, with 21,452 triangles at scale 40.0. At the problem location, some spatial-chunk traces reported immediate hits (for example, chunk 204 in cell `(-1,1)`) while the whole-mesh reference trace was clear for the same sweep direction. Other directions hit in both paths.

That discrepancy was a useful lead, but the later discovery about stale client/server builds means those captures should not be treated as definitive evidence that the current source had a collision defect.

Collision investigations included:
- Synthetic-floor fallback trace/fraction handling.
- Spatial chunk triangle assignment and chunk sizing.
- Comparing chunk traces with whole-mesh traces.
- Testing whole-mesh and spatial-only movement trace paths.
- Additional collision diagnostics and visual overlays.
- Preserving the crouch-clearance safeguard that prevents standing up into overhead geometry.

Relevant commits previously discussed in the investigation include `10a806b`, `320178f`, `04729fe`, `8ed9cce`, `b4def62`, `7407727`, `51e8f3d`, and `e85a553`. These are breadcrumbs for future review, not a declaration that each is a necessary fix. Check the actual diff and branch history before reverting or cherry-picking any of them.

## Procedure before changing collision code again

1. **Verify the build first.** In Visual Studio 2022, explicitly build both the client and server configurations/targets. Do not rely on Ctrl+B unless you have confirmed it rebuilds both relevant DLLs.
2. Confirm the running game is loading the newly built DLLs. Check timestamps or add a temporary unmistakable diagnostic version marker if necessary.
3. Reproduce the issue on the current build before editing source.
4. Capture the full `bfbb_coll_info` output and note the exact location, movement direction, stance, and whether the symptom occurs in first and third person.
5. Compare spatial-chunk, combined, and whole-mesh results where available. Use overlays to locate the actual surface rather than changing chunk parameters by guesswork.
6. Change one thing at a time and test both client and server behavior.
7. Keep the crouch/uncrouch clearance regression test. Do not remove its safeguard simply because a different collision symptom disappears.
8. If the problem returns, start by comparing against checkpoint `35f468d` and inspect the intervening diffs. Preserve unrelated TF2 bridge work; avoid blanket-reverting the branch.

## Current conclusion

As of this checkpoint, the JSP behavior appears functional after rebuilding both client and server. Leave the current implementation alone unless a reproducible regression appears. Use this note and the commit history as a map for future diagnosis, not as a reason to preemptively revert the work.
