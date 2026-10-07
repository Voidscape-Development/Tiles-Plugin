# Tiles

Scene transitions for OBS Studio. The module provides five, all generated on
the GPU with no bundled video:

- **Tiling Transition** — the screen is divided into a lattice of shapes that
  animate in sequence to carry you from one scene to the next.
- **Vortex Transition** — a spiral portal tears open over the outgoing scene,
  swallows the canvas, and lets the incoming one back in through the smoke.
- **Glass Transition** — the outgoing scene cracks from a point of impact,
  holds a beat, and the shards fall away to leave the incoming one.
- **Matte Cutout Transition** — one big shape cuts the outgoing scene away,
  growing out of a point or closing in on it, with optional border, echo
  bands and glow riding its edge for a stinger look.
- **Portal Transition** — a portal is shot open over the outgoing scene with
  the incoming one showing through it, then grows to swallow the canvas.

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

| Shape | What it is |
| --- | --- |
| **Square** | A square lattice. |
| **Hexagon** | Pointy-top hexagons on a triangular lattice. |
| **Triangle** | Equilateral triangles, two per lattice rhombus. |
| **Diamond** | A triangle joined to its own mirror image along the base, so it points up and down. At the same tile size it is as wide as a triangle and √3 times taller than it is wide. |
| **Brick** | Rectangles twice as wide as they are tall, every other row slid half a brick along. Running bond, and no aligned columns anywhere. |
| **Octagon** | Regular octagons on a square lattice. Octagons cannot close the plane alone; what they leave at each lattice corner is a small square standing on its point, and that is a tile in its own right, so the sweep carries two tile sizes at once. |
| **Cairo Pentagon** | The Cairo tiling: pentagons with two right angles and three of 120°, four to a turn around each four-fold point. |
| **Mosaic** | Squares that quarter themselves, twice, on their own hash, mixing three tile sizes in one sweep. Large slabs and fine detail arrive together. |
| **Shatter** | The Voronoi cells of a jittered lattice — irregular, organic cells that break the canvas up like glass rather than a grid. |
| **Circle** | The one shape that cannot tile. |

Everything but `Circle` closes up perfectly with no overlap, whatever the tile
size, grid rotation or origin. Circles are the exception — no arrangement of
circles can tile a plane — so they are placed on the hexagonal lattice and grow
past their own cell, overlapping their neighbours until the last gap closes.
They reach full coverage at exactly the same moment the other shapes do.

Every tile grows by scaling about its own centre, so the area covered tracks
the square of the scale for all of them, mixed tile sizes included: `Mosaic`
and `Octagon` finish in step with the rest rather than leaving their small
tiles behind.

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

# Glass Transition

The outgoing scene is a pane of glass. It cracks from wherever it was struck,
the break races out to the corners, and then the shards let go.

Unlike the other two, this one is not a full screen shader. The pane is a real
mesh of triangles, cut on the CPU and flown by its vertex shader, because a
shard that leaves the frame, turns over or sails past the camera is not
something a fragment shader can work out from the pixel it lands on. The shards
are drawn back to front, which is enough: falling glass is flat planes that
never intersect.

## Break styles

| Style | What it does |
| --- | --- |
| **Window Break** | The shards let go and fall away under gravity, with a push outwards from the impact. The classic broken window. |
| **Blow Outward** | The shards blast away from the impact and past the camera, growing as they pass it. |
| **Crack Wipe** | Nothing moves. The cracks spread and the incoming scene arrives shard by shard behind the front, the cracks fading with the scene they belonged to. |
| **Shatter Out, Assemble In** | Two breaks crossing: the outgoing scene leaves as shards while the incoming one flies in as shards and lands, the last of them seating exactly on the final frame. The scene behind is held down so a shard that has not landed yet reads as missing. |

## Crack patterns

| Pattern | What it is |
| --- | --- |
| **Spiderweb** | Radial spokes crossed by concentric rings, spaced geometrically so the shards grow as they get further from the impact — fine splintering where it was struck, big slabs out at the edges. Both fall out of the one ratio. |
| **Voronoi** | The cells of a jittered lattice of seeds: irregular angular shards with no focal point. The same construction as the `Shatter` tile shape, resolved into real polygons rather than sampled per pixel. |
| **Cracked Ice** | The canvas cut by straight cracks, each piece split again until the pieces are down to size. No lattice, so the shards come out in a wide range of sizes and long splinters appear on their own. The cuts are steered by the impact, so the break still reads as having a source. |

