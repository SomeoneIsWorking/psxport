# Ideas

Maybes not yet asked for as work.

- **Hybrid crop + extend widescreen (user, 2026-11-13).** For sinks wider than 16:9, crop the picture to an
  intermediate aspect and extend the sides for the rest, so neither the crop nor the margins grow extreme.
  Would build on plan 0203's per-layer mappings and the per-record view/drawn windows.
- **Visual mods: replacement textures and models (user, 2026-11-13).** Player-supplied packs loaded at run
  time and never distributed: replacement textures keyed by the guest's VRAM upload, and replacement
  models (first case: a higher-quality Tomba in Tomba 2) drawn in place of a recognised guest model.
  Authoring needs our own texture editor and model editor, each driven over HTTP (`lucent::http::Server`)
  so an agent can inspect and edit, beside a person. Prior art (2026-11-13):
  - DuckStation's texture replacement (experimental) and Beetle PSX HW's use different, incompatible
    keying; most community packs target Beetle PSX HW
    ([DuckStation wiki](https://github.com/stenzek/duckstation/wiki/Texture-Replacement)).
  - Blender already has HTTP-controlled addons for agents (mcp-for-blender, formerly blender-mcp;
    jlpschell/blender-mcp-server; dcc-mcp-blender): evidence for the command surface, licences unchecked.
  - Daemeous's rood-boi fork (CC0) loads HD texture packs for Vagrant Story: its loader may be copied.
  - VS Reimagined's 4K texture pack forbids use with other ports or emulators without written
    permission from HDRcade, so it is usable only if they grant it.
