<!--
SPDX-FileCopyrightText: Copyright (c) 2026 Ingo Ruhnke <grumbel@gmail.com>
SPDX-License-Identifier: GPL-3.0-or-later
-->

# TODO

Follow-up work for wl-x11. Ordered roughly by impact. The project vendors a
patched wlroots (`subprojects/wlroots`); several items assume we can change
the X11 backend freely.

## Medium impact

## Size negotiation (client ↔ host WM)

Loop (log evidence): client prefers 653 → `resize_output_to(653)` → X
`ConfigureNotify 719` (seq *newer* than our configure — not caught by stale
seq filter) → `request_state`/`size_from_wm` → `set_size(719)` → client still
prefers 653 → **client_driven cleared size_from_wm and fit again** → repeat.

- [x] Backend: ignore ConfigureNotify with seq < last ConfigureWindow
- [x] Backend: ignore echo when size matches win_width/height
- [x] Compositor: **do not** let client preferred size override `size_from_wm`
      (removed client_driven fit / clear). While size_from_wm, log
      `size: hold host size` instead of fighting.
- [ ] Manual verify: one bounce max then hold; interactive WM resize still
      updates client via set_size; initial map (size_from_wm=0) still fits.


## Explicitly out of scope (for now)

- Layer-shell (not meaningful without a real desktop surface)
- Full server-side decoration negotiation beyond "always SSD so clients do
  not double-decorate"
- DRM / session backends

---

## Popups (`xdg_popup`) — unclipped menus via proper multi-window model

### Goal

Allow Wayland `xdg_popup` content (menus, combo dropdowns, tooltips) to
extend past the parent toplevel’s X11 window edge without clipping, while
keeping ICCCM/EWMH-correct host behavior and correct seat input (no mid-click
button resets, no dual-pointer races, no surprise `wl_output` globals).

### Current behavior (baseline)

Popups are parented into the **parent toplevel’s scene tree** and drawn in
the same X11 window (`popup_position_and_map` in `src/xdg.c`). Matches
tinywl/sway; input/grab stay simple.

**Trade-off:** content past the parent edge is clipped by the X11 window.

Leftover symbols from the rejected OR attempt remain (`wlx_popup.output`,
`scene_output`, `popup_output_*`, `popup_at_root_pointer`) but are idle under
the parent-scene path.

### Why the previous approach failed (do not repeat)

Mapping each popup (or a shared “popup stage”) as a full **`wlr_output`**
via `wlr_x11_output_create_override_redirect()` failed for structural
reasons, not minor bugs:

| Failure | Cause in current stack |
|---------|------------------------|
| Qt menus vanish on open | Creating a `wlr_output` emits `backend.events.new_output` → new `wl_output` global mid-grab; many clients treat output topology changes as “tear down transient UI” |
| Hover flaky / double-click to open | Each X11 output registers its **own** `wlr_pointer` + touch (`new_input`); seat sees multiple devices; enter/leave races |
| Opening click lost | `wlr_seat_pointer_notify_enter` while `button_count > 0` calls `reset_buttons` and drops the press that owns the xdg_popup grab |
| Menu does not track parent | Hit-testing / placement mixed `wlr_output_layout` bookkeeping coords with real root-space X11 geometry |
| Hacks piled up | `swallow_next_pointer_release`, deferred release, manual root hit-testing fought `wlr_seat` instead of fixing the model |

**Hard rule:** do **not** reintroduce OR-as-full-`wlr_output` for popups.
A present path that is not a Wayland output is required.

### How X11 deals with menus (protocol facts)

ICCCM §4 (pop-up windows) — three options for a short-lived surface:

1. **Normal top-level** — WM-managed and decorated (wrong for menus).
2. **`WM_TRANSIENT_FOR`** — lighter treatment (dialogs); still managed.
3. **Override-redirect** — correct for menus: client sets
   `override-redirect` on create; WM must not reparent, decorate, restack on
   click, or apply normal placement. ICCCM advises a pointer grab while
   mapped. Example given: pop-up menu.

Toolkit practice (GTK, Qt, Chromium, Motif-era code):

- Create as child of the **root**, not of the app window (so geometry is
  root-relative and not clipped by the parent’s border/clip).
- Set `override-redirect = true` (and often `save_under`).
- Set EWMH `_NET_WM_WINDOW_TYPE` to one of:
  `_NET_WM_WINDOW_TYPE_MENU`, `_POPUP_MENU`, `_DROPDOWN_MENU`,
  `_TOOLTIP`, `_COMBO` (type is informational for compositors; OR is what
  disables WM management).
