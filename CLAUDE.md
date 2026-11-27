# Working in apps (the wapps)

Wapp source. The app repo (`../app`) hosts these wapps and its `docs/architecture.md` governs them: a wapp hands the core a payload and is called back; it never decides how bytes travel, and it never polls for what the host can tell it (`system.*` topics, `core.media`).

## MUST: architecture.md and performance.md, before AND after writing code

These are standing rules for every session in the xprs tree, every time:

1. **Before** planning or writing any code, read `../app/docs/architecture.md` (in full, including
   "Before and after writing code" at its top) and `../app/docs/performance.md` §8 (at least §8.5
   and §8.31). In the plan, name the architecture.md §3 layer of every piece
   (core, wapp, screen) and how it learns of changes (which event it listens
   to). Do not start coding before this read, even for a small fix.
2. **While** writing: no `Timer.periodic` / `Stream.periodic` under a minute
   (also when the interval is passed by a name), no sleep in a loop, no
   one-shot timer re-arming itself while there is nothing pending. Wait for
   the event that says it happened, with a one-shot deadline; if the event
   does not exist, add it in the layer that knows (architecture.md §3,
   "Change arrives as an event"). The host never compares a wapp's name;
   the wapp declares what it needs.
3. **After** writing, before committing: run the guard
   (`cd ../app && xbuild $HOME/flutter/bin/dart tool/arch_guard.dart`) and
   `test/arch_guard_test.dart` in the app; both must pass with no new
   violation. Then re-read the diff against architecture.md's checklist: the
   guard matches spellings, and a shape it misses is still a violation (widen
   the rule and add its sample to `test/arch_guard_test.dart` the same day).
4. A violation is fixed, never `arch-ignore`d and never baselined, even when
   it is outside the current task. When a baselined line is fixed, shrink the
   baseline (`--baseline`) and check the diff only removes lines.

