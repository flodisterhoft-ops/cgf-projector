# The What's new window (2026-11-28, app 0.2.9)

Florian asked for this so Valentin, who runs the program and looks after the
lyrics, can see what changed without anyone explaining it. Short lines with
an icon and a coloured tag (New, Changed, Fixed, Removed), no long text, and
numbers instead of song lists when a batch touched many songs. It replaces
the bar at the bottom right that offered "Restart now".

## When it opens

- Once, about 1.5 s after the program starts, on a laptop installed from the
  package (the `app-<version>` layout), and only when something changed
  since it last opened. At start nothing is on the projector, so it never
  appears during a service. It does not block anything (`QDialog::open`).
- Help > What's new opens the last content again.
- Program updates install quietly; the next start opens the new version
  (docs/song-push.md, "App updates"). Library updates still apply at once;
  their summary waits for the next start.

## What it shows

- **Program:** the items of every version after the one last shown, up to
  this build, from `src/whats-new.json` (compiled into the program). On the
  first start after an update from a version without this window, the
  launcher's `previous=` line in `cgf-versions.txt` counts as last shown, so a
  laptop going from 0.2.6 to 0.2.9 sees everything since 0.2.6. A new
  installation shows no history.
- **Library:** what the applied releases added, updated or removed, recorded
  by `LibraryPush` (`libraryRecorded`) in `cgf-whats-new.json` beside the
  database. One line per Bible; for songbooks and songs one line per kind of
  change. Up to 5 songs of a kind are listed by number and title; more show
  only the number ("5,000 songs updated"). A song added and then edited before
  the next start is still new; one added and removed again is left out.
- The heading names both versions: "Program 0.2.9, Library v5".

## Writing the program items

Add an entry at the top of `src/whats-new.json` for each version before it is
published: `{"version": "0.3.0", "items": [{"kind": "new" | "changed" |
"fixed" | "removed", "title": ..., "detail": ...}]}`. The newest entry is
the version the build describes. Keep titles under about 45 characters and a
detail under about 60; plain words a volunteer would use. The first texts
and the library lines were reviewed with GPT Astra 6.2 (high effort) on
2026-11-28 at Florian's request.

`cgf-whats-new.json` is a laptop's own file: `push_release.py` never ships it
in an app release.

## Checks

`tests/push` (69 checks) records the test releases, checks the lines,
the 5-song limit, a 5,000-song batch, "shown once" and Help > What's new, and
saves `whats-new-laptop.png` and `whats-new-batch.png` in `tests/push/out/`.
