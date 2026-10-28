# bbstage — node-local staging + asynchronous drain BP5 engine (fork addition)

**bbstage** is a write-only ADIOS2 engine added by this fork. It decouples the
application-visible checkpoint cost from the (slow) final store: each step is
spilled synchronously to fast node-local media (NVMe/SSD), and per-process
background workers drain it to the final output in large sequential writes
while the application computes.

## Quickstart

```cpp
adios2::IO io = adios.DeclareIO("chk");
io.SetEngine("bbstage");
io.SetParameters({{"BurstBufferPath", "/tmp"},   // node-local staging dir
                  {"CloseMode", "staged"}});     // or "durable" (default)
adios2::Engine e = io.Open("chk.bp", adios2::Mode::Write);
// ... BeginStep / Put / EndStep ...
e.Close();
```

The output is **standard BP5**: read it back with the stock `bp5` engine,
`bpls`, or any other BP5 reader — no reader-side changes.

## Close modes

- **`durable`** (default): `Close()` blocks until all data is drained and
  metadata committed — classic semantics, checkpoint durable on return.
- **`staged`**: `Close()` returns as soon as the last step has reached the
  node-local tier; the drain and the metadata commit continue in the process
  background. A clean process exit finalizes every pending session before the
  process ends; a crash loses checkpoints that were staged but not yet
  finalized. Finalization publishes `<checkpoint-dir>/bbstage.final` — the
  durability/visibility marker that readers and restart policies should key
  on. **Never delete a staged checkpoint before that marker exists.**

## Key parameters

| Parameter          | Default | Meaning                                                                                          |
|--------------------|---------|--------------------------------------------------------------------------------------------------|
| `BurstBufferPath`  | `$TMPDIR` | Node-local staging directory (point it at the node's NVMe/SSD)                                |
| `CloseMode`        | `durable` | `durable` or `staged` (see above)                                                              |
| `StagingWatermarkMB` | `1024` | staged mode: per-rank cap on staged-but-undrained bytes; `EndStep` blocks above it (0 = unlimited) |
| `DrainThreads`     | `1`     | drain worker threads per rank                                                                    |
| `CommitTimeoutSecs`| `300`   | give up waiting on a stuck peer rank after this long (0 = forever)                               |

Full parameter list, protocol semantics, and limitations:
[`docs/user_guide/source/engines/bbstage.rst`](docs/user_guide/source/engines/bbstage.rst).

## Notes

- Write-only engine; the read side is the stock `bp5` engine. POSIX staging only.
- Design lineage: the staging + background-drain approach follows
  [BBView](https://github.com/tsukuba-hpcs/bbview) (Koyama & Tatebe,
  REX-IO 2025), implemented fully in-process. There is no view
  transformation: the staged payload is already serialized in final BP5
  layout, so the background work is a byte copy plus a deferred metadata
  commit.
- Fork layout: `master` = pinned upstream base + bbstage. The engine is
  purely additive (`source/adios2/engine/bbstage/` plus a few registration
  lines), so re-aligning `master` to a newer upstream is a small rebase of a
  handful of commits.
- Tests: `testing/adios2/engine/bbstage/` (built with `ADIOS2_BUILD_TESTING=ON`).
