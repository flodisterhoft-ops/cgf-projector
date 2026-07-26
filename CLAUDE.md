# CGF Projector

Read **AGENTS.md** in this directory first — it is the project master brief
(what this fork is, the two-track strategy, locked architecture decisions,
related projects and paths, guard rails, roadmap). It applies to all agents
working here.

Non-negotiables (details in AGENTS.md):
- This is a PUBLIC repo — read NEVER-COMMIT.md before any push.
- Projection must work fully offline; sync is snapshot-down / proposals-up.
- The church's production SoftProjector and its spData.sqlite are read-only
  to this project; all testing uses copies.
- Build + smoke test against the DB copy before every commit (see BUILD.md).
