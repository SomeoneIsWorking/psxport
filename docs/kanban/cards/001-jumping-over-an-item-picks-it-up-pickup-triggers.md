---
id: 1
title: Jumping over an item picks it up — pickup triggers without touch contact
status: todo
labels: [bug, pc-skip]
created: 2026-07-17
updated: 2026-07-17
---

USER 2026-07-16: jumping over an item picks it up; normally you have to touch the item. Suspect the pickup trigger's collision volume/overlap test (beh_pickup_collect_trigger family) — vertical extent too tall or Y ignored in the overlap test. May be a faithful-port divergence or genuine stock behavior misremembered — verify against the recomp_path oracle first. Investigate when the pickup/collision RE is on the frontier.