- Position with `ConfigureWindow` in **root coordinates** before map.
- Input: active **pointer grab** (owner-events style) for the menu’s
  lifetime; click outside dismisses.

Compositing WMs still see `MapNotify` for OR windows and must composite
them, but must not frame or restack them as managed clients.

### How Wayland `xdg_popup` works (protocol facts)

From xdg-shell:

- Created with `get_popup(parent, positioner)`; position is **relative to
  the parent surface’s window geometry**, not global screen space.
- Compositor sends `xdg_popup.configure(x, y, width, height)` then
  `xdg_surface.configure`; client acks and attaches a buffer.
- **Explicit grab:** `xdg_popup.grab(seat, serial)` from a qualifying input
  event. Nested grab: parent must be toplevel or a popup that already holds
  the grab. Dismiss order is topmost-first.
- During grab, the grabbing client receives pointer/touch for **all of its
  surfaces** (X11 “owner-events” analogue); keyboard focus stays on the
  topmost grabbing popup. Click outside → compositor dismisses the grab
  stack.
- Nested popups stack above earlier ones for the same toplevel.
- Parent must be mapped before the popup.

wlroots implements the grab in `wlr_xdg_popup` / seat grab helpers; the
compositor must not break enter/button serial continuity across map.

### Design: “present window” (not `wlr_output`)

Introduce a first-class **X11 present window** object in the vendored
backend that can host a buffer and receive pointer events **without**
being a Wayland output.

#### Backend (`subprojects/wlroots/backend/x11/`)

New type, roughly:

```c
struct wlr_x11_present_window {
    struct wlr_x11_backend *x11;
    xcb_window_t win;
    int32_t width, height;
    int16_t root_x, root_y;       /* last Configure position */
    bool mapped;
    bool override_redirect;       /* always true for popups */
    /* Present / DRI3 / SHM path — share buffer import with wlr_x11_output */
    xcb_present_event_t present_event_id;
    struct wl_list buffers;
    /* Signals for compositor: destroy, present_complete — NOT new_output */
};
```

API sketch (names flexible):

| Function | Role |
|----------|------|
| `wlr_x11_present_window_create(backend, OR flags)` | Create root child, OR, ARGB visual, no `WM_NORMAL_HINTS` |
| `wlr_x11_present_window_configure(win, x, y, w, h)` | Root-space position + size; apply before map |
| `wlr_x11_present_window_map` / `unmap` | MapWindow / UnmapWindow |
| `wlr_x11_present_window_present(win, buffer, …)` | Same Present/DRI3 path as output_commit buffer path |
| `wlr_x11_present_window_get_xcb(win)` | For compositor property side-channel |
| `wlr_x11_present_window_destroy` | |

**Must not:**

- Call `wlr_output_init` / emit `backend.events.new_output`
- Create or emit `wlr_pointer` / `wlr_touch` (`new_input`)
- Appear in `wlr_output_layout`
- Set managed ICCCM hints that invite WM framing

**May:**

- Select XI2 events on the window (button/motion) and forward them into the
  **existing single** seat path the compositor already uses (root-pointer
  query + compositor hit-test), *or* leave XI2 only on toplevel outputs and
  rely on root query (current compositor style). Prefer one path only.
- Set `_NET_WM_WINDOW_TYPE_POPUP_MENU` (and optionally `XdndAware` later).

Refactor shared code out of `wlr_x11_output` (buffer import, Present
complete, expose handling) so outputs and present-windows do not diverge.

#### Scene graph bookkeeping (compositor)

The internal `wlr_scene` remains a bookkeeping graph; it is **not** a
desktop layout.

For each mapped surface that has an X11 host window:

| Kind | X11 host | Scene role |
|------|----------|------------|
| `xdg_toplevel` | Managed `wlr_output` window (unchanged) | `wlx_window.scene_tree` pinned to that output’s layout region |
| `xdg_popup` | OR `wlr_x11_present_window` | Own `scene_tree`; rendered via a **scene output bound only for present**, or direct render-to-present-window — must not publish `wl_output` |

Track explicitly:

```c
struct wlx_popup {
    /* existing fields … */
    struct wlr_x11_present_window *xpresent; /* replaces output/scene_output */
    /* cached root-space box of the OR window for hit-testing */
    int16_t root_x, root_y;
    int32_t root_w, root_h;
};
```

Parent move/resize:

- On parent ConfigureNotify (or our knowledge of parent root position
  change), recompute each child popup’s root position:
  `parent_root + scaled(popup.geometry)` and
  `wlr_x11_present_window_configure`.
