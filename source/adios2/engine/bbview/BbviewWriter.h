/*
 * SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ADIOS2_ENGINE_BBVIEW_BBVIEWWRITER_H_
#define ADIOS2_ENGINE_BBVIEW_BBVIEWWRITER_H_

#include "adios2/common/ADIOSConfig.h"
#include "adios2/core/Engine.h"
#include "adios2/engine/bbview/BbviewDrainCommon.h"
#include "adios2/engine/bbview/BbviewStagedFinalizer.h"
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

/** Engine "bbview": daemonless view-aware burst buffer, porting the ideas of
 * bbview (Koyama & Tatebe, "BBView: A View-Aware Burst-Buffer Mechanism for
 * MPI-IO", REX-IO 2025) from an Open MPI ompio component + bbviewd daemon to
 * a pure in-process ADIOS2 transport.
 *
 * The bbview idea: the application's payload is written to node-local SSD in
 * a cheap layout as fast as possible, while the expensive placement work and
 * the actual layout transformation are deferred to a background agent that
 * replays the data onto the final storage. In bbview this is the MPI file
 * view deferred from write time to staging-out time by bbviewd. In ADIOS2 the
 * same contract is implemented without any daemon:
 *
 *   - EndStep() serializes the step in memory (BP5Serializer), then writes
 *     the step payload synchronously to a per-rank staging file on
 *     node-local SSD (BurstBufferPath). This is the app-visible cost: a
 *     single sequential local write, like bbview's staging file.
 *   - One or more in-process worker threads (replacing bbviewd) drain the
 *     staging files to the final store at the pre-assigned data offsets.
 *   - The per-step metadata (the ADIOS2 analogue of the "view descriptor")
 *     is aggregated collectively at EndStep() time, but its *commit* to the
 *     BP5 metadata files (.bp md.0/md.idx/mmd.0) is performed by a
 *     rank-0 committer thread only once the data of that step has been
 *     drained by every writer rank. The metadata commit is the visibility
 *     point, so a standard "bp5" reader (including streaming readers) never
 *     observes a step whose data is not yet on the final store.
 *   - In-process hand-off replaces bbview's /tmp/bbview.sock + xattr
 *     protocol with an in-memory FIFO queue plus a small fixed-size drain
 *     progress file per writer in the output directory, letting the rank-0
 *     committer gate metadata commits.
 *   - Unlike bbview's fire-and-forget flush, drain errors are recorded and
 *     rethrown to the application at the next EndStep()/Flush()/Close(), and
 *     commit waiting is subject to a configurable timeout.
 *
 * Output layout is standard BP5 (data.<rank> sub-files, one per writer, plus
 * md.0/md.idx/mmd.0 written by rank 0), so output is readable by stock ADIOS2
 * readers once steps are committed (all steps at Close()).
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
 *                       mirrors bbview page-cache semantics)
 *   DrainDataSync       fsync final data file after every drain barrier
 *                       at Flush()/Close() (default false)
 *   CommitPollMs        committer poll interval for drain progress
 *                       (default 2 ms)
 *   CommitTimeoutSecs   give up gating a metadata commit after this long
 *                       waiting for slow/dead peers (default 300, 0 = wait
 *                       forever)
 *   DeleteStagedFiles   remove staging files once drained (default true)
 *   CloseMode           "durable" (default) or "staged". Staged: Close()
 *                       returns as soon as the last step is written to the
 *                       node-local burst buffer; the drain and the metadata
 *                       commit then run in the process background
 *                       (BbviewStagedFinalizer) while the application
 *                       computes. A clean process exit finalizes every
 *                       pending session before the process ends; a crash
 *                       loses the checkpoints that were staged but not yet
 *                       finalized. Finalization publishes
 *                       <finalDir>/bbview.final, the durability/visibility
 *                       marker for readers and restart policies.
 *   StagingWatermarkMB  staged mode: per-rank cap on staged-but-undrained
 *                       bytes; EndStep() blocks above it so the node-local
 *                       burst buffer cannot fill (default 1024, 0 =
 *                       unlimited)
 */
class BbviewWriter : public BP5Engine, public core::Engine
{
public:
    BbviewWriter(IO &io, const std::string &name, const Mode mode, helper::Comm comm);
    ~BbviewWriter() override;

    StepStatus BeginStep(StepMode mode, const float timeoutSeconds = -1.0) final;
    size_t CurrentStep() const final;
    void PerformPuts() final;
    void EndStep() final;
    void Flush(const int transportIndex = -1) final;

    void NotifyEngineAttribute(std::string name, AttributeBase *Attr, void *data) noexcept final;

private:
    // drain protocol types shared with the staged-close finalizer
    using DrainJob = BbviewDrainJob;
    using DrainProgress = BbviewDrainProgress;

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

    // drain machinery (the daemonless bbviewd)
    void StartDrainThreads();
    void StopDrainThreads() noexcept;
    void WorkerLoop();
    void CommitterLoop();
    void RunDataJob(DrainJob &job);
    void RunCommitJob(const DrainJob &job);
    void WaitForAllRanksDrained(size_t step);
    void WriteProgress(uint64_t drainedSteps, uint64_t drainedBytes);
    void CheckDrainError();
    void DrainAndCommitAll();

    // metadata
    void AggregateAndEnqueueCommit(format::BP5Serializer::TimestepInfo &TSInfo, uint64_t finalPos,
                                   size_t dataSize);
    void MakeBbHeader(std::vector<char> &buffer, size_t &position, const std::string &fileType,
                      const bool isActive);
    void WriteIndexHeaderAndWriterMap();

    // bbview-specific parameters
    unsigned int m_DrainThreads = 1;
    bool m_StagingSync = false;
    bool m_DrainDataSync = false;
    unsigned int m_CommitPollMs = 2;
    float m_CommitTimeoutSecs = 300.f;
    bool m_DeleteStagedFiles = true;
    bool m_CloseModeStaged = false;                 // CloseMode=staged
    uint64_t m_StagingWatermarkBytes = 1024ULL << 20; // StagingWatermarkMB (0 = unlimited)

    format::BP5Serializer m_BP5Serializer;

    // final store files: one data sub-file per writer rank; metadata files on
    // rank 0 only
    std::string m_DataFileName;
    int m_DataFd = -1; // raw fd, written by drain threads only (pwrite)
    std::shared_ptr<Transport> m_MDFile;
    std::shared_ptr<Transport> m_MMDFile;
    std::shared_ptr<Transport> m_MDIndexFile;

    // node-local burst buffer (bbview's BBVIEW_TMP_DIR)
    std::string m_BurstBufferPath;
    std::string m_StagingDir;
    std::string m_ProgressPath;
    int m_ProgressFd = -1; // raw fd to the fixed-size seqlock progress record
    uint64_t m_ProgressSeq = 0;
    std::string m_Host;

    size_t m_WriterStep = 0;
    bool m_IsFirstStep = true;
    bool m_MarshalAttributesNecessary = true;
    uint64_t m_DataPos = 0;     // next append offset in the final data sub-file
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
    uint64_t m_PublishedSteps = 0; // drain progress already written to the progress file
    std::set<size_t> m_DoneDataSeqs;
    size_t m_CommitEnqueued = 0;
    size_t m_CommitDone = 0;
    uint64_t m_ProgressBytes = 0; // bytes drained (progress record)
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

#endif /* ADIOS2_ENGINE_BBVIEW_BBVIEWWRITER_H_ */
