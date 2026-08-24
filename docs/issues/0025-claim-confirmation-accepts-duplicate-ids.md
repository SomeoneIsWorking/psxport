---
id: 25
title: Claim confirmation accepts duplicate IDs
status: open
symptom: info.py claim confirm C025 reports success but updates the first of two C025 files, leaving the intended claim stale
tags: project-info,claims,tooling,duplicate-id
created: 2026-08-25
updated: 2026-08-25
---

## Root cause\n\nThe claim loader permits duplicate frontmatter IDs and the confirm command selects one match without refusing ambiguity. This repository contains two distinct C025 claims, so a successful confirmation can mutate the wrong evidence record.\n\n## Proper fix\n\nThe loader must enforce globally unique claim IDs for add/list/check/confirm/falsify and refuse with every conflicting path before writing. Add a regression with two files sharing one ID.\n\n## Current handling\n\nThe mistaken edit was reverted explicitly and the intended recompiler claim was updated by exact path. Do not trust ID-based mutation until uniqueness is enforced.\n
