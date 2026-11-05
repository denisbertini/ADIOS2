/*
 * SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "BbstageStagedFinalizer.h"

#include "adios2/engine/bp5/BP5Engine.h" // m_ActiveFlagPosition

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace adios2
{
namespace core
{
namespace engine
{

namespace
{
constexpr size_t DrainCopyBufferSize = 4 * 1024 * 1024; // cf. bbviewd's replay buffer

[[noreturn]] void ThrowPosix(const std::string &what)
{
    throw std::runtime_error("bbstage finalizer: " + what + ": " + std::strerror(errno));
}

void PwriteAll(int fd, const char *ptr, size_t size, uint64_t offset)
{
    size_t left = size;
    while (left > 0)
    {
        const ssize_t w = pwrite(fd, ptr, left, static_cast<off_t>(offset));
        if (w < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            ThrowPosix("pwrite staging->final failed");
        }
        ptr += w;
        offset += static_cast<uint64_t>(w);
        left -= static_cast<size_t>(w);
    }
}

BbstageStagedFinalizer &GetFinalizer()
{
    static BbstageStagedFinalizer instance;
    return instance;
}

} // end anonymous namespace

BbstageStagedFinalizer &BbstageStagedFinalizer::Instance() { return GetFinalizer(); }

void BbstageStagedFinalizer::SetStagingWatermark(uint64_t bytes)
{
    std::lock_guard<std::mutex> lck(m_Mutex);
    m_StagingWatermark = bytes;
}

void BbstageStagedFinalizer::SetStagingStallTimeout(float secs)
{
    std::lock_guard<std::mutex> lck(m_Mutex);
    m_StagingStallTimeoutSecs = secs;
}

void BbstageStagedFinalizer::AccountStaged(uint64_t bytes)
{
    const auto now = [] { return std::chrono::steady_clock::now(); };
    std::unique_lock<std::mutex> lck(m_Mutex);
    if (m_Error)
    {
        std::rethrow_exception(m_Error);
    }
    // A single staging write that alone exceeds the watermark cannot be
    // throttled (it would deadlock); let it through.
    if (m_StagingWatermark > 0 && bytes < m_StagingWatermark)
    {
        const bool forever = (m_StagingStallTimeoutSecs <= 0.f);
        // Poll-based wait: the drain workers run from Open and release this
        // wait as jobs complete (AccountDrained), so normally the CV notify
        // wakes us long before the poll interval. The periodic wake-up is
        // what makes the stall detection below possible.
        const auto stallTimeout = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<float>(m_StagingStallTimeoutSecs));
        uint64_t pendingMin = m_PendingStaged;
        auto lastProgress = now();
        for (;;)
        {
            if (m_Error != nullptr)
            {
                break;
            }
            if (m_PendingStaged <= m_StagingWatermark - bytes)
            {
                break; // the new bytes fit under the watermark
            }
            if (m_PendingStaged < pendingMin)
            {
                pendingMin = m_PendingStaged;
                lastProgress = now();
            }
            else if (!forever && (now() - lastProgress) > stallTimeout)
            {
                // The burst buffer is full at the watermark and the drain
                // has made no progress for the whole stall timeout: waiting
                // longer will not help. Report it instead of hanging.
                std::ostringstream err;
                err << "bbstage staged close: staging watermark of "
                    << m_StagingWatermark << " bytes reached with " << m_PendingStaged
                    << " bytes still pending and no drain progress for "
                    << m_StagingStallTimeoutSecs << " s; the drain to the final "
                    "store is stalled (slow or unreachable final store, or "
                    "a burst buffer that the run cannot keep below the "
                    "watermark)";
                throw std::runtime_error(err.str());
            }
            m_StagingCV.wait_for(lck, std::chrono::milliseconds(100));
        }
        if (m_Error)
        {
            std::rethrow_exception(m_Error);
        }
    }
    m_PendingStaged += bytes;
}

void BbstageStagedFinalizer::AccountDrained(uint64_t bytes)
{
    {
        std::lock_guard<std::mutex> lck(m_Mutex);
        // clamp: a job's bytes may have been accounted under a different
        // watermark configuration (e.g. watermark set mid-run), so never
        // underflow
        m_PendingStaged = (m_PendingStaged > bytes) ? m_PendingStaged - bytes : 0;
    }
    m_StagingCV.notify_all();
}

void BbstageStagedFinalizer::CheckError()
{
    std::exception_ptr err;
    {
        std::lock_guard<std::mutex> lck(m_Mutex);
        err = m_Error;
    }
    if (err)
    {
        std::rethrow_exception(err);
    }
}

void BbstageStagedFinalizer::RegisterSession(Session session)
{
    std::lock_guard<std::mutex> lck(m_Mutex);
    if (!m_WorkersStarted)
    {
        m_Rank = session.Rank;
        m_DrainWorker = std::thread(&BbstageStagedFinalizer::DrainWorkerLoop, this);
        if (m_Rank == 0)
        {
            m_CommitWorker = std::thread(&BbstageStagedFinalizer::CommitWorkerLoop, this);
        }
        m_WorkersStarted = true;
    }
    // The concurrent drain (staged mode) may have fully drained the session
    // before the hand-off: no job is left for the drain worker to complete,
    // so DataDone would never be set and the session could never be
    // finalized. Detect the already-drained state here; the degenerate
    // zero-step session is a special case of it.
    if (session.DrainedWatermark >= session.NextDataSeq && session.DataQueue.empty())
    {
        session.DataDone = true;
    }
    auto sp = std::make_shared<Session>(std::move(session));
    m_Sessions.push_back(sp);
    if (sp->DataDone)
    {
        // The workers only run the data-side cleanup (fsync, close fds,
        // remove the staging dir) when the session's last job completes; a
        // fully drained session has no such job, so run it here. The drain
        // worker never touches this session (its queue is empty), so this
        // caller is the single actor for its resources.
        try
        {
            CompleteSessionData(*sp);
        }
        catch (...)
        {
            if (!m_Error)
            {
                m_Error = std::current_exception();
            }
            m_DataCV.notify_all();
            m_CommitCV.notify_all();
            m_StagingCV.notify_all();
        }
    }
    m_DataCV.notify_all();
    m_CommitCV.notify_all();
}

void BbstageStagedFinalizer::CompleteSessionData(Session &s)
{
    // Data-side cleanup of a fully drained session. Called outside
    // m_Mutex by a single actor (the drain worker after the last job, or
    // RegisterSession for a session handed over already drained).
    if (s.DrainDataSync && s.DataFd >= 0 && fsync(s.DataFd) != 0)
    {
        ThrowPosix("fsync of data sub-file " + s.DataFileName);
    }
    if (s.DataFd >= 0)
    {
        close(s.DataFd);
        s.DataFd = -1;
    }
    if (s.ProgressFd >= 0)
    {
        close(s.ProgressFd);
        s.ProgressFd = -1;
    }
    // NOTE: the progress FILE must survive until session finalization: the
    // rank-0 committer polls it (it may start polling after this rank's
    // drain already finished), and a removed file is indistinguishable from
    // "no progress". FinalizeSession() removes it once the last commit is
    // done. Aggregate co-write: the node-local records live until the node
    // leader removes them at node-full (see PublishNodeRecord); this rank
    // only closes its own fd.
    if (s.LocalProgressFd >= 0)
    {
        close(s.LocalProgressFd);
        s.LocalProgressFd = -1;
    }
    std::error_code ec;
    if (!s.StagingDir.empty())
    {
        // Aggregate co-write: this can fail while the node's local records
        // still exist (the leader removes them, with the dir, at node-full);
        // best effort.
        std::filesystem::remove(s.StagingDir, ec); // succeeds only if empty
    }
    if (s.Verbose > 1)
    {
        std::cout << "bbstage finalizer rank " << s.Rank << ": all data of "
                  << s.FinalDir << " drained\n";
    }
}

BbstageStagedFinalizer::~BbstageStagedFinalizer()
{
    // Exit barrier: on a clean process exit, complete every pending session
    // before the process ends. The workers never use MPI (all coordination
    // is through the drain progress files), so this is safe even after
    // MPI_Finalize.
    {
        std::lock_guard<std::mutex> lck(m_Mutex);
        m_Finishing = true;
    }
    m_DataCV.notify_all();
    m_CommitCV.notify_all();
    m_StagingCV.notify_all();
    if (m_DrainWorker.joinable())
    {
        m_DrainWorker.join();
    }
    if (m_CommitWorker.joinable())
    {
        m_CommitWorker.join();
    }
    if (m_Error)
    {
        try
        {
            std::rethrow_exception(m_Error);
        }
        catch (const std::exception &e)
        {
            std::fprintf(stderr,
                         "bbstage finalizer: background finalization failed; one or more "
                         "checkpoints may be incomplete: %s\n",
                         e.what());
        }
        catch (...)
        {
            std::fprintf(stderr,
                         "bbstage finalizer: background finalization failed; one or more "
                         "checkpoints may be incomplete\n");
        }
    }
}

void BbstageStagedFinalizer::RunDataJob(Session &s, BbstageDrainJob &job)
{
    // Copy this rank's staging file into the data sub-file at its final
    // offset. Aggregate co-write: the offset is this rank's pre-assigned
    // range in the shared node sub-file; no two ranks write the same bytes.
    auto drainOne = [&](const std::string &stagingPath, uint64_t finalPos, size_t dataSize) {
        const int sfd = open(stagingPath.c_str(), O_RDONLY | O_CLOEXEC);
        if (sfd < 0)
        {
            ThrowPosix("open staging file for drain " + stagingPath);
        }
        // Verify the staging file size before draining (mirrors the engine's
        // check): fail fast with both sizes instead of a short read mid-copy.
        struct stat st;
        if (fstat(sfd, &st) != 0)
        {
            close(sfd);
            ThrowPosix("fstat staging file for drain " + stagingPath);
        }
        if (static_cast<size_t>(st.st_size) != dataSize)
        {
            close(sfd);
            throw std::runtime_error("bbstage finalizer: staging file " + stagingPath +
                                     " has size " + std::to_string(st.st_size) + ", expected " +
                                     std::to_string(dataSize) +
                                     " (staging file truncated or corrupted)");
        }
        std::vector<char> buf(DrainCopyBufferSize);
        size_t remain = dataSize;
        uint64_t inOff = 0;
        uint64_t outOff = finalPos;
        while (remain > 0)
        {
            const size_t want = std::min(remain, buf.size());
            ssize_t n = 0;
            do
            {
                n = pread(sfd, buf.data(), want, static_cast<off_t>(inOff));
            } while (n < 0 && errno == EINTR);
            if (n <= 0)
            {
                close(sfd);
                throw std::runtime_error("bbstage finalizer: short read of staging file " +
                                         stagingPath + " at " + std::to_string(inOff) + " (" +
                                         std::to_string(remain) + " bytes still due)");
            }
            PwriteAll(s.DataFd, buf.data(), static_cast<size_t>(n), outOff);
            inOff += static_cast<uint64_t>(n);
            outOff += static_cast<uint64_t>(n);
            remain -= static_cast<size_t>(n);
        }
        close(sfd);

        if (s.DeleteStagedFiles)
        {
            std::error_code ec;
            std::filesystem::remove(stagingPath, ec); // best effort
        }
    };

    drainOne(job.StagingPath, job.FinalPos, job.DataSize);

    if (s.Verbose > 2)
    {
        std::cout << "bbstage finalizer rank " << s.Rank << " drained step " << job.Step
                  << " (" << job.DataSize << " bytes) of " << s.FinalDir << "\n";
    }
}

void BbstageStagedFinalizer::PublishProgress(Session &s)
{
    // Called under m_Mutex by the single drain worker, so no two threads can
    // race the lazy create or the sequence counter.
    if (s.ProgressFd < 0)
    {
        s.ProgressFd =
            open(s.ProgressPath.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (s.ProgressFd < 0)
        {
            ThrowPosix("create progress file " + s.ProgressPath + " (rank " +
                        std::to_string(s.Rank) + ", host " + s.Host + ")");
        }
    }
    BbstageWriteProgressRecord(s.ProgressFd, s.ProgressPath, static_cast<uint64_t>(s.Rank),
                              s.DrainedWatermark, s.ProgressBytes, s.ProgressSeq, s.Host);
}

void BbstageStagedFinalizer::PublishLocalProgress(Session &s)
{
    // Aggregate co-write: this rank's node-local drain record (in the
    // staging directory, on the burst-buffer medium). The node leader folds
    // it into the node record; nobody on the final store reads it.
    // Called under m_Mutex.
    if (s.LocalProgressFd < 0)
    {
        s.LocalProgressFd = open(s.LocalProgressPath.c_str(), O_RDWR | O_CREAT | O_TRUNC |
                                         O_CLOEXEC,
                                 0644);
        if (s.LocalProgressFd < 0)
        {
            ThrowPosix("create local progress file " + s.LocalProgressPath + " (rank " +
                       std::to_string(s.Rank) + ", host " + s.Host + ")");
        }
    }
    BbstageWriteProgressRecord(s.LocalProgressFd, s.LocalProgressPath,
                               static_cast<uint64_t>(s.Rank), s.DrainedWatermark, s.ProgressBytes,
                               s.LocalProgressSeq, s.Host);
}

void BbstageStagedFinalizer::PublishNodeRecord(Session &s)
{
    // Aggregate co-write: fold every rank's node-local progress record into
    // the one node record on the final store. The node record reports the
    // node's drained minimum (steps) and summed drained bytes; a record
    // reporting N steps therefore guarantees that all N steps of every rank
    // on this node are on the final store -- the invariant the rank-0
    // committer gates on.
    // Called under m_Mutex; only the node leader does this.
    uint64_t minSteps = s.DrainedWatermark; // own watermark (in memory)
    uint64_t sumBytes = s.ProgressBytes;
    for (const int member : s.NodeMembers)
    {
        if (member == s.Rank)
        {
            continue;
        }
        BbstageDrainProgress p;
        if (BbstageReadProgress(s.StagingDir + "/bbstage.drain." + std::to_string(member), p))
        {
            minSteps = std::min(minSteps, p.DrainedSteps);
            sumBytes += p.DrainedBytes;
        }
        else
        {
            minSteps = 0; // sibling has not drained anything yet (or is dead)
        }
    }
    if (minSteps <= s.NodePublishedSteps)
    {
        return;
    }
    if (s.ProgressFd < 0)
    {
        // No O_TRUNC: the fd may have been closed by CompleteSessionData
        // while the node record still has content, and a record is always a
        // full 56-byte write at offset 0 anyway.
        s.ProgressFd = open(s.ProgressPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (s.ProgressFd < 0)
        {
            ThrowPosix("create node progress file " + s.ProgressPath + " (rank " +
                       std::to_string(s.Rank) + ", host " + s.Host + ")");
        }
    }
    BbstageWriteProgressRecord(s.ProgressFd, s.ProgressPath, static_cast<uint64_t>(s.Rank),
                               minSteps, sumBytes, s.ProgressSeq, s.Host);
    s.NodePublishedSteps = minSteps;
    if (minSteps >= s.NextDataSeq)
    {
        // Node fully drained: the node-local records have served their
        // purpose. The leader is the only actor that may remove them -- a
        // sibling removing its own record before the node is full would read
        // back as "0 steps" and stall the node record forever -- and it
        // removes them here, in the same critical section as the final node
        // record publish. The staging dir goes with them when it is empty
        // (all staging files were removed by their owners as they drained).
        std::error_code ec;
        for (const int member : s.NodeMembers)
        {
            std::filesystem::remove(s.StagingDir + "/bbstage.drain." + std::to_string(member), ec);
        }
        std::filesystem::remove(s.StagingDir, ec); // best effort: succeeds only if empty
    }
}

void BbstageStagedFinalizer::EraseDoneSessions()
{
    // Called under m_Mutex. An aggregate node leader session is kept while
    // its node record is still behind (NodePublishedSteps < NextDataSeq):
    // the leader must keep folding and publishing until the whole node has
    // drained, even after its own queue is empty.
    m_Sessions.erase(std::remove_if(m_Sessions.begin(), m_Sessions.end(),
                                    [this](const std::shared_ptr<Session> &x) {
                                        return x->DataDone && (m_Rank != 0 || x->Finalized) &&
                                               !(x->Aggregate && x->IsNodeLeader &&
                                                 x->NodePublishedSteps < x->NextDataSeq);
                                    }),
                     m_Sessions.end());
}

void BbstageStagedFinalizer::DrainWorkerLoop()
{
    // stall tracking for the aggregate node leader's node record publish
    // (see the idle path below)
    std::shared_ptr<Session> stallSession;
    uint64_t stallSteps = 0;
    std::chrono::steady_clock::time_point stallStart = std::chrono::steady_clock::now();
    for (;;)
    {
        std::shared_ptr<Session> s;
        BbstageDrainJob job;
        bool have = false;
        {
            std::unique_lock<std::mutex> lck(m_Mutex);
            const bool nodePublishPending =
                std::any_of(m_Sessions.begin(), m_Sessions.end(),
                            [](const std::shared_ptr<Session> &x) {
                                return x->Aggregate && x->IsNodeLeader &&
                                       x->NodePublishedSteps < x->NextDataSeq;
                            });
            if (nodePublishPending)
            {
                // Sibling ranks' drain completions never notify this worker
                // (they only write their node-local records), so an aggregate
                // node leader must keep publishing the node record on a timer
                // until the whole node has drained, even with an empty queue.
                m_DataCV.wait_for(lck, std::chrono::milliseconds(2),
                                  [this, &nodePublishPending] {
                                      return m_Error != nullptr || m_Finishing ||
                                             std::any_of(m_Sessions.begin(), m_Sessions.end(),
                                                         [](const std::shared_ptr<Session> &x) {
                                                             return !x->DataQueue.empty() ||
                                                                    (x->Aggregate && x->IsNodeLeader &&
                                                                     x->NodePublishedSteps <
                                                                         x->NextDataSeq);
                                                         });
                                  });
            }
            else
            {
                m_DataCV.wait(lck, [this] {
                    return m_Error != nullptr || m_Finishing ||
                           std::any_of(m_Sessions.begin(), m_Sessions.end(),
                                       [](const std::shared_ptr<Session> &x) {
                                           return !x->DataQueue.empty();
                                       });
                });
            }
            if (m_Error)
            {
                return;
            }
            for (const auto &x : m_Sessions)
            {
                if (!x->DataDone && !x->DataQueue.empty())
                {
                    s = x;
                    break;
                }
            }
            if (s)
            {
                job = std::move(s->DataQueue.front());
                s->DataQueue.pop_front();
                have = true;
            }
            else
            {
                // No job to drain. An aggregate node leader whose node record
                // is behind still has work: fold the node-local records and
                // publish (self-gating: no-op when the minimum has not
                // advanced).
                bool published = false;
                for (const auto &x : m_Sessions)
                {
                    if (x->Aggregate && x->IsNodeLeader && x->NodePublishedSteps < x->NextDataSeq)
                    {
                        try
                        {
                            PublishNodeRecord(*x);
                            // Stall detection: a sibling that dies stops its
                            // node-local record from advancing, and the node
                            // minimum can never reach the session's last
                            // step. Fail with an error after
                            // CommitTimeoutSecs without progress instead of
                            // polling forever (at process exit the
                            // destructor would otherwise block on this
                            // worker).
                            if (stallSession != x)
                            {
                                stallSession = x;
                                stallSteps = x->NodePublishedSteps;
                                stallStart = std::chrono::steady_clock::now();
                            }
                            else if (x->NodePublishedSteps != stallSteps)
                            {
                                stallSteps = x->NodePublishedSteps;
                                stallStart = std::chrono::steady_clock::now();
                            }
                            else if (x->CommitTimeoutSecs > 0.f &&
                                     (std::chrono::steady_clock::now() - stallStart) >
                                         std::chrono::duration_cast<
                                             std::chrono::steady_clock::duration>(
                                             std::chrono::duration<float>(x->CommitTimeoutSecs)))
                            {
                                throw std::runtime_error(
                                    "bbstage finalizer: node progress of " + x->FinalDir +
                                    " stuck at " + std::to_string(x->NodePublishedSteps) +
                                    " of " + std::to_string(x->NextDataSeq) + " steps for " +
                                    std::to_string(x->CommitTimeoutSecs) +
                                    " s: a sibling rank on this node is not draining "
                                    "(dead or stalled)");
                            }
                        }
                        catch (...)
                        {
                            if (!m_Error)
                            {
                                m_Error = std::current_exception();
                            }
                        }
                        published = true;
                    }
                }
                if (published)
                {
                    EraseDoneSessions(); // a just-completed node may release its session
                }
                if (m_Finishing && !published)
                {
                    return; // process exiting and nothing left to drain or publish
                }
            }
        }
        if (!have)
        {
            m_DataCV.notify_all();
            m_CommitCV.notify_all();
            continue;
        }

        try
        {
            RunDataJob(*s, job);
        }
        catch (...)
        {
            {
                std::lock_guard<std::mutex> lck(m_Mutex);
                if (!m_Error)
                {
                    m_Error = std::current_exception();
                }
            }
            m_DataCV.notify_all();
            m_CommitCV.notify_all();
            m_StagingCV.notify_all();
            return;
        }

        bool justDone = false;
        try
        {
            {
                std::lock_guard<std::mutex> lck(m_Mutex);
                s->DoneDataSeqs.insert(job.Seq);
                while (s->DoneDataSeqs.count(s->DrainedWatermark) != 0)
                {
                    s->DoneDataSeqs.erase(s->DrainedWatermark);
                    ++s->DrainedWatermark;
                }
                s->ProgressBytes += job.DataSize;
                if (s->DrainedWatermark > s->PublishedSteps)
                {
                    // Aggregate co-write: this rank's record is node-local
                    // (the node leader folds it into the node record);
                    // otherwise the record goes straight to the final store.
                    if (s->Aggregate)
                    {
                        PublishLocalProgress(*s);
                    }
                    else
                    {
                        PublishProgress(*s);
                    }
                    s->PublishedSteps = s->DrainedWatermark;
                }
                if (s->Aggregate && s->IsNodeLeader)
                {
                    // Fold the node's local records into the node record
                    // (no-op when the node's drained minimum has not
                    // advanced).
                    PublishNodeRecord(*s);
                }
                if (m_PendingStaged >= job.DataSize)
                {
                    m_PendingStaged -= job.DataSize;
                }
                else
                {
                    m_PendingStaged = 0;
                }
                if (!s->DataDone && s->DrainedWatermark >= s->NextDataSeq &&
                    s->DataQueue.empty())
                {
                    s->DataDone = true;
                    justDone = true;
                }
            }
            m_StagingCV.notify_all();
            if (justDone)
            {
                // outside the lock: data-side session cleanup (single actor)
                CompleteSessionData(*s);
                // release sessions fully done on this rank (rank != 0 has no
                // commit worker, so this worker is the only reclaimer there);
                // an aggregate node leader session is kept while its node
                // record is still behind (it must keep publishing until the
                // whole node has drained)
                {
                    std::lock_guard<std::mutex> lck(m_Mutex);
                    EraseDoneSessions();
                }
            }
        }
        catch (...)
        {
            {
                std::lock_guard<std::mutex> lck(m_Mutex);
                if (!m_Error)
                {
                    m_Error = std::current_exception();
                }
            }
            m_DataCV.notify_all();
            m_CommitCV.notify_all();
            m_StagingCV.notify_all();
            return;
        }
        m_DataCV.notify_all();
        m_CommitCV.notify_all();
    }
}

void BbstageStagedFinalizer::RunCommitJob(Session &s, const BbstageDrainJob &job)
{
    if (!job.MMDBytes.empty())
    {
        s.MMDFile->Write(job.MMDBytes.data(), job.MMDBytes.size());
        s.MMDFile->Flush();
    }
    s.MDFile->Write(job.MDRecord.data(), job.MDRecord.size());
    s.MDFile->Flush();
    s.MDIndexFile->Write(job.IdxRecord.data(), job.IdxRecord.size());
    s.MDIndexFile->Flush();
    // The md.idx record is the per-step visibility marker: readers only ever
    // see steps whose data is already on the final store.

    if (s.Verbose > 2)
    {
        std::cout << "bbstage finalizer committed step " << job.Step << " metadata of "
                  << s.FinalDir << "\n";
    }
}

void BbstageStagedFinalizer::WaitForSessionDrained(Session &s, size_t step)
{
    const auto start = std::chrono::steady_clock::now();
    const bool forever = (s.CommitTimeoutSecs <= 0.f);
    const auto timeout = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<float>(s.CommitTimeoutSecs));
    const auto poll = std::chrono::milliseconds(s.CommitPollMs);

    // own drain state first (in-memory)
    for (;;)
    {
        {
            std::lock_guard<std::mutex> lck(m_Mutex);
            if (m_Error)
            {
                std::rethrow_exception(m_Error);
            }
            if (s.DrainedWatermark >= step + 1)
            {
                break;
            }
        }
        if (!forever && (std::chrono::steady_clock::now() - start) > timeout)
        {
            throw std::runtime_error("bbstage finalizer: timeout waiting for own drain of "
                                     "step " + std::to_string(step) + " of " + s.FinalDir);
        }
        std::this_thread::sleep_for(poll);
    }

    // remote peers via their atomically-updated progress files (the committer
    // only exists on rank 0, so ranks 1..WriterCount-1 are the peers).
    // Aggregate co-write mode: one node record per node, published by each
    // node's leader from the node-local per-rank records; gate on ALL nodes,
    // including rank 0's own -- in co-write mode the in-memory check above
    // covers only rank 0's own slices, not its node's.
    const int peers = s.Aggregate ? s.NodeCount : s.WriterCount;
    for (int r = (s.Aggregate ? 0 : 1); r < peers; ++r)
    {
        const std::string path = s.FinalDir + "/bbstage.drain." + std::to_string(r);
        for (;;)
        {
            {
                std::lock_guard<std::mutex> lck(m_Mutex);
                if (m_Error)
                {
                    std::rethrow_exception(m_Error);
                }
            }
            BbstageDrainProgress p;
            if (BbstageReadProgress(path, p) && p.DrainedSteps >= step + 1)
            {
                break;
            }
            if (!forever && (std::chrono::steady_clock::now() - start) > timeout)
            {
                throw std::runtime_error("bbstage finalizer: timeout waiting for drain of "
                                         "step " + std::to_string(step) +
                                         (s.Aggregate ? " on node " : " on rank ") +
                                         std::to_string(r) + " (progress: " +
                                         (BbstageReadProgress(path, p)
                                              ? std::to_string(p.DrainedSteps) + " steps"
                                              : std::string("no progress file")) +
                                         ")");
            }
            std::this_thread::sleep_for(poll);
        }
    }
}

void BbstageStagedFinalizer::FinalizeSession(Session &s)
{
    // final active flag = 0 in md.idx header (same as the durable path)
    const char inactive = 0;
    s.MDIndexFile->Write(&inactive, 1, BP5Engine::m_ActiveFlagPosition);
    s.MDIndexFile->Flush();
    if (s.MDFile)
    {
        s.MDFile->Close();
    }
    if (s.MMDFile)
    {
        s.MMDFile->Close();
    }
    if (s.MDIndexFile)
    {
        s.MDIndexFile->Close();
    }
    s.MDFile = nullptr;
    s.MMDFile = nullptr;
    s.MDIndexFile = nullptr;

    // all commits of this session are done: the progress files are no longer
    // needed by anyone (the committer only reads them while gating commits).
    // Aggregate mode: one per node; otherwise one per writer rank.
    std::error_code ec;
    const int nProgress = s.Aggregate ? s.NodeCount : s.WriterCount;
    for (int r = 0; r < nProgress; ++r)
    {
        std::filesystem::remove(s.FinalDir + "/bbstage.drain." + std::to_string(r), ec);
    }

    // publish the durability/visibility marker: tmp + rename with retry,
    // since on network final stores a rename of a just-created tmp file can
    // transiently fail with ENOENT due to stale lookups
    const std::string tmp = s.FinalDir + "/bbstage.final.tmp";
    const std::string fin = s.FinalDir + "/bbstage.final";
    const std::string content = "bbstage finalized: " + s.FinalDir + "\n";
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        {
            const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd >= 0)
            {
                ssize_t off = 0;
                bool failed = false;
                while (off < static_cast<ssize_t>(content.size()))
                {
                    const ssize_t w = write(fd, content.data() + off,
                                            content.size() - static_cast<size_t>(off));
                    if (w < 0)
                    {
                        if (errno == EINTR)
                        {
                            continue;
                        }
                        failed = true;
                        break;
                    }
                    off += w;
                }
                close(fd);
                if (failed)
                {
                    break;
                }
            }
        }
        if (rename(tmp.c_str(), fin.c_str()) == 0)
        {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            if (s.Verbose > 1)
            {
                std::cout << "bbstage finalizer: session finalized " << s.FinalDir << "\n";
            }
            return;
        }
        if (attempt == 49)
        {
            throw std::runtime_error("bbstage finalizer: cannot publish marker " + fin + ": " +
                                     std::strerror(errno));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw std::runtime_error("bbstage finalizer: cannot write marker " + tmp + ": " +
                             std::strerror(errno));
}

void BbstageStagedFinalizer::CommitWorkerLoop()
{
    for (;;)
    {
        std::shared_ptr<Session> s;
        BbstageDrainJob job;
        bool have = false;
        std::vector<std::shared_ptr<Session>> toFinalize;
        {
            std::unique_lock<std::mutex> lck(m_Mutex);
            m_CommitCV.wait_for(lck, std::chrono::milliseconds(5), [this] {
                return m_Error != nullptr || m_Finishing ||
                       std::any_of(m_Sessions.begin(), m_Sessions.end(),
                                   [](const std::shared_ptr<Session> &x) {
                                       return !x->CommitQueue.empty() ||
                                              (!x->Finalized && x->DataDone &&
                                               x->CommitDone >= x->CommitEnqueued);
                                   });
            });
            if (m_Error)
            {
                return;
            }
            for (const auto &x : m_Sessions)
            {
                if (!x->Finalized && !x->CommitQueue.empty())
                {
                    s = x;
                    break;
                }
            }
            if (s)
            {
                job = std::move(s->CommitQueue.front());
                s->CommitQueue.pop_front();
                have = true;
            }
            for (const auto &x : m_Sessions)
            {
                if (!x->Finalized && x->DataDone && x->CommitQueue.empty() &&
                    x->CommitDone >= x->CommitEnqueued)
                {
                    toFinalize.push_back(x);
                }
            }
            if (m_Finishing && !have && toFinalize.empty() && m_Sessions.empty())
            {
                return; // process exiting and nothing left to commit or finalize
            }
        }

        if (have)
        {
            try
            {
                WaitForSessionDrained(*s, job.Step);
                RunCommitJob(*s, job);
                {
                    std::lock_guard<std::mutex> lck(m_Mutex);
                    ++s->CommitDone;
                }
                m_CommitCV.notify_all();
            }
            catch (...)
            {
                {
                    std::lock_guard<std::mutex> lck(m_Mutex);
                    if (!m_Error)
                    {
                        m_Error = std::current_exception();
                    }
                }
                m_DataCV.notify_all();
                m_CommitCV.notify_all();
                m_StagingCV.notify_all();
                return;
            }
        }

        for (const std::shared_ptr<Session> &f : toFinalize)
        {
            try
            {
                FinalizeSession(*f);
                std::lock_guard<std::mutex> lck(m_Mutex);
                f->Finalized = true;
            }
            catch (...)
            {
                {
                    std::lock_guard<std::mutex> lck(m_Mutex);
                    if (!m_Error)
                    {
                        m_Error = std::current_exception();
                    }
                }
                m_DataCV.notify_all();
                m_CommitCV.notify_all();
                m_StagingCV.notify_all();
                return;
            }
        }

        // release fully completed sessions
        {
            std::lock_guard<std::mutex> lck(m_Mutex);
            m_Sessions.erase(std::remove_if(m_Sessions.begin(), m_Sessions.end(),
                                            [](const std::shared_ptr<Session> &x) {
                                                return x->DataDone && x->Finalized;
                                            }),
                             m_Sessions.end());
        }
    }
}

} // end namespace engine
} // end namespace core
} // end namespace adios2