Whichever pattern is picked, the shards are an exact partition of the canvas:
every pixel belongs to one shard and no pixel belongs to two. Every cell is
built by cutting a convex region with half planes, and two cells that share a
cut share the line it was computed from, so there is no gap for the incoming
scene to show through before the shards have moved.

## Timing

The transition runs in three phases, and every shard's flight fits inside it:

1. **Crack** — the front races out from the impact. The break is sized to the
   furthest corner as seen from wherever the pane was struck, so the whole pane
   is cracked by the end of this phase even with the impact pushed outside the
   frame.
2. **Hold** — the cracked pane sits still, long enough to read.
3. **Flight** — the shards let go, nearest the impact first. The window each
   one gets runs from wherever it starts to the end of the transition, so the
   last shard to let go still finishes exactly on the final frame.

**Crack Easing** shapes the order the shards crack in, not the transition clock,
so the crack front can accelerate away and settle while the shards themselves
still fall at a steady rate — the same distinction the tiling transition makes.

## Settings

| Setting | What it does |
| --- | --- |
| **Preset** | A whole look in one pick: break style, crack pattern, timing, motion and glass finish together. Changing any of them afterwards puts this back to Custom. |
| **Shard Size** | In pixels at your canvas resolution. There is a ceiling on the shard count, so on a large canvas a very small size stops getting smaller rather than building a mesh too big to draw — it gives fewer, larger shards, never a partial break. |
| **Break Seed** | Which break you get, without changing anything about how it looks. The same seed always gives the same break, so a transition plays the same way every time. |
| **Impact Centre X/Y** | Where the pane is struck. Can be pushed outside the frame (−50% to 150%); the break is always sized to the furthest corner from wherever it sits. |
| **Crack Spread** / **Hold** | The phase split, as shares of the transition. The hold is held above zero so the break is always readable before it comes apart. |
| **Gravity Direction** / **Gravity** | Which way the shards fall and how hard. 0° pulls to the right, 90° straight down, matching the tiling transition's angle convention. |
| **Throw** | How hard the shards are pushed away from the impact as they let go. It is an impulse and gravity is an acceleration, so one is linear in the flight and the other goes with its square — that is the whole of the ballistics. |
| **Spin** | Turns a shard makes over its whole flight. Each takes its own axis and direction, so they tumble rather than rotating together. |
| **Scatter** | Blends the ordered break towards a shuffle: shards stop letting go in step with the crack front and stop flying straight out along their own radius. |
| **Flat shards** | Keeps the shards in the screen plane instead of tumbling through it. Cleaner and more graphic, and it also means they no longer have to be depth sorted each frame. |
| **Crack Colour** | Lights the cracks, the bevel along each cut edge, and the glint. |
| **Crack Width** / **Crack Glow** | The hairline along each cut, and how hard it burns as it forms. The flash travels with the crack front rather than lighting the whole break at once. |
| **Edge Light** / **Edge Light Width** | The lit bevel that gives the shards thickness. A shard caught at a glancing angle picks up more of it, the way a real one does as it turns over. |
| **Refraction** | How far the scene is bent near a shard's edges, as though seen through a thick broken pane. Measured in pixels at the very edge, falling to nothing in the middle. |
| **Shard Variation** | Randomly lightens and darkens each shard, so the break reads as many pieces of glass rather than one image cut up. |
| **Glint** | A highlight sweeping across the pane as it breaks. It tracks where each shard started rather than where it has flown to, so it reads as one light crossing a sheet of glass. |

Every one of the glass looks is gated on the crack front, which is what makes
the first frame of the transition the outgoing scene exactly — no seams between
shards, no rim light, no tint — and the last frame the incoming scene the same
way, by the shards having either faded out or landed.


# Matte Cutout Transition

A single shape cuts the outgoing scene away to the incoming one: an iris wipe
in its plainest form, and with the bands behind its edge turned up, a stinger.
The tiling transition's Keyhole mode does something related with a whole
lattice of small shapes; this is one shape, which frees it to be one that could
never tile.

