---
id: 19
title: Already-60fps ports still depend on the interpolation owner for frame commit
status: open
symptom: Mega Man X4's game/core/vsync_sync.cpp calls c->game->fps60.frame_commit(c,1) even though the title is already 60fps and should own no interpolation capture/queue state.
tags: fps60,interpolation,presentation,architecture,megamanx4
created: 2026-08-22
updated: 2026-08-22
---

Future generic ownership fix. Extract a non-temporal present/pace/field/capture fence outside Fps60. Native 30fps titles layer interpolation rotation on that base owner; already-60fps/widescreen-only titles such as Mega Man X4 call only the base fence. Do not solve this with an inactive flag or a Mega Man X4 special case: even inactive use leaves interpolation as the lifecycle owner.
