# Tiles

Scene transitions for OBS Studio. The module provides two, both generated on the
GPU with no bundled video:

- **Tiling Transition** — the screen is divided into a lattice of shapes that
  animate in sequence to carry you from one scene to the next.
- **Vortex Transition** — a spiral portal tears open over the outgoing scene,
  swallows the canvas, and lets the incoming one back in through the smoke.

## Installing

Grab the build for your platform from the releases page and install it the way
OBS plugins are normally installed on that platform. Then add the transition in
OBS under **Scene Transitions → +**.

# Tiling Transition

Every shape tiles the plane edge to edge, so when the tiles reach full size
there is no uncovered pixel anywhere on the canvas — no seams, no background
showing through.

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

`Square`, `Circle`, `Hexagon`, `Triangle` and `Diamond`.

Squares sit on a square lattice; hexagons, triangles and diamonds on a
triangular one; all four close up perfectly with no overlap. A diamond is a
triangle joined to its own mirror image along the base, so it points up and
down: at the same tile size it is as wide as a triangle and √3 times taller
than it is wide. Circles are the exception — no arrangement of circles can tile
a plane — so they are placed on the hexagonal lattice and grow past their own
cell, overlapping their neighbours until the last gap closes. They reach full
coverage at exactly the same moment the other shapes do.

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

# Vortex Transition

A spiral portal, generated per pixel rather than played back from a video file,
so it scales to any canvas, recolours to your palette, and takes its speed from
whatever duration OBS is set to.

It runs in three phases:

1. **Open** — a portal tears open at the centre over the outgoing scene, its rim
   ragged and lit, with rays dragged inwards towards the hole. It grows until it
   has swallowed the canvas.
2. **Hold** — a full screen spiral turns. This is where the scenes are swapped,
   under an overlay that is opaque from edge to edge, so the cut never shows.
3. **Reveal** — the incoming scene erodes back in from the centre through dark
   smoke tendrils.

The arms come from sampling noise in log-polar space: a constant angular offset
per unit of log(radius) is a logarithmic spiral, so the arms fall out of the
coordinate system rather than being warped into place afterwards.

## Settings

| Setting | What it does |
| --- | --- |
| **Spiral Arms** | How many arms. Low counts read as a few broad sweeps, high counts as fine filaments. |
| **Twist** | How tightly the arms wind in. 0 makes them straight spokes. |
| **Spin** | Turns the vortex makes over the whole transition; negative spins the other way. Driven by the transition clock rather than wall time, so it looks the same every play. |
| **Detail** | Noise frequency along the arms. |
| **Edge Roughness** | How far the opening portal's rim is torn up. 0 gives a clean circle. The tear scales with the portal, so it stays the same fraction of the rim the whole way out. |
| **Intensity** | Overall brightness of the generated light. |
| **Core Colour** | What the hot centres blow out to when the vortex flashes. |
| **Core Bleed** / **Core Blend** | How far the core colour reaches down into the arms when the vortex flashes, and how abruptly it takes over. 50% on both is the original look; turn the bleed up for a wide white-hot flash, down to keep the core colour to the brightest points. |
| **Vortex Colour** | The body of the arms. |
| **Vortex Colours** / **Colour Mix** | The body can mix up to four colours instead of one. Leave the count at 1 for a single flat colour; raise it and colours 2–4 appear alongside a **Colour Mix** setting for how they are spread — *Along the arms* drifts between them on the arms themselves, so different arms and different stretches of one arm take different colours; *Centre to edge* runs the palette out from the eye as a gradient; *Banded arms* is the same field snapped to flat bands of one colour each. |
| **Centre X/Y** | Where the portal opens. Can be pushed outside the frame (−50% to 150%); the portal is always sized to the furthest corner from wherever it sits, so an offset centre never leaves a corner of the outgoing scene showing when the scenes swap. |
| **Portal Open** / **Reveal Start** | The phase split, as a share of the transition. Reveal Start is held above Portal Open so the hold is never empty. |
| **Smoke Scale** / **Smoke Softness** | Size and edge width of the tendrils the incoming scene comes back through. |
| **Swirl the scenes into the vortex** | Off by default. Screws the outgoing scene down into the centre and unwinds the incoming one back out of it, instead of leaving both flat behind the vortex. The first and last frame are untouched either way. |

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
left unfinished.

`test/test-vortex.c` does the same for `data/effects/vortex_transition.effect`:
that the transition starts on an untouched outgoing scene and ends on an
untouched incoming one for every pixel and every setting, that the portal is
opaque edge to edge for the whole hold so the scene swap cannot show, that the
noise wrapped around the circle actually meets itself and leaves no seam, and
that the portal is always sized past the furthest corner of the canvas.

It also covers the bounds the shader uses to skip work: the portal boundary
never leaves the band the radius test assumes it is in, and the reveal really is
finished below its low bound and untouched above its high one, so the pixels
those tests throw away had nothing to contribute. The colour path is checked the
same way — a single colour comes back exactly as it went in, a mix never invents
a colour outside the two entries it sits between, and both core sliders land on
the values the look was tuned at when left at their defaults.

Neither needs OBS or a GPU:

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