Every cutout is one field over the canvas, saying how far the front has to
travel before each pixel changes scene. A shape that grows is its own gauge —
the scale at which its outline passes the pixel — so the polygons come out as
true pixel distances square to their edges, and the bands trailing the front
are concentric copies of the shape. A sweep is the distance along the sweep with
the edge's own profile taken off it. Either way the front only has to run from
the lowest value on the canvas to the highest, which is found by sampling the
canvas edge, so an offset centre, a turn, a spin or a stretch never cuts the
transition short.

## Shapes

| Shape | What it is |
| --- | --- |
| **Circle**, **Square**, **Triangle**, **Diamond**, **Hexagon**, **Octagon** | The plain shapes, grown about the centre. |
| **Star** | Any number of points from 3 to 12, with **Star Depth** setting how far in the inner corners sit. At full depth it is the regular polygon. |
| **Heart** | A square on its point with a disc on each upper edge. |
| **Cross (X)** | Two crossed bars. Turn it 45° with **Angle** for a plus. |
| **Wave – Sine Sweep** | A wavy edge sweeping across the canvas. |
| **Wave – Curling Crest** | Waves that rise gently and fall away steeply, each with a lip hanging over its face, rolling across the canvas. |
| **Wave – Radial Ripple** | A circle whose outline ripples as it grows. |
| **Clock** | A hand sweeping round from twelve o'clock; **Segments** gives it more than one. |
| **Spiral** | Arms winding out from the centre; **Spiral Turns** sets how many times they wind between the centre and the far corner. |
| **Blinds** | Bars that open from their middles, across the canvas. |
| **Zigzag** | A sawtooth edge sweeping across. |
| **Custom Image – Shape** | Your own image's silhouette, grown like the built-in shapes. Its opaque pixels are the shape, or for an image with no transparency, its bright ones. It is traced as seen from the image's centre, so holes and overhangs are filled in — a shape with a hole grown about its centre would grow the hole too, and never close. |
| **Custom Image – Luma Wipe** | Your own image laid over the canvas, used as an order: dark areas change scene first, light ones last. |

The three waves and the zigzag have **Wave Height**, **Wavelength** and **Wave
Drift** — how many wavelengths the edge slides along over the transition, so it
rolls as it travels.

## Direction and motion

**Inside Out** grows the cutout from the centre with the incoming scene inside
it. **Outside In** closes the outgoing scene down onto the centre with the
incoming scene arriving from the edges. For the sweeps, the clock and the luma
wipe it runs the wipe backwards.

- **Angle** turns the shape; for the sweeps it is the direction of travel, 0°
  left to right and 90° top to bottom, the same convention as the tiling
  transition.
- **Spin** — turns the shape makes over the transition.
- **Aspect Stretch** — the shape's width over its height. 178% makes a circle a
  16:9 oval.
- **Centre X/Y** — where the shape grows from, −50% to 150%.
- **Easing** — linear, ease in and out, ease out, ease in, or **Overshoot**,
  which runs past and settles back.
- **Pause Length** / **Pause Size** — holds the cutout still part way through
  at a set size: the cartoon iris that stops on a circle before it closes. An
  overshoot shows best with a pause, which gives it something to settle onto.

## Edge and bands

Behind the front, in order: a **Border** in one colour, then up to four **Echo
Bands** in their own colours, then the incoming scene. **Edge Softness**
feathers every one of those edges, and **Glow** lights the front itself. Set
the border and echoes to 0 for a plain cutout.

The front is run past the field's range at both ends by the feather and the
glow, so neither leaves anything on the first or last frame.

## Presets

**Iris Out**, **Cartoon Iris**, **Star Stinger**, **Spinning Hexagon**,
**Heart Pop**, **Rolling Wave**, **Clock Wipe** and **Blinds**, each setting
the shape, motion and bands together. Editing any of them afterwards puts the
list back to Custom; the centre and the mask image are left alone.

# Portal Transition

A portal is shot open over the outgoing scene with the incoming one showing
through it, holds, and grows until it has swallowed the canvas.

1. **Open** — the oval pops open from a point, overshooting slightly and
   settling. With **Spiral open** on, its inside is a swirl of the portal's
   energy that clears from the centre outwards to let the incoming scene
   through.
2. **Hold** — the portal sits open, its rim burning and throwing sparks.
3. **Grow** — it grows until the rim is past the furthest corner, sized from
   wherever the centre is.

