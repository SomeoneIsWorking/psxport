# 0204 — Visual mods: replacement textures and models, with HTTP-driven editors

User, 2026-11-13: "we can maybe mod some visuals in other games too, I was thinking maybe we can remodel
Tomba 2 Tomba for example to be higher quality but we need tools for that, we might need an agentic tool
for 3D model editing and texture editing" … "By 'agentic' I mean a tool you can interact with by like
HTTP" … "We can make our own tools like texture editor and model editor with HTTP hooks".

## Shape

- Player-supplied packs loaded at run time and never distributed: replacement textures keyed by the
  guest's VRAM upload, and replacement models drawn in place of a recognised guest model (first case: a
  higher-quality Tomba in Tomba 2).
- Our own texture editor and model editor, each serving an HTTP control surface through
  `lucent::http::Server`, so an agent can inspect and edit beside a person.

## Prior art (2026-11-13)

- DuckStation's texture replacement (experimental) and Beetle PSX HW's use different, incompatible
  keying; most community packs target Beetle PSX HW
  ([DuckStation wiki](https://github.com/stenzek/duckstation/wiki/Texture-Replacement)).
- Blender has HTTP-controlled addons for agents (mcp-for-blender, formerly blender-mcp;
  jlpschell/blender-mcp-server; dcc-mcp-blender): evidence for the command surface, licences unchecked.
- Daemeous's rood-boi fork (CC0) loads HD texture packs for Vagrant Story: its loader may be copied.
- VS Reimagined's 4K texture pack forbids use with other ports or emulators without written permission
  from HDRcade.

## Phases

1. Survey Tomba 2's model and texture formats and where a replacement would hook in.
2. Texture pack loader (keying, invalidation on VRAM writes) in psxport.
3. Model replacement in the record path.
4. The editors and their HTTP surfaces.

## Progress

Not started.