- Nested popups: geometry is relative to immediate parent surface; walk the
  chain to the toplevel’s root origin.

#### Input routing (compositor + seat)

Single logical pointer. Hit-test in **root coordinates** only:

1. `query_root_pointer_position`
2. Topmost hit among:
   - mapped popup present-windows (stack order = map/nest order)
   - then toplevel content windows (`content_xwin` / related)
3. Convert to surface-local coords for that surface
4. Deliver via existing `wlr_seat_pointer_*` on the **one** seat

Invariants (already partly in `src/input.c` — keep and extend):

- **Never** `wlr_seat_pointer_notify_enter` when `button_count > 0` if the
  focused surface would change (resets buttons, breaks xdg_popup grab).
- On popup map with button held: defer enter until release
  (`wlx_pointer_refresh_focus` on button release — already present).
- Outside click with active grab: let wlroots xdg_popup grab dismiss;
  do not synthesize competing enters to foreign surfaces mid-grab.
- Do not use `wlr_output_layout` coordinates for hit-testing.

Keyboard: focus follows xdg_popup grab rules (topmost grabbing popup);
host WM focus still drives which **toplevel** is active when no grab.

#### Mapping sequence (popup)

1. `new_popup` → allocate `wlx_popup`, build `scene_tree` (unmapped/disabled).
2. Initial commit → `wlr_xdg_popup_unconstrain_from_box` with a large box
   (or parent-output bounds in root space once multi-monitor host geometry
   is known) so the client is not artificially constrained to the parent
   window size.
3. Surface map:
   - Create present-window if needed; set window type MENU/POPUP_MENU.
   - Size = buffer / scheduled geometry (scaled).
   - Position = parent root position + geometry offset (SSD: account for
     window-geometry origin vs surface origin, same as today’s
     `popup_at_root_pointer` math).
   - Configure then Map (size before map, consistent with toplevel rules).
   - Enable scene node; present first frame.
4. Commit while mapped → resize/reposition present-window if geometry
   changed; present.
5. Unmap / destroy → unmap+destroy present-window; disable scene node;
   refresh pointer focus.

#### Nested popups

Same present-window model per popup. Stacking: raise OR window above
siblings on map (`XCB_STACK_MODE_ABOVE` relative to parent popup’s X
window or free above). Dismiss order remains wlroots’ grab stack.

#### Client compatibility checklist

| Client class | Expectation |
|--------------|-------------|
| GTK / foot menus | OR window, no new `wl_output`, grab serial preserved |
| Qt menus | **No** `wl_output` global at map time (critical) |
| Nested submenus | Second present-window; parent geometry chain |
| Tooltip (no grab) | Still OR present-window; no seat grab; hover leave dismisses per client |
| Opening click | Button held across map; enter deferred until release |

### Implementation phases

Track progress here; check off when done. Prefer backend completeness
before switching the compositor path so we can A/B.

#### Phase 0 — Research lock-in (this document)

- [x] Document X11 OR / ICCCM / EWMH menu behavior
- [x] Document xdg_popup positioning + grab semantics
- [x] Record failure modes of OR-as-`wlr_output`
- [x] Identify backend split: present-window vs output

#### Phase 1 — Backend present-window

- [x] Extract shared buffer import + Present complete from
      `backend/x11/output.c` into a helper usable by both output and
      present-window (`x11_buffer_get_or_create` / `x11_buffer_destroy` /
      `x11_buffer_find_by_pixmap`; Present handler routes idle/complete to
      outputs **and** present-windows)
- [x] Implement `wlr_x11_present_window` create/configure/map/unmap/
      present/destroy (`backend/x11/present_window.c`)
- [x] **No** `new_output` / **no** `new_input` emissions
- [x] Public headers under `wlr/backend/x11.h`
- [ ] Unit-level smoke: create OR window, present solid buffer, destroy
      (manual under X11 — needs a running DISPLAY; defer to integration)

#### Phase 2 — Compositor data model

- [x] Replace idle `wlx_popup.output` / `scene_output` / `l_output` with
      `xpresent` + root box cache (`root_x/y/w/h`, `root_box_valid`)
- [x] Registry: popup list already exists (`server->popups`); destroy paths
      call `popup_destroy_xpresent` (unmap + destroy) so X windows cannot leak
- [x] Parent root-position helper: `window_content_root_position()`; popup
      hit-test uses cached root box via `popup_update_root_box`

#### Phase 3 — Map/present path

- [x] `popup_position_and_map`: create/configure/map present-window;
      scene tree reparented to scene root and parked at (-100000,-100000)
      so parent outputs do not double-paint; root-space Configure + Map
