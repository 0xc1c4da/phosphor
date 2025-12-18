# Phosphor

A native UTF‑8 ANSI / text-mode art editor.

Phosphor stores glyphs as Unicode codepoints based on the [Unscii](http://viznut.fi/unscii/) 8×16 font, and focuses on practical workflows for ANSI art: layers, palettes, scripting, SAUCE metadata, and browsing 16colo.rs packs.

![Phosphor Screenshot](dist/screenshot.png)

## Features

- Native UTF‑8 canvas: store and edit Unicode codepoints directly (Unscii-backed).
- Layers: add/remove, reorder, rename, and toggle visibility.
- Undo/redo: project-aware undo history persisted inside `.phos`.
- Color tools: xterm‑256 color picker + standard palette browser/management.
- Character tools:
  - Unicode Character Picker (ICU-backed) with search and metadata.
  - Character Palettes + Character Sets.
- Scriptable workflows:
  - ANSL editor (LuaJIT): write generative "shaders" that render into the active layer.
  - Tool palette (LuaJIT): all tools are Lua scripts with parameter UI (e.g. pencil/eraser/fill/select).
- Image support:
  - Open common images (`.png/.jpg/.gif/.bmp`) and preview them.
  - Convert images to ANSI-like character art via Chafa (“Convert to ANSI…” creates a new canvas).
- 16colo.rs browser:
  - Browse packs, thumbnails, and download/open remote artwork.
  - HTTP responses are cached on disk for speed/offline-ish reuse.
- SAUCE metadata:
  - Parse SAUCE when importing ANSI/text where present.
  - Edit SAUCE fields in-app and persist them in `.phos` project files.

## File formats

- Project: `.phos`
  - Stored as CBOR, compressed with zstd.
  - Includes layers, colors, caret state, undo/redo history, and SAUCE metadata snapshot.
- Import:
  - ANSI / text: `.ans`, `.asc`, `.txt` (with UTF‑8 vs CP437 detection).
  - Images: `.png`, `.jpg`, `.jpeg`, `.gif`, `.bmp`.
- Export:
  - Not implemented yet (menu item exists, but saving/exporting `.ans` is currently a stub).

## UI windows

From the menu you can toggle:

- Xterm‑256 Color Picker
- Unicode Character Picker
- Character Palettes
- Character Sets
- Layer Manager
- ANSL Editor
- Tool Palette
- Preview (minimap with pan/zoom interaction)
- 16colo.rs Browser
- Settings (theme + key bindings editor)

## Configuration + data directories (Linux)

Phosphor uses `XDG_CONFIG_HOME` when available; otherwise defaults to `~/.config/phosphor`.

- `session.json`: UI/window state, open canvases/images, active tool, theme, etc.
- `assets/`: extracted bundled assets on first run (fonts, palettes, key bindings, tools, ANSL examples).
- `cache/`: cache files (including HTTP cache for the 16colo.rs browser).

## Quick start

If you don't have Nix installed yet, see the official installer: `https://nixos.org/download/`.

Run Phosphor directly from the flake:

```bash
nix run github:0xc1c4da/phosphor#phosphor
```

If you have the repo cloned locally:

```bash
nix run .#phosphor
```

## Building from source

This repo builds a native binary called `phosphor`.

### With Nix

```bash
nix develop
make
./phosphor
```

You can also use:

```bash
nix build
nix run
```

### Without Nix

- Install dependencies for: SDL3, Vulkan, Dear ImGui, Chafa, ICU, LuaJIT, zstd, libcurl, nlohmann_json, and stb headers.
- Set `IMGUI_DIR` to a checkout of `ocornut/imgui` or checkout in `vendor/imgui`:

```bash
make IMGUI_DIR=/path/to/imgui
```

## License

MIT License. See [LICENSE](LICENSE).

## Links

- Unscii font: [http://viznut.fi/unscii/](http://viznut.fi/unscii/)
- ANSI art resources: [https://16colo.rs/](https://16colo.rs/)