<!--
SPDX-FileCopyrightText: Copyright (c) 2026 Ingo Ruhnke <grumbel@gmail.com>
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Size negotiation (X11 host ↔ Wayland client)

This is the design record for size policy in wl-x11. Read this before
changing `src/xdg.c`, `src/x11.c`, `src/output.c`, or the vendored X11
backend size path. Update this file when the policy changes.

| Layer | Paths |
|-------|--------|
| Vendored X11 backend | `subprojects/wlroots/backend/x11/output.c` |
| Compositor policy | `src/xdg.c` (surface commit), `src/x11.c` (`output_request_state`, `_NET_WM_STATE`), `src/output.c` (`resize_output_to`, `output_commit`, `wlx_toplevel_fill_host`) |

Related: [AGENTS.md](../AGENTS.md) (placement), [TODO.md](../TODO.md).

---

## 0. Working notes (2026-07-30)

### Symptom report

1. **Small Wayland surface in a large X11 window** (letterboxed / empty
   chrome) — rare while floating; **common after maximize** and after
   toolkit redraws on **focus loss** while still maximized.
2. Content-driven apps that shrink/grow their window for UI (e.g. 845↔427)
   must still move the host window when the host is **not** owning size.
3. Unrestricted preferred fit oscillated (557↔2560) and exhausted the
   swapchain.

### Root causes identified (not heuristics)

| Cause | Detail |
|-------|--------|
| **No origin on ConfigureNotify** | ICCCM: clients must track Notify, not assume ConfigureWindow stuck. Event has sequence + size only — no “who asked”. Self-ack must be done by the issuer (backend pending size). |
| **Content vs frame** | Resize uses **`output->win`** (wlroots content window) only. Frame outer size must never feed `wlr_output` mode (historical runaway growth). |
| **`size_from_wm` sticky** | Set on every external `request_state` and on maximize. Without **client_assert** clearing it when floating, content resize never moved the host again. |
| **Unmaximize + stale buffer** | request_state restores e.g. 500×350 with size_from_wm; client may still commit maximized buffer. Grow client_assert must not clear size_from_wm or host jumps back to fullscreen. |
| **Silent hold** | Under host authority, preferred mismatch only **logged**. Client could keep a small buffer; `last_conf` already matched host so nothing re-sent `set_size` → permanent letterbox. Focus-loss toolkit redraws trigger the same path while maximized. |
| **Grow-only** | Fixed oscillation but broke intentional shrink. Wrong trade-off. |

### Correct authority model

```
host_authority = maximized || fullscreen || size_from_wm

if host_authority:
    // X window size is fixed by the host WM (or last external configure).
    // NEVER resize_output_to from preferred.
    // ALWAYS ensure xdg configure matches host content size (fill_host).
else:
    // Floating, client-driven.
    // Preferred fit host both ways (grow and shrink).
    // client_assert may clear size_from_wm.
    // Oscillation guard blocks rapid A→B→A (maximize races without state).
```

### Why fill_host on every lagging commit

xdg-shell configures are **hints**. A client may attach a smaller buffer
(especially on deactivate). The host X window does not shrink while
maximized. The only correct compositor action is to **re-send**
`xdg_toplevel.configure` for the host size until geometry catches up
(or the user unmaximizes). Letterboxing is a temporary present fallback
(`hold_present`), not a steady state we accept without re-asserting.

### Why client_assert does not run while tiled

While maximized/fullscreen, a “smaller preferred” is **not** a request to
shrink the host — EWMH still has the window maximized. Clearing
`size_from_wm` and fitting would fight the WM. Unmaximize clears
`size_from_wm` explicitly in the property handler.

### Content window vs decoration frame (do not regress)

| Id | Meaning |
|----|---------|
| `wlr_x11_output` / `output->win` | Content window; MODE and ConfigureNotify |
| `win->xwin` | Often the WM **frame** after reparent |
| `win->content_xwin` | Discovered client window for EWMH messages |

All size negotiation numbers in logs (`output=`, preferred, last_conf)
are **content / logical Wayland** sizes, not frame outer size.

---

## 1. Roles

| Authority | Mechanism | Meaning |
|-----------|-----------|---------|
| Host WM | ConfigureNotify on **content** window; `_NET_WM_STATE` | User resize, maximize, tile |
| Wayland client | Commit geometry / buffer | Preferred content size |
| wl-x11 | Policy below | Chooses who drives the X window and what configure the client gets |

---

## 2. Protocol facts (public)

### X11 / ICCCM

- Resize with `ConfigureWindow`; **must** follow `ConfigureNotify`.
- WM may rewrite size. **No origin/purpose field** on the event.
- Real Notify: parent-relative coords. Synthetic: root-relative, `send_event`.
- `WM_NORMAL_HINTS`: we set **PSize | PMinSize** only (no position flags).

### EWMH

- `_NET_WM_STATE` maximized/fullscreen is **property** state, orthogonal to
  a single ConfigureNotify, but usually paired with a configure.

### xdg-shell

