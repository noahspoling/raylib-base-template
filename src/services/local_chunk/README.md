# Local Chunk Service

`services/local_chunk` generates the fine-grained, walkable local map for a
single planet hex (the "Xkm2 chunk group for a hex" from the original
request). It is a **separate service from `services/world`**: it consumes
World's read-only query surface (`world_planet()`, `world_fields()`,
`world_seed()`) as plain parameters and has no `World*` in any of its own
signatures. Nothing under `services/world` knows this file exists;
`local_chunk_lua.c` is the only file that depends on both.

## Contents

- [Core idea: HEX = SQUARE](#core-idea-hex--square)
- [Determinism rules](#determinism-rules)
- [The pipeline](#the-pipeline)
- [Staggered borders](#staggered-borders)
- [File map](#file-map)
- [Debug views](#debug-views)
- [Known limitations](#known-limitations)
- [How to extend](#how-to-extend)

## Core idea: HEX = SQUARE

The hex is a topological abstraction that exists **only** at the planet
scale, to decide which square local-map chunks are adjacent and how their
shared border behaves. The square chunk itself is not clipped to any hex
shape -- it **is** the hex's entire walkable area, at a real-world target
scale of roughly 15-50km per hex/chunk (enough hexes to build up to an
earth-sized planet). The tangent-plane `extent` used throughout the code is
currently just an abstract unit; wiring it to an actual km scale is not done
yet.

An earlier version of this code point-in-polygon-clipped each square to the
hex's inscribed polygon and dimmed the outside corners. That was wrong --
removed. Don't reintroduce hex-shaped clipping of the local map.

## Determinism rules

The one property every stage in this pipeline must hold, because a hex can
be generated starting from any of its neighbors and border tiles need to
match up regardless of order:

- **Interior-only data** (nothing about a neighbor): pure function of
  `(world_seed, cell)`.
- **Border-shared data** (elevation control value, river crossing point,
  edge seed): pure function of the **unordered** cell pair
  `{min(A,B), max(A,B)}` -- symmetric, so cell A computing it and cell B
  computing it, in either order, with neither generated yet, produce
  bit-identical results.

Where this is applied in `local_chunk.c`:
- Per-cell seed: `hash3(cell, 0, 0, world_seed)`.
- Edge seed: `hash3(min(A,B), max(A,B), 0, world_seed)`.
- Elevation: not a per-cell or per-edge value at all -- see [The
  pipeline](#the-pipeline) below. It's a continuous function of raw world
  position, which is a stronger (and simpler) guarantee than symmetry over a
  pair.
- Shared-edge crossing point: `edge_midpoint()`, the midpoint of the two
  triangle-corner points common to both cells' corner rings -- a function of
  the unordered pair, not of "self" or "neighbor."

A quick way to break this without noticing: adding any per-cell mutable
cache that a neighbor's generation could observe before your own cell has
run. Don't.

## The pipeline

Mirrors `services/world`'s own stage-array pattern
(`world_pipeline.h`/`world_climate.c` etc.): `LocalChunkStage { name, run }`
registered in a `STAGES[]` array in `local_chunk.c`, executed in order by
`local_chunk_run()`. Adding a stage is one function + one array entry;
nothing else changes.

1. **Elevation** -- samples the active `WorldGenerator.sample()` (the same
   continuous, pure function of 3D position that produced the coarse
   per-hex elevation in `world_gen.c`) directly at each fine-grid point's
   actual world-space position (`generator_index` is threaded through from
   `world_generator_index()`). Two adjacent hexes sampling the same physical
   point get the identical value by construction -- no blending, no fading,
   no seam to get wrong. An earlier version of this stage built its own
   inverse-distance-weighted blend across a handful of control points with
   fine `fbm` detail faded to zero at the boundary; that's why hexes used to
   render as smooth "circle" blobs that didn't connect naturally across
   borders. Don't reintroduce a local approximation here -- if the
   generator's real terrain shape isn't detailed enough at this scale, add
   another CONTINUOUS layer (see next paragraph), not a local reblend.

   The generator's own `noise_scale` is tuned for continent-scale features
   across the whole sphere, so at the size of one hex it barely varies --
   sampling it alone gives smooth but too-simple coastlines (a near-straight
   line, not natural bays/coves). A second, higher-frequency `fbm` layer is
   added on top for local roughness, using the SAME rule as the coarse
   sample: a pure function of raw world position with a fixed seed
   (`world_seed + 9001`, distinguishing it from the coarse generator's own
   octave seeds), no per-cell or per-hex parameter at all -- so it's
   automatically seamless too, no fading needed. If borders still look too
   smooth/simple, tune `kDetailFreq`/`kDetailAmp`/`kDetailOctaves` in
   `stage_elevation` (or add a third such layer at a different frequency);
   don't go back to a per-cell-seeded version.
2. **Rivers** -- for each border the coarse hydrology pass flagged as
   carrying flow, draws a path between the actual inflow/outflow crossing
   points (never through the hex center unless this cell is a genuine
   source/sink), meandering via the same `fbm` noise the elevation stage
   uses, envelope-forced to exactly hit both crossing points.
3. **Biome context** -- stub. The extension point for "a mountain tile
   should look at its forest/desert/rainforest neighbors" -- not
   implemented; see [How to extend](#how-to-extend).

## Staggered borders

The hex is flat-top oriented (6 sides: N, NE, SE, S, SW, NW -- no direct
east/west side exists). N and S each border one neighbor with a **half-tile
stagger** (a "brick" offset); the 4 diagonal sides border their neighbors
cleanly, tile-boundary-to-tile-boundary.

- `local_chunk_north_frame()`: projects the sphere's actual polar axis (+Y)
  into a cell's tangent plane, giving a well-defined "local north" 2D
  direction everywhere except exactly at the 2 pole cells.
- `local_chunk_edge_role()`: classifies a neighbor slot as CLEAN or
  STAGGERED (N/S), picking whichever neighbor is angularly closest to local
  north/south. A sphere has no globally consistent "up" (hairy ball
  theorem), so this can't have perfect global meaning everywhere -- what it
  guarantees is **reciprocity**: a candidate staggered edge is only kept
  staggered if both sides independently agree, otherwise it's forced to
  CLEAN on both sides. `local_chunk_debug_check_reciprocity()` sweeps the
  whole planet and logs how often that fallback fires (see [Known
  limitations](#known-limitations) for the current number).
- `local_chunk_cross_border()`: the actual tile-coordinate remap when
  stepping across a border. Clean edges are 1:1. Staggered edges apply a
  half-tile offset, with a symmetric tie-break (lower cell id = phase 0, the
  other = phase 0.5) deciding who's out of phase. **Not generally
  invertible** -- crossing back from the destination column doesn't
  necessarily reproduce the original tile. That's inherent to a half-tile
  brick stagger (a brick doesn't have a single "opposite" brick across a
  running-bond seam either), not a bug.

## File map

| File | Role |
|---|---|
| `local_chunk.h` / `local_chunk.c` | The service itself: pipeline, edge roles, cross-border remap, debug renders. No `World*` anywhere. |
| `local_chunk_lua.h` / `local_chunk_lua.c` | Composition glue + Lua bindings (`gramarye.local_chunk.*`). The only place that imports both `services/world` and `services/local_chunk`. |
| `../world_noise.h` | Shared hash/value-noise primitives (also used by `world_gen.c`) -- local-chunk noise and world noise must use the identical hash, or a value computed on either side of a border could disagree. Don't fork a second copy. |

## Debug views

Three renderers, all debug-only (none is the final production tile
renderer):

- `local_chunk_draw_in_rect()` -- one hex's raw raster, auto-contrast
  stretched. Cheapest, least meaningful across hexes (see limitations).
- `local_chunk_draw_group_in_rect()` -- a whole `LocalChunkGroup`
  (center + neighbors) composited as a hex-polygon mosaic, absolute
  elevation color ramp. Useful for eyeballing cross-hex consistency; not
  wired into `planet.lua`'s UI by default (`LOCAL_CHUNK_GROUP_DEBUG_KIND`).
- `local_chunk_draw_square_in_rect()` -- one hex as a plain square grid,
  north-up, staggered edges tinted, sample crossings marked.
- `local_chunk_draw_connected_in_rect()` -- **the current default UI view**
  (`LOCAL_CHUNK_DEBUG_KIND`). Center square plus up to 6 neighbor squares in
  the standard flat-top-hex offset layout (N above, S below, 4 diagonals to
  either side, all the same size), contiguous by construction.

## Known limitations

Being tracked here rather than fixed silently -- read before assuming
either of these is resolved:

- **Reciprocity fallback rate is not negligible.** Logged once at startup;
  last measured 276/2700 (~10%) of candidate-staggered edges get forced to
  CLEAN because the two sides' independent north/south picks disagree. The
  north/south-picking heuristic in `pick_north_south_slot()` (nearest
  neighbor by angle) is prone to near-ties; tightening it is real follow-up
  work, not a rare edge case as originally assumed.
- **km scale isn't wired up.** `extent` is an abstract tangent-plane unit;
  nothing yet maps it to the intended ~15-50km per hex.
- **Biome context stage is an empty stub.**
- **Non-square (true flat-top aspect ratio) grid dimensions** and **actual
  gameplay traversal using `local_chunk_cross_border()`** are both
  explicitly out of scope so far.

## How to extend

**Add a pipeline stage**: write a `static void stage_foo(LocalChunkFields *f)`
in `local_chunk.c`, add `{ "Foo", stage_foo }` to the `STAGES[]` array. It
runs after every earlier stage, in array order, so it can read anything they
filled in (e.g. `f->elevation`, `f->river_mask`).

**Biome context (the actual next stage to write)**: read
`f->neighbors[i].terrain` / `.humidity` / `.temperature` for each of this
cell's neighbors and blend classification/detail near that specific border
accordingly -- e.g. a mountain hex bordering a rainforest hex should look
different near that edge than the same mountain hex bordering a desert.
`f->self_terrain` is this cell's own coarse classification (from
`world_gen.c`'s `tile_classify()`); this stage is where a FINER, per-tile
terrain gets derived instead of using the one coarse value everywhere. Since
`f->elevation` is now a real continuous surface, this is also where
elevation-*change*-driven rules belong -- e.g. "steep elevation change near
a coastline should be more cliff than beach": compute the gradient across
`f->elevation` near the shore and let that ratio drive the classification,
rather than a flat per-terrain rule. `f->neighbors[i].elevation` (the
neighbor's coarse value) is a cheap first signal for this before computing
an actual local gradient.

**Changing the noise/hash primitives**: edit `../world_noise.h` only, never
duplicate them into this file -- `world_gen.c` depends on the exact same
implementation.
