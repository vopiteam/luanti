# UI, input and rendering

The largest group of fork changes. They exist because a phone is not a desktop:
the screen is small and its pixel density varies by a factor of three, the only
pointer is a finger, and the whole interface has to be styleable by the game
rather than fixed by the engine.

Element-level reference for the additions below lives in `doc/lua_api.md`
alongside the upstream elements; this document explains what each group is for
and why it works the way it does.

## Formspec

### Fonts scale with the form, not with the screen

Formspec font sizes are derived from `imgsize`, so a form keeps its proportions
across devices instead of having text grow or shrink relative to the elements
around it. The same scaling is applied to hypertext, tooltips and checkbox
labels, so a form does not end up with four different notions of "normal text".

### Elements

| Element | Addition |
|---|---|
| `model[]` | Display fixes; an optional silhouette fit mode |
| `model_overlay[]` | New — a model layer with bone attachments |
| `map[]` | New — a top-down world view. Local games store its fog-of-war inside the world folder |
| `animated_image[]` | One-shot playback, and a 2D grid atlas as the frame source |
| `checkbox[]` | Custom image and styling |
| `label[]` | Alignment, and auto-centering for multi-line labels |
| `scrollbar[]` | Custom textures, routed through the GUI scaling filter, with auto-sized thumb caps |
| `textarea[]` | Touch drag-to-scroll for read-only areas, with an optional scrollbar |
| scroll containers | Touch drag-to-scroll with momentum |

### Backgrounds

`NineSliceBackground` is available standalone and used for tooltips, the
selected inventory slot and the status panel. Its parameters are settings
rather than constants (`tooltip_corner_size`, `tooltip_padding_*`,
`status_text_*`), so a game can tune them against its own art without an engine
change. Desktop values live in `defaultsettings.cpp`; iOS and Android override
them in their platform settings.

## HUD

- **9-slice rendering** for HUD elements, and word-wrapping text panels that
  report their measured size back to Lua — a panel can be laid out against its
  actual height rather than a guess.
- **`anchor_above_hotbar`** positions an element relative to the hotbar, so it
  tracks the hotbar across DPI instead of being pinned to the screen edge.
- **Tappable image buttons**, configurable from Lua, with keyboard bindings so
  the same action is reachable without touch.
- **`status_text` flag** and a redesigned in-game status panel.
- **High `z_index` renders above formspec**, so a HUD element can deliberately
  sit over an open form.
- **A HUD flag controls touch-control visibility** from Lua, and touch HUD
  state is dropped entirely while the HUD is hidden.

## Touch input

- Redesigned touch controls, with the joystick hideable from Lua.
- **Hotbar drag-to-drop.**
- The inventory touch button is anchored to the right edge of the hotbar, and
  its texture is reloaded when its rect size changes.
- **Tap-to-interact works in third-person back view.**
- Synthetic game-key events are delivered while a menu is active, so menus
  built on game keys keep working.
- Interact packets remember the key state that was sent.
- Digging animation is cancelled when a formspec opens.

## Baked node icons

Inventory icons for mesh nodes are generated ahead of time rather than rendered
at runtime. The runtime baking pipeline was removed; what remains is an export
mode:

```
luanti --dump-baked-icons <directory>
```

It renders every node marked with the `icon_bake` group to a PNG in that
directory and exits. The generated files are then shipped as ordinary
`inventory_image` textures — nothing bakes at runtime in a release build.

Export is tuned by settings, read only by the dump mode:

| Setting | Default |
|---|---|
| `inventory_icon_bake_resolution` | `256` |
| `inventory_icon_bake_margin_percent` | `5.0` |
| `inventory_mesh_outline` | `true` |
| `inventory_mesh_outline_percent` | `4.5` |
| `inventory_mesh_outline_color` | `#000000` |

Connected nodeboxes are rendered as a two-post segment, so a fence reads as a
fence rather than as a single ambiguous post. Hidden nodes are excluded from
the candidate diagnostics.

The `icon_bake` group value also carries the orientation, which drives the
node's 3D wield mesh as well — hand and icon show the same side. Generic node
wield meshes are normalized (bounding-box centred, fit to one node); `wield_raw`
exempts a node from that normalization for meshes authored against the raw
transform.

## Camera and movement

- Per-player camera **pitch and yaw range limits**, settable from Lua, hardened
  against non-finite bounds.
- Reduced near clipping plane.
- Server-side view bobbing control.
- `physics_override` swim controls, surface-to-shore stepping, improved autojump
  checks, and flight descent into water is blocked.
- Client stutter no longer accumulates after repeated teleports.

## Rendering

- **glTF morph target (blend shape) animation**, hardened against feedback
  loops and untrusted media.
- The crack overlay is projected in node-local space in the nodes shader.
- A failed font load is never cached, so a transient failure does not persist
  for the session.
- The GUI scaling cache is pre-seeded for main-menu textures.

## Pause menu

The pause menu is rendered from the Lua pause environment, which makes it
themeable by the game, and is robust when there is no game overlay to draw
over.

## Input hardening

Untrusted input from HUD, scrollbar and `map[]` formspec fields is clamped, as
are configurable UI panel rects and scaled font sizes. String-to-float parsing
helpers are hardened. These are validation of values that arrive from a server
or from mod code, not of local settings.