Everything hangs off the signed distance from the oval's edge. Outside, the
outgoing scene ripples under the rim's light; inside, the incoming scene bulges
as though seen through a lens near the edge; on the edge, the rim is a band of
light whose width the noise keeps moving, crawling round the oval so it burns
rather than sitting still.

## Settings

| Setting | What it does |
| --- | --- |
| **Preset** | **Blue Portal**, **Orange Portal**, **Quick Shot** or **Slow Reveal**. Editing any setting afterwards puts this back to Custom. |
| **Portal Colour** | Blue, Orange, or Custom with a colour picker of its own. The hot core of the rim is the colour most of the way to white, the way both stock portals are built. |
| **Centre X/Y** | Where the portal opens, −50% to 150%. |
| **Portal Width** / **Portal Size** | The oval's width over its height, and its open height as a share of the canvas height. |
| **Open** / **Hold** | The phase split, as shares of the transition. The two together stop at 90% so the grow always has time. |
| **Spiral open** | The swirling energy inside the opening portal. Off shows the incoming scene through it at once. |
| **Rim Width**, **Glow**, **Glow Reach** | The rim band, the brightness of all the light, and how far it reaches out over the outgoing scene. |
| **Rim Swirl** | How far the burning pattern crawls round the rim over the transition. |
| **Sparks** | Sparks thrown off the rim. They die away as the portal grows. |
| **Distortion** | How far the scenes are bent near the rim, in pixels. 0 leaves both flat. |

Every width scales with the portal while it opens, so a portal a few pixels
across is not all rim. The first frame has no portal at all, and on the last it
has grown past the furthest corner by more than anything the edge reaches in, so
both ends are exactly one scene.

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

`test/test-glass.c` covers the glass transition differently, because that one
builds its shards in C rather than in a shader: the test links `src/glass-mesh.c`
directly instead of mirroring it. It checks the two properties the transition
rests on — that the shards are an exact partition of the canvas for every
pattern, shard size and impact point, including impacts outside the frame, and
that every shard's flight fits inside the transition, seated and opaque on the
first frame and finished on the last. It also checks that the seam bleed only
ever grows a shard, that every cell is convex with its centroid inside it (the
shards are drawn as a fan from that point), that the shard ceiling gives fewer
larger shards rather than a partial break, and that the same settings always
give the same break.

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

`test/test-matte.c` and `test/test-portal.c` link `src/matte-math.c` and
`src/portal-math.c`, which hold the geometry and the clock the plugin builds the
two transitions' uniforms from. The matte test checks that both ends are exact
for every shape, both directions, any centre on or off the canvas, turn, spin,
stretch and band layout; that each shape is what it says — the polygons are 1
on their own outline, a star at full depth is the polygon; that the grown
shapes scale about their centres and every field is monotone along rays from
it, which is what lets the range be found from the canvas edge alone; that the
clock starts at 0 and finishes at 1 with the pause holding; and that image
silhouettes trace sensibly, holes filled. The portal test checks that the first
frame has no portal and the last has every pixel past the shader's early exit to
the untouched incoming scene, and that the size is continuous across the
phases, with a pop that overshoots only a little.

None of them needs OBS or a GPU:

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

`test/glass-check.c` is the same idea for the glass transition, which needs its
own because it draws a mesh rather than a sprite. It builds the real shards,
uploads them and renders every break style and crack pattern. A vertex layout
that does not match what the shader declares, a triangle wound the wrong way, a
seam the bleed fails to close, or a first frame that is not quite the outgoing
scene all show up here and nowhere else:

```sh
xvfb-run -a ./build/glass-check data/effects/glass_transition.effect
```

`test/matte-check.c` and `test/portal-check.c` render the two newer effects.
The matte check covers every shape in both directions, plain and as a full
stinger, and also renders each shape with hard edges mid-transition and
compares every pixel's scene against `matte_field()` evaluated on the CPU. That
is what holds the shader's field and its C mirror to each other: the plugin
finds the front's range with the mirror, so a mirror that drifted would cut the
transition short.

```sh
xvfb-run -a ./build/matte-check data/effects/matte_transition.effect
xvfb-run -a ./build/portal-check data/effects/portal_transition.effect
```

## Licence

GPL-2.0-or-later. See [LICENSE](LICENSE).
