*************
bbstage Engine
*************

.. versionadded:: 2.12

The **bbstage** engine is a write-only BP5 engine with node-local
burst-buffer staging and asynchronous drain: **no daemon service is required
on any node**. The staging + background-drain approach follows `BBView: A
View-Aware Burst-Buffer Mechanism for MPI-IO <https://github.com/tsukuba-hpcs/bbview>`_
(Koyama & Tatebe, REX-IO 2025); unlike BBView there is no daemon and no view
transformation -- the staged payload is already serialized in final BP5
layout, so the background work is a byte copy plus a deferred metadata commit.

Idea
----

BBView's observation is that an application's payload can be buffered in a
cheap local layout while the expensive placement work is *deferred* and
replayed in a single sequential pass by a background agent (in BBView: the MPI
file view, staged out by ``bbviewd`` onto the parallel filesystem).

The engine implements this contract fully in-process:

* ``EndStep()`` serializes the step in memory and writes the payload
  **synchronously to a per-rank staging file on node-local SSD**
  (``BurstBufferPath``). This is a single contiguous local write; the
  application's memory is released immediately, so in-flight steps are bounded
  by SSD, not by RAM.
* **In-process drain threads** (the daemonless replacement of ``bbviewd``)
  copy each staged step to its pre-assigned position in the final store.
  The copy is asynchronous: the application proceeds with the next step.
* **Deferred metadata commit**: the per-step metadata (the ADIOS2 analogue of
  BBView's view descriptor) is aggregated collectively at ``EndStep()`` time,
  but a rank-0 *committer* thread appends it to the BP5 metadata files
  (``md.0`` / ``md.idx`` / ``mmd.0``) only after **every** writer rank has
  drained that step's data. The metadata commit is the visibility point.
  Ranks publish their drain progress through a small per-rank record file
  (``bbstage.drain.<rank>``) in the output directory, updated with fixed-size
  sequence-locked writes (no renames), so the coordination also works on
  network/FUSE mounts that cache directory lookups.
* Consequences:

  - The output is a **standard BP5 stream** and is readable by the stock
    ``bp5`` engine -- both after ``Close()`` and, for streaming readers, step
    by step as steps commit. A reader can never observe a step whose data has
    not landed on the final store.
  - In ``durable`` mode (default), ``Flush()`` and ``Close()`` are the
    durability points: they block until all steps are drained and committed.
  - In ``staged`` mode (``CloseMode=staged``), ``Close()`` returns as soon as
    the last step has reached the node-local tier; the drain and the metadata
    commit then run in the process background while the application computes.
    A clean process exit finalizes every pending session before the process
    ends; a crash loses checkpoints that were staged but not yet finalized.
    Finalization publishes ``<checkpoint-dir>/bbstage.final`` -- the
    durability/visibility marker that readers and restart policies should key
    on. Never delete a staged checkpoint before that marker exists.
  - Unlike BBView's fire-and-forget flush, **drain errors are reported** to
    the application at the next ``EndStep()`` / ``Flush()`` / ``Close()``, and
    the committer gives up (with an explicit error naming the stuck rank)
    after ``CommitTimeoutSecs``.

Engine name and read side
-------------------------

Use ``io.SetEngine("bbstage")``. In write mode this activates the burst-buffer
writer. In read mode the engine maps to the standard BP5 reader, since the
staged-and-drained output is ordinary BP5::

    adios2::IO io = adios.DeclareIO("bb");
    io.SetEngine("bbstage");
    io.SetParameters({{"BurstBufferPath", "/scratch/local"}});
    adios2::Engine engine = io.Open("out.bp", adios2::Mode::Write);
    for (...) { engine.BeginStep(); engine.Put(var, data); engine.EndStep(); }
    engine.Close();   // drains the burst buffer

Readers may simply use ``bp5`` (or ``bbstage``), streaming or random access.

Engine parameters
-----------------

bbstage-specific parameters:

+--------------------+---------+-----------------------------------------------+
| Parameter          | Default | Description                                   |
+====================+=========+===============================================+
| BurstBufferPath    | $TMPDIR | Directory of the node-local staging tier      |
|                    | or /tmp | (BBView's ``BBVIEW_TMP_DIR``)                 |
+--------------------+---------+-----------------------------------------------+
| DrainThreads       | 1       | Drain worker threads per rank (max 16)        |
+--------------------+---------+-----------------------------------------------+
| StagingSync        | false   | ``fsync`` the staging file inside ``EndStep`` |
+--------------------+---------+-----------------------------------------------+
| DrainDataSync      | false   | ``fsync`` the final data file at each         |
|                    |         | ``Flush()``/``Close()`` drain barrier         |
+--------------------+---------+-----------------------------------------------+
| CommitPollMs       | 2       | Committer poll interval for drain progress    |
+--------------------+---------+-----------------------------------------------+
| CommitTimeoutSecs  | 300     | Timeout for waiting on a peer rank's drain    |
|                    |         | before failing the stream (0 = forever)       |
+--------------------+---------+-----------------------------------------------+
| DeleteStagedFiles  | true    | Remove staging files once drained             |
+--------------------+---------+-----------------------------------------------+
| CloseMode          | durable | ``durable`` (default): ``Close()`` waits for  |
|                    |         | drain + commit. ``staged``: ``Close()``       |
|                    |         | returns at the node-local dump; drain +       |
|                    |         | commit run in the background, finalized at a  |
|                    |         | clean exit (see above)                        |
+--------------------+---------+-----------------------------------------------+
| StagingWatermarkMB | 1024    | staged mode: per-rank cap on staged-but-      |
|                    |         | undrained bytes (MiB); ``EndStep`` blocks     |
|                    |         | above it (0 = unlimited)                      |
+--------------------+---------+-----------------------------------------------+

Environment variables
~~~~~~~~~~~~~~~~~~~~~

When the engine is reached through middleware that cannot pass engine
parameters (e.g. the WarpX checkpoint path via openPMD-api), the following
environment variables act as fallbacks. Precedence: engine parameter >
environment variable > default.

+--------------------------+---------------------------------------------+
| Variable                 | Effect                                      |
+==========================+=============================================+
| ``BBSTAGE_CLOSE_MODE``   | ``durable`` or ``staged`` (``CloseMode``)   |
+--------------------------+---------------------------------------------+
| ``BBSTAGE_STAGING_WA-    | watermark in MiB                            |
| TERMARK_MB``             | (``StagingWatermarkMB``)                    |
+--------------------------+---------------------------------------------+
| ``BBSTAGE_TIMING``       | ``1`` prints one per-phase Open/Close       |
|                          | timing line per rank to stderr (default:    |
|                          | off)                                        |
+--------------------------+---------------------------------------------+

Shared BP5 parameters are honored where they apply:
``BufferChunkSize``, ``InitialBufferSize``, ``GrowthFactor``, ``BufferVType``,
``StripeSize``, ``StatsLevel``, ``UseOneTimeAttributes``, ``FlattenSteps``,
``verbose``.

Limitations (first iteration)
-----------------------------

* POSIX only (node-local staging via POSIX file I/O).
* Write mode only; ``Append`` is not supported yet.
* ``Flush()`` between steps only (not inside a step);
  ``MemoryStart``/``MemoryCount`` Put sub-blocks are not supported.
* Reading steps that are still sitting in the burst buffer (BBView-style
  read-your-writes from local SSD across ranks) is planned; until then,
  readers see a step once it is drained+committed, which is exactly when
  the standard ``bp5`` reader may safely see it.
