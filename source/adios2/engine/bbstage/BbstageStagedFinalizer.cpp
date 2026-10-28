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
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
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

void BbstageStagedFinalizer::AccountStaged(uint64_t bytes)
{
    std::unique_lock<std::mutex> lck(m_Mutex);
    if (m_Error)
    {
        std::rethrow_exception(m_Error);
    }
    // A single staging write that alone exceeds the watermark cannot be
    // throttled (it would deadlock); let it through.
    if (m_StagingWatermark > 0 && bytes < m_StagingWatermark)
    {
        m_StagingCV.wait(lck, [this, bytes] {
            return m_Error != nullptr || m_PendingStaged <= m_StagingWatermark - bytes;
        });
        if (m_Error)
        {
            std::rethrow_exception(m_Error);
        }
    }
    m_PendingStaged += bytes;
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
    if (session.NextDataSeq == 0)
    {
        session.DataDone = true; // degenerate: no steps staged, nothing to drain
    }
    m_Sessions.push_back(std::make_shared<Session>(std::move(session)));
    m_DataCV.notify_all();
    m_CommitCV.notify_all();
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
    const int sfd = open(job.StagingPath.c_str(), O_RDONLY | O_CLOEXEC);
    if (sfd < 0)
    {
        ThrowPosix("open staging file for drain " + job.StagingPath);
    }
    std::vector<char> buf(DrainCopyBufferSize);
    size_t remain = job.DataSize;
    uint64_t inOff = 0;
    uint64_t outOff = job.FinalPos;
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
                                     job.StagingPath + " at " + std::to_string(inOff) + " (" +
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
        std::filesystem::remove(job.StagingPath, ec); // best effort
    }

    if (s.Verbose > 2)
    {
        std::cout << "bbstage finalizer rank " << s.Rank << " drained step " << job.Step
                  << " (" << job.DataSize << " bytes -> " << job.FinalPos << ") of " << s.FinalDir
                  << "\n";
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

void BbstageStagedFinalizer::DrainWorkerLoop()
{
    for (;;)
    {
        std::shared_ptr<Session> s;
        BbstageDrainJob job;
        bool have = false;
        {
            std::unique_lock<std::mutex> lck(m_Mutex);
            m_DataCV.wait(lck, [this] {
                return m_Error != nullptr || m_Finishing ||
                       std::any_of(m_Sessions.begin(), m_Sessions.end(),
                                   [](const std::shared_ptr<Session> &x) {
                                       return !x->DataQueue.empty();
                                   });
            });
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
            else if (m_Finishing)
            {
                return; // process exiting and nothing left to drain
            }
        }
        if (!have)
        {
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
                    PublishProgress(*s);
                    s->PublishedSteps = s->DrainedWatermark;
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
                if (s->DrainDataSync && s->DataFd >= 0 && fsync(s->DataFd) != 0)
                {
                    ThrowPosix("fsync of data sub-file " + s->DataFileName);
                }
                if (s->DataFd >= 0)
                {
                    close(s->DataFd);
                    s->DataFd = -1;
                }
                if (s->ProgressFd >= 0)
                {
                    close(s->ProgressFd);
                    s->ProgressFd = -1;
                }
                // NOTE: the progress FILE must survive until session
                // finalization: the rank-0 committer polls it (it may start
                // polling after this rank's drain already finished), and a
                // removed file is indistinguishable from "no progress".
                // FinalizeSession() removes it once the last commit is done.
                std::error_code ec;
                if (!s->StagingDir.empty())
                {
                    std::filesystem::remove(s->StagingDir, ec); // succeeds only if empty
                }
                if (s->Verbose > 1)
                {
                    std::cout << "bbstage finalizer rank " << s->Rank << ": all data of "
                              << s->FinalDir << " drained\n";
                }
                // release sessions fully done on this rank (rank != 0 has no
                // commit worker, so this worker is the only reclaimer there)
                {
                    std::lock_guard<std::mutex> lck(m_Mutex);
                    m_Sessions.erase(std::remove_if(m_Sessions.begin(), m_Sessions.end(),
                                                    [this](const std::shared_ptr<Session> &x) {
                                                        return x->DataDone &&
                                                               (m_Rank != 0 || x->Finalized);
                                                    }),
                                     m_Sessions.end());
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

    // remote ranks via their atomically-updated progress files (the committer
    // only exists on rank 0, so ranks 1..WriterCount-1 are the peers)
    for (int r = 1; r < s.WriterCount; ++r)
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
                                         "step " + std::to_string(step) + " on rank " +
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
    // needed by anyone (the committer only reads them while gating commits)
    std::error_code ec;
    for (int r = 0; r < s.WriterCount; ++r)
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
