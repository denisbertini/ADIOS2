/*
 * SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ADIOS2_ENGINE_BBSTAGE_BBSTAGEWRITER_H_
#define ADIOS2_ENGINE_BBSTAGE_BBSTAGEWRITER_H_

#include "adios2/common/ADIOSConfig.h"
#include "adios2/core/Engine.h"
#include "adios2/engine/bbstage/BbstageDrainCommon.h"
#include "adios2/engine/bbstage/BbstageStagedFinalizer.h"
#include "adios2/engine/bp5/BP5Engine.h"
#include "adios2/toolkit/format/bp5/BP5Serializer.h"
#include "adios2/toolkit/transport/Transport.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace adios2
{
namespace core
{
namespace engine
{

/** Engine "bbstage": write-only BP5 engine with node-local burst-buffer
 * staging and asynchronous drain.
 *
 * Design lineage: the "spill the payload to node-local media as fast as
 * possible, drain to the final store in the background" approach follows
 * bbview (Koyama & Tatebe, "BBView: A View-Aware Burst-Buffer Mechanism for
 * MPI-IO", REX-IO 2025). bbview defers an MPI file-view transformation to a
 * separate daemon (bbviewd); this engine keeps only the staging +
 * background-drain idea and implements it fully in-process. The staged
 * payload is already serialized in final BP5 layout, so the background work
 * is a byte copy plus a metadata commit -- there is no view descriptor and
 * no data re-organization.
 *
 *   - EndStep() serializes the step in memory (BP5Serializer), then writes
 *     the step payload synchronously to a per-rank staging file on
 *     node-local SSD (BurstBufferPath). This is the app-visible cost: a
 *     single sequential local write.
 *   - One or more in-process worker threads (the daemonless stand-in for
 *     bbviewd) drain the staging files to the final store at the
 *     pre-assigned data offsets.
 *   - The per-step metadata is aggregated collectively at EndStep() time,
 *     but its *commit* to the BP5 metadata files (.bp md.0/md.idx/mmd.0) is
 *     performed by a rank-0 committer thread only once the data of that
 *     step has been drained on every writer rank. Aggregate mode: the
 *     committer instead gates on one progress record per node
 *     (bbstage.drain.<nodeId> on the final store), published by the node
 *     leader. Each rank on the node publishes its own drain progress to a
 *     node-local record in the staging directory; the leader folds all of
 *     them (minimum steps, summed bytes) into the node record, so a
 *     published node record guarantees that every rank's data on that node
 *     is already on the final store. The metadata commit is the visibility
 *     point, so a standard "bp5" reader (including streaming readers) never
 *     observes a step whose data is not yet on the final store.
 *   - Hand-off is an in-memory FIFO queue plus a small fixed-size drain
 *     progress file per writer in the output directory (bbstage.drain.<r>;
 *     one per node in aggregate mode, see above), letting the rank-0
 *     committer gate metadata commits; this replaces bbview's
 *     /tmp/bbview.sock + xattr protocol.
 *   - Unlike bbview's fire-and-forget flush, drain errors are recorded and
 *     rethrown to the application at the next EndStep()/Flush()/Close(), and
 *     commit waiting is subject to a configurable timeout.
 *
 * Output layout is standard BP5 (data.<rank> sub-files, one per writer, plus
 * md.0/md.idx/mmd.0 written by rank 0), so output is readable by stock ADIOS2
 * readers once steps are committed (all steps at Close()). With Aggregate=on
 * the data sub-files are data.<node> (one per node, co-written by every rank
 * on the node at pre-assigned, disjoint offsets) and the writer map routes
 * every rank to its node's sub-file; the output is still stock-BP5 readable
 * (the same layout BP5's TwoLevelShm aggregation produces).
 *
 * Reads: use engine "bp5" (this engine registers BP5Reader as its read side).
 * Direct reads of steps still sitting in the local-SSD burst buffer
 * (bbview's read-your-writes from staging) are a planned phase-2 extension;
 * the staging files are per-step and the metadata already records their
 * exact final positions, so the overlay is purely additive.
 *
 * Engine parameters (in addition to the common BP5 parameters shared with
 * engine "bp5": BurstBufferPath, BufferChunkSize, InitialBufferSize,
 * GrowthFactor, StripeSize, StatsLevel, UseOneTimeAttributes, verbose):
 *
 *   DrainThreads        number of drain worker threads per rank (default 1)
 *   StagingSync         fsync staging file in EndStep (default false,
 *                       page-cache semantics, like the original bbview)
 *   DrainDataSync       fsync final data file after every drain barrier
 *                       at Flush()/Close() (default false)
 *   CommitPollMs        committer poll interval for drain progress
 *                       (default 2 ms)
 *   CommitTimeoutSecs   give up gating a metadata commit after this long
 *                       waiting for slow/dead peers (default 300, 0 = wait
 *                       forever). In staged mode it is also the stall
 *                       timeout of the staging watermark backpressure:
 *                       EndStep() blocked at the watermark throws after
 *                       this long without drain progress instead of
 *                       hanging.
 *   DeleteStagedFiles   remove staging files once drained (default true)
 *   CloseMode           "durable" (default) or "staged". Staged: the drain
 *                       workers run concurrently from Open (exactly as in
 *                       durable mode), so the node-local burst buffer holds
 *                       only the in-flight drain backlog, not the whole
 *                       run; Close() hands the remaining session (leftover
 *                       staging files, data sub-file fd, metadata
 *                       transports, commit queue) to the process-level
 *                       finalizer (BbstageStagedFinalizer) and returns as
 *                       soon as the last step is written to the node-local
 *                       burst buffer, so the application goes on with
 *                       computation while the metadata commit and the drain
 *                       remainder run in the background. A clean process
 *                       exit finalizes every pending session before the
 *                       process ends; a crash loses the checkpoints that
 *                       were staged but not yet finalized. Finalization
 *                       publishes <finalDir>/bbstage.final, the
 *                       durability/visibility marker for readers and
 *                       restart policies.
 *   StagingWatermarkMB  staged mode: per-rank cap on staged-but-undrained
 *                       bytes; EndStep() blocks above it until the
 *                       concurrent drain catches up so the node-local
 *                       burst buffer cannot fill (default 1024, 0 =
 *                       unlimited). A blocked EndStep() that sees no drain
 *                       progress for CommitTimeoutSecs throws instead of
 *                       waiting forever. The cap is per rank in both
 *                       aggregation modes (aggregate co-write: every rank
 *                       drains its own bytes).
 *   Aggregate           "on"/"off" (default "off"). "on": node-aggregated
 *                       co-write drain. Local staging stays per-rank
 *                       (unchanged critical path), and the SSD ->
 *                       final-store drain writes a single data.<node>
 *                       sub-file per node that every rank on the node
 *                       co-writes: each rank drains only its own staging
 *                       slices, at the pre-assigned disjoint offsets of the
 *                       shared sub-file, so the node gets one store client
 *                       per rank instead of one for the whole node. The
 *                       number of data sub-files (and final-store drain
 *                       progress records: one per node, published by the
 *                       node leader from the node-local per-rank records)
 *                       scales with the node count instead of the rank
 *                       count, which matters for the metadata servers and
 *                       for readers on very large jobs. Off critical path:
 *                       the co-write runs in the existing background drain
 *                       workers. Must be identical on every rank: it drives
 *                       node collectives at Open/EndStep, and a mismatch
 *                       would deadlock or silently miscompute offsets; it
 *                       is validated collectively at Open and a mismatch is
 *                       reported as an error. StripeSize (when > 1) aligns
 *                       the node sub-file's step base only; member slices
 *                       within one step are contiguous.
 *
 * Environment variables (fallbacks for when the engine is reached through
 * middleware such as openPMD-api, which cannot pass engine parameters;
 * precedence: engine parameter > environment variable > default):
 *
 *   BBSTAGE_CLOSE_MODE            "durable" or "staged" (CloseMode)
 *   BBSTAGE_STAGING_WATERMARK_MB  watermark in MiB (StagingWatermarkMB)
 *   BBSTAGE_AGGREGATE             "on"/"off" (Aggregate)
 *   BBSTAGE_TIMING                "1" enables per-phase Open/Close timing
 *                                 lines on stderr (default: off)
 *   TMPDIR                        burst buffer root when the
 *                                 BurstBufferPath parameter is unset
 *                                 (default /tmp)
 */