- [x] Frame scheduling: present on map/commit via `popup_render_and_present`
      (direct client buffer when scale=1; else render-pass composite);
      `xpresent_frame` sends `frame_done` to popup surfaces
- [x] Unconstrain box: prefer host `_NET_WORKAREA` / root size in parent
      surface coords (nested popups use parent popup root box); large fallback

#### Phase 4 — Input

- [x] Unified root-space hit test: popups first (topmost), then toplevels
      (`surface_under_root_pointer` in `src/input.c`)
- [x] Wire `popup_at_root_pointer` to present-window / root box geometry
      (scene hit via parked tree origin + root-relative pixels)
- [x] Preserve button_count / deferred-enter rules (motion + enter paths)
- [x] Outside click clears pointer focus before button delivery so
      xdg_popup grab dismisses (serial / no grabbing-client surface)
- [ ] Manual verify under host WM with GTK/Qt menus (integration)

#### Phase 5 — Parent motion & lifecycle

- [x] Reposition popups when parent X11 window moves
      (`ConfigureNotify` → `wlx_reposition_popups_for_window`; configure OR)
- [x] Parent unmap/destroy → destroy child present-windows first
      (`destroy_present_windows_for_toplevel`)
- [x] Nested popup chain: placement relative to parent popup root box;
      reposition oldest-first so parents update before submenus

#### Phase 6 — Cleanup & policy

- [x] Dead per-popup OR-`wlr_output` path removed (present-window is the
      only menu host). `wlr_x11_output_create_override_redirect` retained
      only for bootstrap `WLX-BOOT` and CSD transient dialogs.
- [x] `AGENTS.md` / README updated for present-window model
- [x] SSD vs CSD: menus are independent OR present-windows; parent-expand
      approach remains **retired**

### Retired idea: expand parent window

Temporarily growing the parent X11 window to the union of parent+popup
bounds is **not** the direction of travel. It interacts badly with SSD
(frame jumps), `size_from_wm`, and interactive move. Present-windows match
how X11 menus actually work.

### Related leftovers (keep until Phase 6)

- **Bootstrap `WLX-BOOT` virtual monitor** — still required so clients like
  foot start with ≥1 `wl_output`. Not used for popups; must remain the only
  non-toplevel output clients see at connect.
- **wlroots xdg_popup grab behavior** (swallow outside release / ignore
  some failed presses) — still relevant for Qt opening click; re-test after
  Phase 4 rather than patching seat further.

### Risks & mitigations

| Risk | Mitigation |
|------|------------|
| Host compositor clips OR windows oddly | Use ARGB visual + transparent back pixel; type MENU |
| Present path lag vs parent output | Share MSC/idle notify handling; schedule present on commit |
| Focus: click on OR steals WM focus | Expected for menus; on dismiss, restore focus to parent toplevel via existing X11 focus path |
| Multi-monitor parent on monitor A, menu on B | Root coords handle this; unconstrain box should cover union of host monitors if queryable |
| Scale (`--scale`) | All root positions/sizes in host pixels; surface coords converted with existing `wlx_pointer_to_surface` / `wlx_scale_size` |

### Progress log

- 2026-07-28: Design written; parent-scene remains active code path until
  Phase 3 lands. No behavioral change yet.
- 2026-07-28: Phase 1 backend API landed:
  - `wlr_x11_present_window_*` public API (create/configure/map/unmap/
    present/destroy + frame/destroy listeners)
  - Shared `x11_buffer_*` helpers used by outputs and present-windows
  - Present event path unlocks buffers for both; frame signal on
    present-windows (no `wlr_output_send_frame`)
  - EWMH `_NET_WM_WINDOW_TYPE_POPUP_MENU` (+ dropdown/menu) on create
  - Backend destroy tears down remaining present-windows
  - Compositor still uses parent-scene popups (Phase 2–4 next)

- 2026-07-28: Phase 2 compositor data model:
  - `wlx_popup` uses `xpresent` + root box cache; removed output/scene_output
  - `popup_destroy_xpresent` / frame+destroy listeners wired for Phase 3
  - `popup_at_root_pointer` uses cached root box; `window_content_root_position`
  - Public `wlr_x11_present_window_get_geometry` / `is_mapped`
  - Still parent-scene painting until Phase 3

- 2026-07-28: Phase 3 map/present path:
  - Popups create OR present-windows; parent scene no longer clips menus
  - Scene parked off-layout; Present of client buffer or composited buffer
  - Reposition on parent move updates present-window Configure
  - Unconstrain still uses large fixed box (host workarea TBD)

