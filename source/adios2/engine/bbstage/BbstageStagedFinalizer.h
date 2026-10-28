/*
 * SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ADIOS2_ENGINE_BBSTAGE_BBSTAGESTAGEDFINALIZER_H_
#define ADIOS2_ENGINE_BBSTAGE_BBSTAGESTAGEDFINALIZER_H_

#include "adios2/engine/bbstage/BbstageDrainCommon.h"
#include "adios2/toolkit/transport/Transport.h"

#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace adios2
{
namespace core
{
namespace engine
{

/** Process-level finalizer for the bbstage "staged close" mode
 * (CloseMode=staged).
 *
 * In staged mode the application-visible checkpoint cost ends at the
 * node-local burst-buffer write: Close() hands the whole session (undrained
 * staging files, data sub-file fd, metadata transports, commit queue) to
 * this singleton and returns immediately, so the application goes on with
 * computation while the data is drained to the final store in the
 * background. One drain worker (every rank) and one committer worker (rank 0
 * only) process the sessions in hand-off order. No MPI is used after the
 * hand-off: all inter-rank coordination is through the drain progress files,
 * so the workers stay valid even after MPI_Finalize.
 *
 * Staged-mode durability contract:
 *  - a CLEAN process exit completes every pending session before the
 *    process ends: the singleton destructor (static destruction, i.e. the
 *    normal exit path) is the final barrier;
 *  - a CRASH before a session is finalized loses that checkpoint: the final
 *    directory holds partial data files, an incomplete md.idx, and no
 *    "bbstage.final" marker; a restart goes back to the last finalized
 *    checkpoint;
 *  - finalization (rank 0) writes the md.idx inactive flag, closes the
 *    metadata files, and atomically publishes <finalDir>/bbstage.final; the
 *    marker's presence is the visibility/durability point for readers and
 *    for restart policies.
 */
class BbstageStagedFinalizer
{
public:
    /** One checkpoint file handed over by a staged Close(). */
    struct Session
    {
        // identity
        std::string FinalDir;
        std::string DataFileName;
        std::string StagingDir;
        std::string ProgressPath;
        std::string Host;
        int Rank = 0;
        int WriterCount = 1;
        int Verbose = 0;

        // handed-over resources (ownership transfers on registration)
        int DataFd = -1;
        int ProgressFd = -1;
        uint64_t ProgressSeq = 0;
        std::shared_ptr<Transport> MDFile;
        std::shared_ptr<Transport> MMDFile;
        std::shared_ptr<Transport> MDIndexFile;

        // handed-over job state
        std::deque<BbstageDrainJob> DataQueue;
        std::deque<BbstageDrainJob> CommitQueue;
        size_t NextDataSeq = 0;
        size_t DrainedWatermark = 0;
        uint64_t PublishedSteps = 0;
        uint64_t ProgressBytes = 0;
        std::set<size_t> DoneDataSeqs;
        size_t CommitEnqueued = 0;
        size_t CommitDone = 0;

        // engine parameters
        bool DrainDataSync = false;
        bool DeleteStagedFiles = true;
        unsigned int CommitPollMs = 2;
        float CommitTimeoutSecs = 300.f;

        // lifecycle (worker threads only, under the finalizer mutex)
        bool DataDone = false;
        bool Finalized = false; // rank 0: md closed + bbstage.final published
    };

    static BbstageStagedFinalizer &Instance();

    // use Instance(); direct construction is only for the singleton itself
    BbstageStagedFinalizer() = default;

    /** Hand over a closed session; the workers start on the first call. */
    void RegisterSession(Session session);

    /** Rethrow the background drain/commit error, if any (no-op when the
     *  finalizer has no sessions or no error; safe to call always). */
    void CheckError();

    /** Staged-mode backpressure: block until `bytes` fit under the
     *  per-rank staging watermark (bytes >= watermark skips the wait), then
     *  account for them. Watermark 0 = unlimited. */
    void AccountStaged(uint64_t bytes);
    void SetStagingWatermark(uint64_t bytes);

    ~BbstageStagedFinalizer();
    BbstageStagedFinalizer(const BbstageStagedFinalizer &) = delete;
    BbstageStagedFinalizer &operator=(const BbstageStagedFinalizer &) = delete;

private:
    void DrainWorkerLoop();
    void CommitWorkerLoop();
    void RunDataJob(Session &s, BbstageDrainJob &job);
    void RunCommitJob(Session &s, const BbstageDrainJob &job);
    void WaitForSessionDrained(Session &s, size_t step);
    void FinalizeSession(Session &s);
    void PublishProgress(Session &s); // called under m_Mutex

    std::mutex m_Mutex;
    std::condition_variable m_DataCV;
    std::condition_variable m_CommitCV;
    std::condition_variable m_StagingCV;

    // shared_ptr: both workers hold (copies of) pointers to sessions across
    // unlocked sections while the other worker may erase deque elements; a
    // middle erase of a std::deque shifts the later elements, so raw
    // pointers into the deque would dangle
    std::deque<std::shared_ptr<Session>> m_Sessions;
    bool m_WorkersStarted = false;
    bool m_Finishing = false;
    int m_Rank = -1;
    std::thread m_DrainWorker;
    std::thread m_CommitWorker;

    uint64_t m_StagingWatermark = 0; // 0 = unlimited
    uint64_t m_PendingStaged = 0;    // staged bytes not yet drained (this rank)

    std::exception_ptr m_Error;
};

} // end namespace engine
} // end namespace core
} // end namespace adios2

#endif // ADIOS2_ENGINE_BBSTAGE_BBSTAGESTAGEDFINALIZER_H_