- Compositor sends `xdg_toplevel.configure(w,h,states)` then
  `xdg_surface.configure(serial)`; client `ack_configure` before commit.
- Non-zero size is a **hint** to use that window geometry.
- Zero size means client may pick dimensions.

References: [ICCCM](https://x.org/releases/X11R7.6/doc/xorg-docs/specs/ICCCM/icccm.html),
[wm-spec](https://specifications.freedesktop.org/wm-spec/latest/),
[xdg-shell](https://wayland.app/protocols/xdg-shell).

---

## 3. Backend (pending self-configure)

On `ConfigureWindow` (`output_set_custom_mode`):

- Record `last_configure_seq`, `pending_width/height`, `has_pending_configure`.
- Optimistically update `win_width/height`.

On `ConfigureNotify`:

| Condition | Action |
|-----------|--------|
| seq older than last ConfigureWindow | Stale — ignore |
| size equals pending | **Self-ack** — clear pending; **no** `request_state` |
| size equals current win_* | Echo — ignore |
| else | **External** — clear pending; `request_state` |

Helpers: `wlr_x11_output_has_pending_configure()`,
`wlr_x11_output_get_pending_size()`.

---

## 4. Compositor paths

### 4.1 `output_request_state` (external host size)

1. Ignore empty / same-as-current mode.
2. `size_from_wm = true`.
3. Commit MODE → backend may ConfigureWindow again (new pending).
4. `output_commit` → `wlx_toplevel_fill_host("output_commit")`.

### 4.2 `_NET_WM_STATE` maximize / fullscreen

1. Mirror into `wlr_xdg_toplevel_set_maximized/fullscreen`.
2. On maximize/fs: `size_from_wm = true`, clear CSD margins.
3. On restore: `size_from_wm = false`.
4. If max/fs: `wlx_toplevel_fill_host("net_wm_state")`.

### 4.3 Surface commit preferred-size policy (`src/xdg.c`)

```
client_assert_shrink = mismatch && conf differs && !tiled && preferred < host
if client_assert_shrink && size_from_wm: clear size_from_wm
# grow assert while size_from_wm: ignore (stale max buffer after unmaximize)

host_authority = size_from_wm || tiled

if !host_authority && mismatch:
    fit host to preferred (both directions), unless A→B→A within 400ms
elif host_authority && mismatch:
    do not move host; wlx_toplevel_fill_host(...)
```

### 4.4 `wlx_toplevel_fill_host`

Computes logical size from current **content** output mode (CSD margins
subtracted only when not tiled). Calls `set_size` unless both `last_conf`
and current geometry already match that size (±1px).

---

## 5. Policy table

| Situation | Move X window? | Client configure |
|-----------|----------------|------------------|
| Map / floating preferred grow | Yes (fit) | set_size to preferred |
| Floating preferred shrink | Yes (fit) | set_size to preferred |
| Floating rapid A→B→A | No (oscillation) | unchanged |
| client_assert **shrink** while floating | Clears size_from_wm; then fit | as fit |
| client_assert **grow** while size_from_wm | No (keep host; fill_host) | fill_host |
| Host interactive resize | Yes (request_state) | fill_host |
| Maximized / fullscreen | Only via WM | fill_host until geometry matches |
| Preferred small while max | No | fill_host (re-assert) |

---

## 6. Failure modes this policy targets

| Failure | Mitigation |
|---------|------------|
| Self ConfigureNotify as request_state | Backend pending match |
| Host resize dropped by soft-ignore | No compositor awaiting loop |
| 557↔2560 thrash | Oscillation guard |
| Content shrink blocked forever | Bidirectional fit + client_assert |
| Small surface in large host | fill_host under host authority |
| Maximize focus-loss letterbox | fill_host on every lagging commit while tiled |
| Frame size fed into mode | Never; content window only |

---

## 7. Automated client: `tools/resize-torture`

Raw xdg-shell stress client (flake output `.#resize-torture`) cycles:

map → grow → shrink → oscillate → restable → follow →
maximize → undersize_while_max → unmaximize → done

```bash
nix run .#resize-torture -- --loop
# under wl-x11: compare stderr with compositor size: lines
```

See `tools/resize-torture/README.md`.

## 8. Manual verification checklist

- [ ] Initial map grows from 1×1 to client preferred once.
- [ ] Content-driven shrink/grow moves the X window when floating.
- [ ] Interactive WM resize updates Wayland via fill_host.
- [ ] Maximize: client fills content area; no permanent letterbox.
- [ ] Focus loss while maximized: still filled (or re-configure), host stays max.
- [ ] Unmaximize: size_from_wm cleared; client can preferred-fit again.
- [ ] Rapid maximize races do not thrash host size.

---

## 9. Changelog (design)

| Date | Change |
|------|--------|
| 2026-07 | Backend pending self-configure; drop awaiting_* |
| 2026-07 | Grow-only preferred fit (later revised) |
| 2026-07 | Bidirectional fit + oscillation guard + client_assert |
| 2026-07 | **Host authority + fill_host** so maximized/focus-loss cannot leave a small surface in a large X window; notes in §0 |
