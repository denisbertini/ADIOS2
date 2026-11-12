# ADIOS2 — bbstage fork

Fork of [ORNL ADIOS2](https://github.com/ornladios/ADIOS2) adding
**bbstage**: a write-only engine with node-local staging and asynchronous
drain that decouples checkpoint cost from the final store's speed and
jitter. `master` = pinned upstream base + a handful of purely additive
commits (`source/adios2/engine/bbstage/` + registration lines).

**📖 Documentation: <https://denisbertini.github.io/bbstage-docs/>**
— quickstart, parameters, durability semantics, and *tuning by
measurement* (every constant tied to its measurement).

| resource | where |
|---|---|
| Documentation site | <https://denisbertini.github.io/bbstage-docs/> |
| Engine overview (in-repo) | [`BBSTAGE.md`](BBSTAGE.md) |
| Full parameter/protocol reference | [`docs/user_guide/source/engines/bbstage.rst`](docs/user_guide/source/engines/bbstage.rst) |
| Engine source | `source/adios2/engine/bbstage/` |
| Tests | `testing/adios2/engine/bbstage/` (`ADIOS2_BUILD_TESTING=ON`) |
| Sibling project (same architecture at the POSIX boundary) | [denisbertini/bblite](https://github.com/denisbertini/bblite) |

One-paragraph summary: each step is spilled synchronously to fast
node-local media while background workers drain it to the final store in
large sequential writes as the application computes. Output is standard
BP5 — stock readers, no changes. Under store contention this measured
**3.4×** per-checkpoint improvement; on quiet stores it correctly ties
with the native engine (the win is proportional to contention, by
design).