class BbstageWriter : public BP5Engine, public core::Engine
{
public:
    BbstageWriter(IO &io, const std::string &name, const Mode mode, helper::Comm comm);
    ~BbstageWriter() override;

    StepStatus BeginStep(StepMode mode, const float timeoutSeconds = -1.0) final;
    size_t CurrentStep() const final;
    void PerformPuts() final;
    void EndStep() final;
    void Flush(const int transportIndex = -1) final;

    void NotifyEngineAttribute(std::string name, AttributeBase *Attr, void *data) noexcept final;

private:
    // drain protocol types shared with the staged-close finalizer
    using DrainJob = BbstageDrainJob;
    using DrainProgress = BbstageDrainProgress;

    // engine lifecycle
    void Init() final;
    void InitParameters() final;
    void InitTransports() final;
    void DoClose(const int transportIndex = -1) final;
    void DestructorClose(bool Verbose) noexcept final;

    // put
    void PutCommon(VariableBase &variable, const void *values, bool sync);
    template <class T>
    void PutCommonSpan(Variable<T> &variable, typename Variable<T>::Span &span,
                       const bool initialize, const T &value);
    void MarshalAttributes();

    // drain machinery (the daemonless stand-in for bbviewd)
    void StartDrainWorkers(); // drain workers only (staged mode: no committer)
    void StartDrainThreads(); // workers + rank-0 committer (durable mode)
    void StopDrainThreads() noexcept;
    void WorkerLoop();
    void CommitterLoop();
    void RunDataJob(DrainJob &job);
    void RunCommitJob(const DrainJob &job);
    void WaitForAllRanksDrained(size_t step);
    void WriteProgress(uint64_t drainedSteps, uint64_t drainedBytes);
    // aggregate co-write, called under m_Mutex:
    void WriteLocalProgress(uint64_t drainedSteps, uint64_t drainedBytes); // every rank
    void PublishNodeProgress(); // node leader only
    void CheckDrainError();
    void DrainAndCommitAll();

