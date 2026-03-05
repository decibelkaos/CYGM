# WORKFLOW.md - Vibe Coding Operating Mode

## Core Loop
1. Plan a small slice (30-90 minutes max)
2. Implement only scoped files
3. Run checks/build
4. Log outcomes in `Documents/notes/bringup-log.md`
5. Record decisions in `Documents/notes/decisions.md`
6. Move next task in `TASKS.md`

## Prompt Contract (use this format)
- Goal:
- Scope (files/folders allowed):
- Constraints:
- Verification commands:
- Deliverable:

## Rules
- Prefer additive changes.
- Show diff summary before risky edits.
- Stop and ask before destructive operations.
- Keep commits small and reversible.
- Update docs whenever behavior changes.
