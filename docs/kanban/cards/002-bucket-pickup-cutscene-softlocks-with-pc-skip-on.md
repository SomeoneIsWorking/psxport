---
id: 2
title: Bucket-pickup cutscene SOFTLOCKS with pc_skip ON (default config)
status: todo
labels: [bug, pc-skip]
created: 2026-07-17
updated: 2026-07-17
---

USER live 2026-07-15: picking up a bucket initiates a cutscene that SOFTLOCKS under the default config (pc_skip=true). Likely a collapsed-init/wait fork in the cutscene trigger where the pc_skip shortcut branch fails to advance (a wait that never completes, or a phase-gate counter not bumped — cf. the pc_skip frame-counter-bump rule). Repro: replay the bucket-pickup pad capture default config, watch for the cutscene hang after bucket pickup; compare pc_skip=false (SBS Core A / MODE=skip) which should progress. Blocks gameplay progress.