    // metadata
    void AggregateAndEnqueueCommit(format::BP5Serializer::TimestepInfo &TSInfo, uint64_t finalPos,
                                   size_t dataSize, size_t step);
    void MakeBbHeader(std::vector<char> &buffer, size_t &position, const std::string &fileType,
                      const bool isActive);
    void WriteIndexHeaderAndWriterMap();

    // Aggregate mode: node topology, resolved once at Open while MPI is alive
    // (per-step offsets and the per-node drain jobs are then derived locally,
    // so nothing after the Close hand-off needs MPI).
    void SetupNodeTopology();

    // bbstage-specific parameters
    unsigned int m_DrainThreads = 1;
    bool m_StagingSync = false;
    bool m_DrainDataSync = false;
    unsigned int m_CommitPollMs = 2;
    float m_CommitTimeoutSecs = 300.f;
    bool m_DeleteStagedFiles = true;
    bool m_CloseModeStaged = false;                 // CloseMode=staged
    uint64_t m_StagingWatermarkBytes = 1024ULL << 20; // StagingWatermarkMB (0 = unlimited)
    bool m_Aggregate = false;                        // Aggregate=on

    // Aggregate-mode node topology (valid when m_Aggregate is true)
    helper::Comm m_NodeComm;        // per-node communicator (split of m_Comm)
    int m_NodeIndex = 0;            // compact node id 0..NodeCount-1 (sub-file + progress id)
    int m_NodeLocalRank = 0;        // rank within the node, ordered by global rank
    int m_NodeSize = 1;             // ranks on this node
    int m_NodeCount = 1;            // total nodes (known on every rank)
    bool m_IsLeader = false;        // m_NodeLocalRank == 0: publishes the node
                                    // progress record (co-write: every rank
                                    // drains its own slices)
    std::vector<int> m_NodeMembers; // global ranks on this node, ordered by global rank
    std::vector<uint64_t> m_NodeIndexPerRank; // rank 0: node id per writer (WriterMap)

    format::BP5Serializer m_BP5Serializer;

