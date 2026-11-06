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
- **`staged`**: the drain workers run concurrently from `Open()` (exactly as
  in durable mode), so the node-local tier holds only the *in-flight drain
  backlog* — not the whole run — no matter how many steps the session has.
  `Close()` hands the remaining session (leftover staging files, data sub-file
  fd, metadata transports, commit queue) to the process-level finalizer and
  returns as soon as the last step has reached the node-local tier; the
  metadata commit and the drain remainder continue in the background while the
  application computes. A clean process exit finalizes every pending session
  before the process ends; a crash loses checkpoints that were staged but not
  yet finalized. Finalization publishes
  `<checkpoint-dir>/bbstage.final` — the durability/visibility marker that
  readers and restart policies should key on. **Never delete a staged
  checkpoint before that marker exists.**

## Key parameters

| Parameter          | Default | Meaning                                                                                          |
|--------------------|---------|--------------------------------------------------------------------------------------------------|
| `BurstBufferPath`  | `$TMPDIR` | Node-local staging directory (point it at the node's NVMe/SSD)                                |
| `CloseMode`        | `durable` | `durable` or `staged` (see above)                                                              |
| `StagingWatermarkMB` | `1024` | staged mode: cap on staged-but-undrained bytes; `EndStep` blocks above it until the concurrent drain catches up, then throws after `CommitTimeoutSecs` without drain progress (0 = unlimited). Per rank in both aggregation modes |
| `DrainThreads`     | `1`     | drain worker threads per rank (run from `Open` in both close modes)                              |
| `CommitTimeoutSecs`| `300`   | give up waiting on a stuck peer rank after this long (0 = forever); in staged mode also the stall timeout of the watermark backpressure |
| `Aggregate`        | `off`   | `on`: node-aggregated co-write drain. Local staging stays per-rank; the SSD -> final-store drain writes one `data.<node>` sub-file per node that every rank on the node co-writes (each rank drains its own slices at pre-assigned disjoint offsets: one store client per rank, one file per node). Data sub-file count scales with node count, not rank count. Off critical path. Must be uniform across ranks |

When bbstage is reached through middleware that cannot pass engine
parameters (e.g. the WarpX checkpoint path via openPMD-api), these
environment variables act as fallbacks (precedence: parameter > env var >
default): `BBSTAGE_CLOSE_MODE`, `BBSTAGE_STAGING_WATERMARK_MB`,
`BBSTAGE_AGGREGATE` (`on`/`off`),
`BBSTAGE_TIMING` (=1: per-phase Open/Close timing and per-rank drain
throughput lines on stderr).

Full parameter list, protocol semantics, and limitations:
[`docs/user_guide/source/engines/bbstage.rst`](docs/user_guide/source/engines/bbstage.rst).

## Notes

- Write-only engine; the read side is the stock `bp5` engine. POSIX staging only.
- **Aggregate co-write drain** (`Aggregate=on`): motivated by metadata/file-count explosion at
  large rank counts. With the stock per-rank layout, a job with R ranks
  writing S separate BP5 files (e.g. WarpX's one-file-per-step) creates R x S
  data sub-files on the final store; the parallel store's metadata servers and
  the post-processing readers then have to deal with that many files.
  `Aggregate=on` keeps the fast per-rank node-local staging (the
  app-visible critical path is unchanged) and drains into one `data.<node>`
  sub-file per node **co-written by every rank on the node**: each rank
  drains only its own staging slices, at the pre-assigned disjoint offsets of
  the shared sub-file, so the node gets one store client per rank. The BP5
  writer map routes every rank to its node's sub-file with a pre-assigned
  intra-node offset (known at staging time, so the drain stays a pure byte
  copy), and the rank-0 committer gates metadata commits on one progress
  record per node (the node leader folds the node's node-local per-rank
  records into it). The output is still stock-BP5 readable.
  Co-write is deliberate: a single drain client per node is capped at the
  store's single-client write rate (~0.7 GiB/s measured on the RZ Lustre),
  which in `staged` mode throttles the app back to the store rate and
  destroys the staging win; one client per rank keeps the drain parallelism
  of the non-aggregated layout at the file count of aggregate. The
  multi-client single-file pattern is the same one BP5's own aggregator
  chains use.
- **Field validation (RZ Lustre, 8 nodes x 32 ranks, 64 GiB/checkpoint,
  10 checkpoints, both engines in one job minutes apart):** with the store
  contended (native BP5 managed only 2.5 GiB/s), staged + aggregate ran
  7.7 s vs 26.5 s mean per checkpoint (**3.4x**) and 100 s vs 283 s total
  job wall (**2.8x**); per-rank `drain-stats` showed the background drain
  streaming ~1.2 GiB/s per node while the app stayed at its compute floor.
  On fast-store days both engines tie at the application floor (nothing to
  hide); the staged win is exactly proportional to store contention, which
  is the intended regime. Output verified stock-`bpls` readable with
  exactly `data.0..data.7` per checkpoint.
- **Two Lustre-specific mechanisms the co-write drain needed** (both
  measured and fixed; they are why an earlier aggregate design drained at
  the single-client rate):
  1. *Shared-inode size lock*: with 32 clients writing a fresh `data.<node>`
     each checkpoint, every drain write lands beyond the current EOF, and
     Lustre maintains inode size on the MDT — concurrent EOF-extending
     writes serialize on the size lock (the classic multi-client append
     pathology; it also explains why `lfs setstripe` did not help). The
     node leader now `ftruncate`s the sub-file to the step end at
     `EndStep` (one MDT op per node per step); drain writes then land
     below EOF on disjoint ranges and parallelize.
  2. *Close waiting on an in-flight copy*: staged `Close()` joined the
     drain workers, so on a store-capped drain every checkpoint paid one
     full drain copy at Close (measured: 13.9 s on all 256 ranks). Close
     now aborts the in-flight copy at its next 4 MiB chunk boundary and
     hands it to the finalizer, which re-drains it idempotently (same
     bytes, same pre-assigned offsets). Close cost is bounded by one
     chunk (measured: 1 ms).
- **Tuning notes:** use the default watermark (1 GiB) for production — it
  self-throttles the app when the store cannot keep up and keeps /tmp
  usage and the exit tail small; a huge watermark (e.g. 64 GiB) instead
  banks the backlog in /tmp (must hold watermark x ranks/node of slack)
  and shifts repayment to process exit. `BBSTAGE_TIMING=1` prints
  per-phase Close timings and per-rank `drain-stats` MiB/s lines — the
  direct measure of drain throughput, independent of app phases.
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
