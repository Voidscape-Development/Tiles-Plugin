# Tiles

A tiling scene transition for OBS Studio. The screen is divided into a lattice
of shapes that animate in sequence to carry you from one scene to the next.

Every shape tiles the plane edge to edge, so when the tiles reach full size
there is no uncovered pixel anywhere on the canvas — no seams, no background
showing through.

## Installing

Grab the build for your platform from the releases page and install it the way
OBS plugins are normally installed on that platform. Then add the transition in
OBS under **Scene Transitions → + → Tiling Transition**.

## Transition types

| Type | What it does |
| --- | --- |
| **Cover** | Coloured tiles build up until they fill the screen, the scene swaps underneath them, then they break up again. |
| **Keyhole** | The tiles are a mask: they open as windows onto the incoming scene and grow until it has taken over. **Invert mask** flips it, so the outgoing scene is what sits inside the shrinking tiles. |
| **Wave** | A solid band of coloured tiles sweeps across the canvas, leaving the incoming scene behind it. The band thickness is set in pixels. |
| **Dissolve – Crop Shrink** | Each tile crops the outgoing scene into a shrinking shape. The image stays put and gets cut away. |
| **Dissolve – Scale Shrink** | Each tile squeezes the outgoing scene down into a shrinking shape. The image shrinks with the tile. |

Both dissolves have an **Incoming scene arrives as tiles** option: instead of
the next scene simply sitting behind the transition, it grows back in tile by
tile, with the tile colour showing in the gap between the outgoing and incoming
shapes.

## Shapes

`Square`, `Circle`, `Hexagon` and `Triangle`.

Squares sit on a square lattice; hexagons and triangles on a triangular one;
all three close up perfectly with no overlap. Circles are the exception — no
arrangement of circles can tile a plane — so they are placed on the hexagonal
lattice and grow past their own cell, overlapping their neighbours until the
last gap closes. They reach full coverage at exactly the same moment the other
shapes do.

**Grid Rotation** turns the lattice itself, independently of the sweep
direction: 30° turns flat-top hexagons into pointy-top ones, a few degrees off
axis stops squares looking like a spreadsheet.

## Direction and origin

| Direction | Order the tiles take their turn |
| --- | --- |
| Inside Out | Nearest the origin first, working outwards |
| Outside In | Furthest from the origin first, closing in |
| Mirrored In | Two fronts start at opposite edges and meet at the origin |
| Mirrored Out | One front leaves the origin and travels both ways at once |
| Directional (angle) | A straight sweep along the chosen angle — 0° is left to right, 90° is top to bottom |

**Origin Centre X/Y** moves the point everything is measured from, and it can be
pushed outside the frame (−50% to 150%) for off-centre effects. The sweep is
always measured out to the furthest corner of the canvas as seen from wherever
the origin is, so an offset origin never cuts the transition short: the last
tile still finishes exactly as the transition ends.

## Other settings

- **Tile Size** — in pixels at your canvas resolution.
- **Tile Colour** / **Tile Colour (end of sweep)** — tiles fade between the two
  across the sweep. Set both the same for a flat colour.
- **Colour Variation** — randomly lightens and darkens each tile so the fill is
  not perfectly flat.
- **Per-Tile Duration** — how long a single tile's own animation lasts, as a
  share of the whole transition. Low values give a sharp travelling edge; 100%
  animates every tile at once. This is not the length of the transition itself,
  which OBS sets.
- **Order Randomness** — blends the ordered sweep towards a shuffle.
- **Easing** — linear, ease in and out, or ease out. This shapes the order tiles
  launch in, not the transition clock, so the wavefront still accelerates away
  and settles while every tile grows at the same steady rate.

If the transition looks choppy, the usual cause is that there are not enough
frames to animate in rather than anything in these settings: OBS's default 300 ms
at 60 fps is only 18 frames, and a tile set to 35% has about 5 of them to grow
in. Raising the transition duration in OBS buys every tile proportionally more
frames, and is the most effective single change.
- **Tiles leave in reverse order** (Cover) — the effect collapses back towards
  where it started instead of the wave carrying on through.

## Building

Standard OBS plugin template layout. See the
[plugin template wiki](https://github.com/obsproject/obs-plugintemplate/wiki)
for build environment requirements.

```sh
cmake --preset ubuntu-x86_64   # or windows-x64 / macos
cmake --build --preset ubuntu-x86_64
```

## Tests

`test/test-tiling.c` mirrors the lattice and coverage math from
`data/effects/tiles_transition.effect` and checks the two properties the
transition depends on: that each shape covers the canvas completely at full
scale (across tile sizes, grid rotations and origins, including origins outside
the frame), and that every tile's turn falls inside the transition so nothing is
left unfinished. It needs no OBS and no GPU:

```sh
cmake -S . -B build -DENABLE_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

`test/gpu-check.c` goes further and renders the effect on a real graphics
device, over every transition type, shape and direction. It paints the outgoing
scene red, the incoming scene blue and the tiles green, so anything else on
screen — most importantly a black pixel — means a gap or a bad sample. It needs
a graphics device, so it is kept out of the ctest suite:

```sh
cmake -S . -B build -DENABLE_GPU_TESTS=ON
cmake --build build
xvfb-run -a ./build/gpu-check data/effects/tiles_transition.effect
```

## Licence

GPL-2.0-or-later. See [LICENSE](LICENSE).