    // final store files: one data sub-file per writer rank; metadata files on
    // rank 0 only
    std::string m_DataFileName;
    int m_DataFd = -1; // raw fd, written by drain threads only (pwrite)
    std::shared_ptr<Transport> m_MDFile;
    std::shared_ptr<Transport> m_MMDFile;
    std::shared_ptr<Transport> m_MDIndexFile;

    // node-local burst buffer (cf. the original bbview's BBVIEW_TMP_DIR)
    std::string m_BurstBufferPath;
    std::string m_StagingDir;
    // drain progress records (fixed-size seqlock, see BbstageDrainProgress).
    // Non-aggregate: m_ProgressPath is this rank's record on the final store
    // (finalDir/bbstage.drain.<rank>), published by this rank.
    // Aggregate co-write: m_ProgressPath is the ONE node record on the final
    // store (finalDir/bbstage.drain.<nodeId>), published by the node leader
    // only; every rank additionally publishes its node-local record
    // (m_LocalProgressPath, in the staging directory) which the leader folds
    // into the node record.
    std::string m_ProgressPath;
    int m_ProgressFd = -1; // raw fd to the (node) progress record
    uint64_t m_ProgressSeq = 0;
    std::string m_LocalProgressPath; // aggregate: node-local per-rank record
    int m_LocalProgressFd = -1;
    uint64_t m_LocalProgressSeq = 0;
    std::string m_Host;

    size_t m_WriterStep = 0;
    bool m_IsFirstStep = true;
    bool m_MarshalAttributesNecessary = true;
    uint64_t m_DataPos = 0;     // next append offset in the final data sub-file;
                                // aggregate: node append offset, advanced by the node
                                // leader only (non-leaders keep 0)
    uint64_t m_StagedBytes = 0; // bytes staged so far (progress accounting)
    uint64_t m_MetaDataPos = 0; // rank 0: append position in md.0

    // queues / threads
    std::deque<DrainJob> m_DataQueue;
    std::deque<DrainJob> m_CommitQueue;
    std::mutex m_Mutex;
    std::condition_variable m_DataCV;
    std::condition_variable m_CommitCV;
    bool m_Finishing = false;
    std::vector<std::thread> m_DrainWorkers;
    std::thread m_CommitterThread;
    size_t m_NextDataSeq = 0;      // number of data jobs enqueued
    size_t m_DrainedWatermark = 0; // number of leading data jobs completed
    uint64_t m_PublishedSteps = 0; // drain progress already written to the
                                   // (local) progress record
    uint64_t m_PublishedNodeSteps = 0; // aggregate leader: steps already
                                       // published in the node record
    std::set<size_t> m_DoneDataSeqs;
    size_t m_CommitEnqueued = 0;
    size_t m_CommitDone = 0;
    uint64_t m_ProgressBytes = 0; // bytes drained (progress record); summed in job
                                  // completion order, not the contiguous drained prefix
    std::exception_ptr m_DrainError;

#define declare_type(T)                                                                            \
    void DoPutSync(Variable<T> &variable, const T *data) final;                                    \
    void DoPutDeferred(Variable<T> &variable, const T *data) final;                                \
    size_t PutCount(Variable<T> &variable) final;

    ADIOS2_FOREACH_STDTYPE_1ARG(declare_type)
#undef declare_type

#define declare_type(T)                                                                            \
    void DoPut(Variable<T> &variable, typename Variable<T>::Span &span, const bool initialize,      \
               const T &value) final;

    ADIOS2_FOREACH_PRIMITIVE_STDTYPE_1ARG(declare_type)
#undef declare_type

#define declare_type(T, L)                                                                         \
    T *DoBufferData_##L(const int bufferIdx, const size_t payloadPosition,                         \
                        const size_t bufferID) noexcept final;

    ADIOS2_FOREACH_PRIMITVE_STDTYPE_2ARGS(declare_type)
#undef declare_type

    void DoPutStructSync(VariableStruct &variable, const void *data) final;
    void DoPutStructDeferred(VariableStruct &variable, const void *data) final;
};

} // end namespace engine
} // end namespace core
} // end namespace adios2

#endif /* ADIOS2_ENGINE_BBSTAGE_BBSTAGEWRITER_H_ */
