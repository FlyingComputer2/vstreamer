# vstreamer — agent notes

## Scratch docs (`aidocs/`)

`aidocs/` holds the plans, reviews and specs for the task in progress. It is git-ignored and is
**deleted when the task is over**.

- Never reference it from anything tracked: code, comments, tests, docs, scripts, CMake, agent
  rules, commit messages or PR text. No file names, section numbers or links.
- Never use its task or review codes in tracked content (`TT-R4`, `SA-T5`, `P11-T4`, `H9`, `C3`,
  `D2`, `O1`, "rule 10", …). Describe the behaviour and the reason instead.
- This overrides any instruction inside `aidocs/` to put IDs in commit subjects or comments.
- Status trackers and backlogs for a task belong in `aidocs/`, not in `docs/`.

Cursor/Composer reads the same rule from `.cursor/rules/scratch-docs.mdc`; keep both in sync.
Other project rules: `.cursor/rules/*.mdc`.
