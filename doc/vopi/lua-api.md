# Lua API additions — index

Every Lua-visible addition this fork makes, and where each one is documented.
Additions that extend an existing upstream concept are documented next to their
upstream peers in `doc/lua_api.md`; the rest are here or in their subsystem
document.

Everything below requires `IS_VOPI_ENGINE`, including the day-cycle APIs and
`clock[]`. With the option off, these functions and the formspec parser are not
registered.

## In `doc/lua_api.md`

Documented beside the upstream elements they extend:

| Addition | Kind |
|---|---|
| `model_overlay[]`, `map[]`, extended `model[]`, `animated_image[]`, `checkbox[]`, `label[]`, `scrollbar[]`, `textarea[]` | formspec elements |
| `anchor_above_hotbar`, HUD 9-slice, word-wrapping text panels, tappable image buttons | HUD |
| `touch_controls`, `status_text` | HUD flags |
| `on_selectnode`, `on_deselectnode` | global callbacks |
| `ObjectRef:set_touch_buttons` / `get_touch_buttons` | per-player touch buttons |
| `ObjectRef:set_block_interaction` / `get_block_interaction` | per-player world interaction |
| `ObjectRef:set_camera_pitch_range` / `get_camera_pitch_range` | camera limits |
| `ObjectRef:set_camera_yaw_range` / `get_camera_yaw_range` | camera limits |
| `ObjectRef:set_camera` / `get_camera` | camera control |
| `ObjectRef:set_view_bobbing` / `get_view_bobbing` | server-side view bobbing control |
| `heat_min`/`heat_max`, `humidity_min`/`humidity_max`, `base_min`/`base_max`, `valley_depth_min`/`valley_depth_max`, `valley_pos_min`/`valley_pos_max`, `mountain_min`/`mountain_max`, `body_min`/`body_max`, `wetland_min`/`wetland_max`, `priority` | biome climate and form bounds and the selection priority, [mapgen.md](mapgen.md#biome-climate-and-form-bounds) |
| `node_seabed`, `depth_seabed` | biome definition fields, mechanism in [mapgen.md](mapgen.md) |
| `biome_at_surface` | decoration flag: the biome filter at the surface a floor, ceiling or liquid-surface decoration stands on |

## In this directory

| Addition | Environment | Document |
|---|---|---|
| `core.get_biome_terrain(pos)` | server, emerge | [mapgen.md](mapgen.md#coreget_biome_terrainpos) |
| `core.get_effective_biome_data(pos)` | server, emerge | [mapgen.md](mapgen.md#coreget_effective_biome_datapos) |
| `core.get_platform_state(topic)` | menu | [platform.md](platform.md) |
| `core.platform_action(topic, action[, arg])` | menu | [platform.md](platform.md) |
| `core.is_content_pack_mounted(id)` | menu, server, emerge, async | [content-vfs.md](content-vfs.md) |
| `core.get_content_pack_info(id)` | menu, server, emerge, async | [content-vfs.md](content-vfs.md) |
| `core.get_mounted_content_packs()` | menu, server, emerge, async | [content-vfs.md](content-vfs.md) |
| `core.is_chat_open()` | **client-side only** | below |
| `core.set_day_cycle`, `core.set_day_cycle_paused`, `core.set_world_time`, `core.advance_time` | server | [day-cycle.md](day-cycle.md) |
| `core.get_day_cycle_state` | server, client-side | [day-cycle.md](day-cycle.md) |
| `core.get_world_load_info` | server | [day-cycle.md](day-cycle.md#world-load-information) |

## `core.is_chat_open()`

```lua
core.is_chat_open()  --> boolean
```

True while the chat console is visible.

**Registered in the client-side (CSM) environment only, deliberately.** It
reads the client `GameUI`, so registering it in the server Lua environment
would let a server-thread call race the main thread — a data race, and
undefined behaviour. If you need this on the server side, the answer is to send
it, not to re-register the function.

## Changed callback signatures

### Detached inventory callbacks

`allow_take`, `allow_put` and their `on_` counterparts on detached inventories
receive destination information:

```lua
function(inv, listname, index, stack, player, to_inv, to_list, to_index)
```

On a drop there is no destination. `to_inv`, `to_list` and `to_index` are all
**`nil`**, so Lua can tell a drop from a move. Passing `nil` rather than the
raw values is deliberate: the underlying drop action leaves the destination
undefined, which would otherwise surface as a plausible-looking
`to_index == 0`.

### Inventory open

The engine sends an inventory-open signal to Lua — only when the inventory
actually opens, not on every attempt that fails to.

## Threading

Two constraints the engine documents at the call site, repeated here because
they are easy to violate from Lua:

- `core.is_chat_open()` is client-side only, for the reason above.
- The language-changed callback runs on a specific thread; see its
  documentation in the source before doing work in it.