- 2026-07-28: Phase 4 input:
  - surface_under_root_pointer: popups then toplevels, root coords only
  - pointer_enter clears focus on miss (grab dismiss)
  - no notify_enter surface switch while button_count > 0

- 2026-07-28: Phase 5 parent motion & lifecycle:
  - Nested placement uses parent popup root box when applicable
  - Reposition reverse-order; toplevel unmap/destroy tears down OR windows

- 2026-07-28: Phase 6 cleanup & policy:
  - AGENTS.md documents present-window rules; parent-scene is fallback only
  - Dual-device / per-popup wlr_output approach fully superseded


---

## Subsurface overflow → present-window (GTK menubar menus)

### Problem (confirmed 2026-07-28)

GTK menubar dropdowns (gedit) are **not** `xdg_popup`. WAYLAND_DEBUG shows:

```
wl_subcompositor.get_subsurface(sub, menu_surface, toplevel_surface)
wl_subsurface.set_position(x, -280)   # above parent origin
```

No `xdg_surface.get_popup`. Protocol does **not** require clipping subsurfaces
to the parent buffer; Weston paints overflow on the large output. wl-x11 maps
each toplevel to a **small X11 window (= wlr_output)** sized to the client
geometry, so subsurface pixels at negative Y fall outside the output and
vanish. Scene `set_clip` is not the cause (unused); the host window size is.

Context menus that use `xdg_popup` already work via present-window.

### Chosen approach (not parent expand)

**Promote overflowing subsurfaces to OR present-windows** — same host model as
`xdg_popup`. Do **not** grow the toplevel X11 window (host WM flicker,
`size_from_wm` / SSD math, position thrash on every `set_position`).

### Design

| Step | Action |
|------|--------|
| Detect | On toplevel (and subsurface) commit: `wlr_surface_for_each_surface`; any child whose box is not ⊆ parent buffer rect is an overflow candidate |
| Map | Create `wlr_x11_present_window`; root position = content root + scaled(subsurface offset); present client buffer |
| Scene | Disable the scene buffer node for that surface so the parent output does not double-paint a clipped copy |
| Input | Root hit-test: subpresent boxes after xdg_popup presents, before toplevels |
| Unmap | Subsurface gone / fully inside bounds → destroy present-window, re-enable scene node |
| Parent lifecycle | Toplevel unmap/destroy tears down all subpresents for that window |

Reuse `wlr_x11_present_window_*` and the same Present / format fallback as
`popup_render_and_present`. No new `wlr_output` / `wl_output` / seat device.

### Implementation phases

#### Phase S0 — Spec (this section)
- [x] Confirm subsurface path via WAYLAND_DEBUG (gedit)
- [x] Prefer present-window promotion over parent expand
- [x] Document detection / scene disable / input order

#### Phase S1 — Data model + sync
- [x] `struct wlx_subpresent` + `server->subpresents` list
- [x] `wlx_window_sync_subpresents(win)` from `surface_commit` / unmap
- [x] Create/configure/map/present OR window; destroy on demote
- [x] Disable matching scene buffer node while presented

#### Phase S2 — Input
- [x] `subpresent_at_root_pointer` + include in `surface_under_root_pointer`
- [x] Motion/button use existing seat path (no extra grab unless needed)

#### Phase S3 — Lifecycle polish
- [x] Parent destroy / output destroy cleanup
- [x] Nested subsurface offsets (for_each_surface accumulates)
- [ ] Manual verify: gedit menubar unclipped; Qt/xdg_popup unchanged

### Progress log

- 2026-07-29: Plan locked; implementation starts Phase S1.
- 2026-07-29: Phase S1–S2 landed (`src/subpresent.c`): overflow subsurfaces
  get OR present-windows; scene node disabled; root hit-test wired.
  Awaiting manual gedit menubar verify.

- 2026-07-29: xdg_popup unconstrain uses host `_NET_WORKAREA` (else X root)
  converted into parent-surface coordinates; nested menus use parent popup
  root box. Remaining open: manual verify (gedit menubar subpresent, GTK/Qt
  xdg_popup), present-window unit smoke under DISPLAY.

- 2026-07-29: Docs — AGENTS.md / README.md describe subsurface→present-window
  path and hit-test order; note KDE server-decoration not advertised.
  `new_subsurface` on toplevel triggers early `wlx_window_sync_subpresents`.
  Open: manual verify only (gedit menubar, GTK/Qt xdg_popup).

- 2026-07-29: Clipboard — advertise/accept `text/html` alongside plain text
  (same underlying bytes). Popup/subpresent work is verify-only until a host
  DISPLAY session is used.
