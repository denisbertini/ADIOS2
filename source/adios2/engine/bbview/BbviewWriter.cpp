/*
 * SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "BbviewWriter.h"

#include "adios2/common/ADIOSMacros.h"
#include "adios2/core/IO.h"
#include "adios2/helper/adiosFunctions.h"
#include "adios2/helper/adiosMath.h"    // IsLittleEndian
#include "adios2/helper/adiosMemory.h"  // CopyToBuffer, PaddingToAlignOffset
#include "adios2/helper/adiosString.h" // GetParameter
#include "adios2/helper/adiosSystem.h" // CleanupBPDirectory, GetEnv
#include "adios2/toolkit/format/bp5/BP5Helper.h"
#include "adios2/toolkit/format/buffer/chunk/ChunkV.h"
#include "adios2/toolkit/format/buffer/malloc/MallocV.h"
#include "adios2/toolkit/transport/OpenFile.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>

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
constexpr size_t DrainCopyBufferSize = 4 * 1024 * 1024; // like bbviewd's replay buffer

// bbview flattened "path-<rank>-<view_index>" staging names, hardened: cap the
// length and mix in a hash to avoid PATH_MAX issues and collisions.
std::string SanitizeForPath(const std::string &name)
{
    std::string out;
    out.reserve(name.size());
    for (const char c : name)
    {
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')
                   ? c
                   : '-';
    }
    if (out.size() > 96)
    {
        std::ostringstream oss;
        oss << std::hex << std::hash<std::string>{}(name);
        out = out.substr(0, 64) + "-" + oss.str();
    }
    return out;
}

[[noreturn]] void ThrowPosix(const std::string &what)
{
    helper::Throw<std::runtime_error>("Engine", "BbviewWriter", what,
                                      what + ": " + std::string(std::strerror(errno)));
    std::abort(); // helper::Throw always throws; keep the noreturn contract explicit
}

void PwriteAll(int fd, const std::vector<core::iovec> &iovs, uint64_t offset)
{
    for (const auto &iov : iovs)
    {
        const char *ptr = static_cast<const char *>(iov.iov_base);
        size_t left = iov.iov_len;
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
}

void PwriteBuffer(int fd, const char *buf, size_t size, uint64_t offset)
{
    size_t left = size;
    const char *ptr = buf;
    while (left > 0)
    {
        const ssize_t w = pwrite(fd, ptr, left, static_cast<off_t>(offset));
        if (w < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            ThrowPosix("pwrite staging readback->final failed");
        }
        ptr += w;
        offset += static_cast<uint64_t>(w);
        left -= static_cast<size_t>(w);
    }
}
} // end anonymous namespace

BbviewWriter::BbviewWriter(IO &io, const std::string &name, const Mode mode, helper::Comm comm)
: Engine("BbviewWriter", io, name, mode, std::move(comm))
{
    m_IO.m_ReadStreaming = false;
    Init();
    m_IsOpen = true;
}

BbviewWriter::~BbviewWriter()
{
    if (m_IsOpen)
    {
        DestructorClose(m_FailVerbose);
    }
    m_IsOpen = false;
}

// PRIVATE --------------------------------------------------------------------

void BbviewWriter::Init()
{
    InitParameters();
    m_RankMPI = m_Comm.Rank();

    if (m_OpenMode != Mode::Write)
    {
        helper::Throw<std::invalid_argument>("Engine", "BbviewWriter", "Init",
                                             "the bbview engine supports write mode only "
                                             "(Append and Read are not implemented; read "
                                             "output with the bp5 engine)");
    }

    m_BP5Serializer.m_Engine = this;
    m_BP5Serializer.m_StatsLevel = static_cast<int>(m_Parameters.StatsLevel);

    if (m_Parameters.DirectIO)
    {
        if (m_Parameters.verbose > 0)
        {
            std::cout << "bbview engine: DirectIO is not yet supported, parameter ignored\n";
        }
    }

    if (m_Parameters.BurstBufferPath.empty())
    {
        std::string tmpdir;
        if (helper::GetEnv("TMPDIR", tmpdir) && !tmpdir.empty())
        {
            m_BurstBufferPath = tmpdir;
        }
        else
        {
            m_BurstBufferPath = "/tmp";
        }
    }
    else
    {
        m_BurstBufferPath = m_Parameters.BurstBufferPath;
    }

    InitTransports();
}

void BbviewWriter::InitParameters()
{
    ParseParams(m_IO, m_Parameters);

    // bbview-specific parameters (case-insensitive)
    Params lower;
    for (const auto &kv : m_IO.m_Parameters)
    {
        std::string k = kv.first;
        std::transform(k.begin(), k.end(), k.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        lower[k] = kv.second;
    }
    uint64_t u = 0;
    if (helper::GetParameter(lower, "drainthreads", u))
    {
        if (u < 1 || u > 16)
        {
            helper::Throw<std::invalid_argument>("Engine", "BbviewWriter", "InitParameters",
                                                 "DrainThreads must be in [1,16], got " +
                                                     std::to_string(u));
        }
        m_DrainThreads = static_cast<unsigned int>(u);
    }
    std::string s;
    if (helper::GetParameter(lower, "stagingsync", s))
    {
        m_StagingSync = (s == "true" || s == "on" || s == "1");
    }
    if (helper::GetParameter(lower, "draindatasync", s))
    {
        m_DrainDataSync = (s == "true" || s == "on" || s == "1");
    }
    if (helper::GetParameter(lower, "commitpollms", u))
    {
        m_CommitPollMs = static_cast<unsigned int>(u);
    }
    float f = 0.f;
    if (helper::GetParameter(lower, "committimeoutsecs", f))
    {
        m_CommitTimeoutSecs = f;
    }
    if (helper::GetParameter(lower, "deletestagedfiles", s))
    {
        m_DeleteStagedFiles = (s == "true" || s == "on" || s == "1");
    }
}

void BbviewWriter::InitTransports()
{
    // TEMPORARY: open-phase profiling to diagnose multi-second Open on the
    // test cluster. One line per rank to stderr; remove after diagnosis.
    const auto tOpenStart = std::chrono::steady_clock::now();
    const auto openPhase = [this, tOpenStart](const char *phase) {
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - tOpenStart)
                              .count();
        std::cerr << "bbview rank " << m_RankMPI << " open-phase " << phase << " @"
                  << static_cast<long long>(ms) << " ms\n";
        std::cerr.flush();
    };

    const std::string finalDir = m_Name;
    const std::string mdFile = GetBPMetadataFileName(m_Name);
    const std::string mmdFile = GetBPMetaMetadataFileNames({m_Name})[0];
    const std::string mdIdxFile = GetBPMetadataIndexFileName(m_Name);
    m_DataFileName = GetBPSubStreamName(m_Name, static_cast<size_t>(m_RankMPI));

    if (m_OpenMode == Mode::Write)
    {
        std::vector<std::string> filesToKeep;
        filesToKeep.push_back(std::filesystem::path(mdFile).filename().string());
        filesToKeep.push_back(std::filesystem::path(mmdFile).filename().string());
        filesToKeep.push_back(std::filesystem::path(mdIdxFile).filename().string());
        for (int r = 0; r < m_Comm.Size(); ++r)
        {
            filesToKeep.push_back(
                std::filesystem::path(GetBPSubStreamName(m_Name, static_cast<size_t>(r)))
                    .filename()
                    .string());
        }
        helper::CleanupBPDirectory(finalDir, filesToKeep, m_Comm);
    }
    openPhase("cleanup");

    if (m_RankMPI == 0)
    {
        std::error_code ec;
        std::filesystem::create_directories(finalDir, ec);
        Params tp = m_IO.m_TransportsParameters.empty() ? Params() : m_IO.m_TransportsParameters[0];
        tp["DirectIO"] = "false"; // metadata files stay buffered
        m_MMDFile = transport::OpenFile(m_Comm, mmdFile, m_OpenMode, tp, true);
        m_MDFile = transport::OpenFile(m_Comm, mdFile, m_OpenMode, tp, true);
        m_MDIndexFile = transport::OpenFile(m_Comm, mdIdxFile, m_OpenMode, tp, true);
        WriteIndexHeaderAndWriterMap();
    }
    openPhase("rank0-md-files");
    m_Comm.Barrier();
    openPhase("barrier1");

    // final data sub-file: raw fd, only drain threads pwrite into it
    m_DataFd = open(m_DataFileName.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (m_DataFd < 0)
    {
        ThrowPosix("open of data sub-file " + m_DataFileName);
    }
    openPhase("data-fd");

    // node-local burst buffer staging directory; file names carry the rank so
    // that multiple ranks sharing a node never clobber each other's steps
    m_StagingDir = m_BurstBufferPath + "/bbview-" + SanitizeForPath(m_Name);
    std::error_code ec;
    std::filesystem::create_directories(m_StagingDir, ec);
    if (ec)
    {
        helper::Throw<std::runtime_error>("Engine", "BbviewWriter", "InitTransports",
                                          "cannot create burst buffer staging directory " +
                                              m_StagingDir + ": " + ec.message());
    }
    openPhase("staging-mkdir");

    char hostname[256] = {0};
    if (gethostname(hostname, sizeof(hostname) - 1) != 0)
    {
        std::strncpy(hostname, "unknown", sizeof(hostname) - 1);
    }
    m_Host = hostname;

    // initial drain progress marker for this rank: a single fixed-size
    // seqlock record, created once and updated in place (see WriteProgress)
    m_ProgressPath = finalDir + "/bbview.drain." + std::to_string(m_RankMPI);
    m_ProgressFd =
        open(m_ProgressPath.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (m_ProgressFd < 0)
    {
        ThrowPosix("create progress file " + m_ProgressPath + " (rank " +
                   std::to_string(m_RankMPI) + ", host " + m_Host + ")");
    }
    WriteProgress(0, 0);
    openPhase("progress-file");
    m_MetaDataPos = 0; // BP5 md.0 carries no header in this format version

    m_Comm.Barrier();
    openPhase("barrier2");
    StartDrainThreads();
    openPhase("threads-started");

    if (m_Parameters.verbose > 0)
    {
        std::cout << "bbview rank " << m_RankMPI << " opened stream " << m_Name
                  << " staging=" << m_StagingDir << " drainThreads=" << m_DrainThreads << "\n";
    }
}

/* ---- engine step API ------------------------------------------------------ */

StepStatus BbviewWriter::BeginStep(StepMode /*mode*/, const float /*timeoutSeconds*/)
{
    if (m_BetweenStepPairs)
    {
        helper::Throw<std::logic_error>("Engine", "BbviewWriter", "BeginStep",
                                        "BeginStep() called twice without EndStep()");
    }
    CheckDrainError();
    m_BetweenStepPairs = true;

    if (m_IsFirstStep && m_Parameters.UseOneTimeAttributes)
    {
        const auto &attributes = m_IO.GetAttributes();
        for (const auto &attributePair : attributes)
        {
            m_BP5Serializer.OnetimeMarshalAttribute(*(attributePair.second));
        }
        m_MarshalAttributesNecessary = false;
    }
    m_IsFirstStep = false;

    if (m_Parameters.BufferVType == (int)BufferVType::MallocVType)
    {
        m_BP5Serializer.InitStep(new format::MallocV(
            "BbviewWriter", false, m_BP5Serializer.m_BufferAlign, m_BP5Serializer.m_BufferBlockSize,
            m_Parameters.InitialBufferSize, m_Parameters.GrowthFactor));
    }
    else
    {
        m_BP5Serializer.InitStep(new format::ChunkV("BbviewWriter", false, m_BP5Serializer.m_BufferAlign,
                                            m_BP5Serializer.m_BufferBlockSize,
                                            m_Parameters.BufferChunkSize));
    }
    return StepStatus::OK;
}

size_t BbviewWriter::CurrentStep() const { return m_WriterStep; }

void BbviewWriter::PerformPuts() { m_BP5Serializer.PerformPuts(false); }

void BbviewWriter::EndStep()
{
    CheckDrainError();
    m_BetweenStepPairs = false;

    MarshalAttributes();
    auto TSInfo = m_BP5Serializer.CloseTimestep(static_cast<int>(m_WriterStep), false);

    const size_t dataSize = TSInfo.DataBuffer->Size();

    // Pre-assign the step's position in the final data sub-file. Because each
    // rank owns its sub-file and drains FIFO, offsets assigned here are final,
    // exactly like BP5's EveryoneWrites mode.
    uint64_t finalPos = m_DataPos;
    if (m_Parameters.StripeSize > 1)
    {
        finalPos += helper::PaddingToAlignOffset(finalPos, m_Parameters.StripeSize);
    }

    // Synchronous spill of the step payload to node-local SSD: the bbview
    // critical path. Cheap, contiguous, local; memory is freed immediately.
    const std::string stagingPath = m_StagingDir + "/step_r" + std::to_string(m_RankMPI) + "_" +
                                    std::to_string(m_WriterStep);
    {
        const int sfd = open(stagingPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (sfd < 0)
        {
            ThrowPosix("open staging file " + stagingPath);
        }
        std::vector<core::iovec> dataVec = TSInfo.DataBuffer->DataVec();
        PwriteAll(sfd, dataVec, 0);
        if (m_StagingSync && fsync(sfd) != 0)
        {
            ThrowPosix("fsync staging file " + stagingPath);
        }
        close(sfd);
    }
    delete TSInfo.DataBuffer;
    TSInfo.DataBuffer = nullptr;

    m_DataPos = finalPos + dataSize;

    // hand off to the in-process bbviewd (was: xattr + socket message)
    {
        DrainJob job;
        job.Seq = m_NextDataSeq++;
        job.Step = m_WriterStep;
        job.StagingPath = stagingPath;
        job.FinalPos = finalPos;
        job.DataSize = dataSize;
        {
            std::lock_guard<std::mutex> lck(m_Mutex);
            m_DataQueue.push_back(std::move(job));
        }
        m_DataCV.notify_one();
    }

    // Collective, small: aggregate this step's metadata and queue its commit
    // on rank 0. The commit only becomes visible once the data has drained.
    AggregateAndEnqueueCommit(TSInfo, finalPos, dataSize);

    ++m_WriterStep;
}

void BbviewWriter::Flush(const int /*transportIndex*/)
{
    if (m_BetweenStepPairs)
    {
        helper::Throw<std::logic_error>("Engine", "BbviewWriter", "Flush",
                                        "Flush() inside a step is not supported by the bbview "
                                        "engine; call it between EndStep() and BeginStep()");
    }
    DrainAndCommitAll();
}

void BbviewWriter::NotifyEngineAttribute(std::string /*name*/, AttributeBase *Attr,
                                         void * /*data*/) noexcept
{
    if (m_Parameters.UseOneTimeAttributes)
    {
        m_BP5Serializer.OnetimeMarshalAttribute(*Attr);
    }
    else
    {
        m_MarshalAttributesNecessary = true;
    }
}

/* ---- drain machinery (daemonless bbviewd) -------------------------------- */

void BbviewWriter::StartDrainThreads()
{
    for (unsigned int i = 0; i < m_DrainThreads; ++i)
    {
        m_DrainWorkers.emplace_back(&BbviewWriter::WorkerLoop, this);
    }
    if (m_RankMPI == 0)
    {
        m_CommitterThread = std::thread(&BbviewWriter::CommitterLoop, this);
    }
}

void BbviewWriter::StopDrainThreads() noexcept
{
    {
        std::lock_guard<std::mutex> lck(m_Mutex);
        m_Finishing = true;
    }
    m_DataCV.notify_all();
    m_CommitCV.notify_all();
    for (auto &t : m_DrainWorkers)
    {
        if (t.joinable())
        {
            t.join();
        }
    }
    m_DrainWorkers.clear();
    if (m_CommitterThread.joinable())
    {
        m_CommitterThread.join();
    }
}

void BbviewWriter::WorkerLoop()
{
    for (;;)
    {
        DrainJob job;
        bool have = false;
        {
            std::unique_lock<std::mutex> lck(m_Mutex);
            m_DataCV.wait(lck, [this] {
                return m_Finishing || m_DrainError != nullptr || !m_DataQueue.empty();
            });
            if (!m_DataQueue.empty())
            {
                job = std::move(m_DataQueue.front());
                m_DataQueue.pop_front();
                have = true;
            }
            else
            {
                return; // finishing or error and nothing left
            }
        }
        if (!have)
        {
            continue;
        }

        try
        {
            RunDataJob(job);
        }
        catch (...)
        {
            {
                std::lock_guard<std::mutex> lck(m_Mutex);
                if (!m_DrainError)
                {
                    m_DrainError = std::current_exception();
                }
            }
            m_DataCV.notify_all();
            m_CommitCV.notify_all();
            return;
        }

        {
            std::lock_guard<std::mutex> lck(m_Mutex);
            m_DoneDataSeqs.insert(job.Seq);
            while (m_DoneDataSeqs.count(m_DrainedWatermark) != 0)
            {
                m_DoneDataSeqs.erase(m_DrainedWatermark);
                ++m_DrainedWatermark;
            }
            m_ProgressBytes += job.DataSize;
            if (m_DrainedWatermark > m_PublishedSteps)
            {
                const uint64_t publishSteps = m_DrainedWatermark;
                const uint64_t publishBytes = m_ProgressBytes;
                m_PublishedSteps = publishSteps;
                // publish under the lock so sequence numbers stay serialized
                // across drain threads; the update itself is one small pwrite
                try
                {
                    WriteProgress(publishSteps, publishBytes);
                }
                catch (...)
                {
                    if (!m_DrainError)
                    {
                        m_DrainError = std::current_exception();
                    }
                }
            }
        }
        m_DataCV.notify_all();
        m_CommitCV.notify_all();
    }
}

void BbviewWriter::RunDataJob(DrainJob &job)
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
            helper::Throw<std::runtime_error>("Engine", "BbviewWriter", "RunDataJob",
                                              "short read of staging file " + job.StagingPath +
                                                  " at " + std::to_string(inOff) + " (" +
                                                  std::to_string(remain) + " bytes still due)");
        }
        PwriteBuffer(m_DataFd, buf.data(), static_cast<size_t>(n), outOff);
        inOff += static_cast<uint64_t>(n);
        outOff += static_cast<uint64_t>(n);
        remain -= static_cast<size_t>(n);
    }
    close(sfd);

    if (m_DeleteStagedFiles)
    {
        std::error_code ec;
        std::filesystem::remove(job.StagingPath, ec); // best effort
    }

    if (m_Parameters.verbose > 2)
    {
        std::cout << "bbview rank " << m_RankMPI << " drained step " << job.Step << " ("
                  << job.DataSize << " bytes -> " << job.FinalPos << ")\n";
    }
}

void BbviewWriter::CommitterLoop()
{
    for (;;)
    {
        DrainJob job;
        bool have = false;
        {
            std::unique_lock<std::mutex> lck(m_Mutex);
            m_CommitCV.wait_for(lck, std::chrono::milliseconds(m_CommitPollMs), [this] {
                return m_Finishing || m_DrainError != nullptr || !m_CommitQueue.empty();
            });
            if (!m_CommitQueue.empty())
            {
                job = std::move(m_CommitQueue.front());
                m_CommitQueue.pop_front();
                have = true;
            }
            else if (m_Finishing || m_DrainError)
            {
                return;
            }
        }
        if (!have)
        {
            continue;
        }

        try
        {
            WaitForAllRanksDrained(job.Step);
            RunCommitJob(job);
            {
                std::lock_guard<std::mutex> lck(m_Mutex);
                ++m_CommitDone;
            }
            m_CommitCV.notify_all();
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lck(m_Mutex);
            if (!m_DrainError)
            {
                m_DrainError = std::current_exception();
            }
            m_DataCV.notify_all();
            m_CommitCV.notify_all();
            return;
        }
    }
}

void BbviewWriter::RunCommitJob(const DrainJob &job)
{
    if (!job.MMDBytes.empty())
    {
        m_MMDFile->Write(job.MMDBytes.data(), job.MMDBytes.size());
        m_MMDFile->Flush();
    }
    m_MDFile->Write(job.MDRecord.data(), job.MDRecord.size());
    m_MDFile->Flush();
    m_MDIndexFile->Write(job.IdxRecord.data(), job.IdxRecord.size());
    m_MDIndexFile->Flush();
    // The md.idx record is the visibility marker: readers only ever see steps
    // whose data is already on the final store.

    if (m_Parameters.verbose > 2)
    {
        std::cout << "bbview committed step " << job.Step << " metadata at md pos "
                  << " size " << job.MDRecord.size() << "\n";
    }
}

void BbviewWriter::WaitForAllRanksDrained(size_t step)
{
    const auto start = std::chrono::steady_clock::now();
    const bool forever = (m_CommitTimeoutSecs <= 0.f);
    const auto timeout = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<float>(m_CommitTimeoutSecs));
    const auto poll = std::chrono::milliseconds(m_CommitPollMs);

    // my own drain state first (in-memory)
    for (;;)
    {
        {
            std::lock_guard<std::mutex> lck(m_Mutex);
            if (m_DrainError)
            {
                std::rethrow_exception(m_DrainError);
            }
            if (m_Finishing)
            {
                helper::Throw<std::logic_error>("Engine", "BbviewWriter",
                                                "WaitForAllRanksDrained",
                                                "engine is closing; commit of step " +
                                                    std::to_string(step) + " abandoned");
            }
            if (m_DrainedWatermark >= step + 1)
            {
                break;
            }
        }
        if (!forever && (std::chrono::steady_clock::now() - start) > timeout)
        {
            helper::Throw<std::runtime_error>("Engine", "BbviewWriter",
                                              "WaitForAllRanksDrained",
                                              "timeout waiting for own drain of step " +
                                                  std::to_string(step));
        }
        std::this_thread::sleep_for(poll);
    }

    // remote ranks via their atomically-updated progress files
    const int size = m_Comm.Size();
    for (int r = 1; r < size; ++r)
    {
        const std::string path = m_Name + "/bbview.drain." + std::to_string(r);
        for (;;)
        {
            {
                std::lock_guard<std::mutex> lck(m_Mutex);
                if (m_DrainError)
                {
                    std::rethrow_exception(m_DrainError);
                }
            }
            DrainProgress p;
            if (ReadProgress(path, p) && p.DrainedSteps >= step + 1)
            {
                break;
            }
            {
                std::lock_guard<std::mutex> lck(m_Mutex);
                if (m_Finishing)
                {
                    helper::Throw<std::logic_error>("Engine", "BbviewWriter",
                                                    "WaitForAllRanksDrained",
                                                    "engine is closing; commit of step " +
                                                        std::to_string(step) + " abandoned");
                }
            }
            if (!forever && (std::chrono::steady_clock::now() - start) > timeout)
            {
                helper::Throw<std::runtime_error>("Engine", "BbviewWriter",
                                                  "WaitForAllRanksDrained",
                                                  "timeout waiting for drain of step " +
                                                      std::to_string(step) + " on rank " +
                                                      std::to_string(r) + " (progress: " +
                                                      (ReadProgress(path, p)
                                                           ? std::to_string(p.DrainedSteps) + " steps"
                                                           : std::string("no progress file")) +
                                                      ")");
            }
            std::this_thread::sleep_for(poll);
        }
    }
}

void BbviewWriter::WriteProgress(uint64_t drainedSteps, uint64_t drainedBytes)
{
    DrainProgress p;
    p.Rank = static_cast<uint64_t>(m_RankMPI);
    p.DrainedSteps = drainedSteps;
    p.DrainedBytes = drainedBytes;
    p.Seq = ++m_ProgressSeq;
    p.SeqCopy = p.Seq;

    // single fixed-offset pwrite of the whole record: no tmp file, no rename
    const char *ptr = reinterpret_cast<const char *>(&p);
    size_t off = 0;
    size_t left = sizeof(p);
    while (left > 0)
    {
        const ssize_t w = pwrite(m_ProgressFd, ptr, left, static_cast<off_t>(off));
        if (w < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            ThrowPosix("write progress " + m_ProgressPath + " (rank " +
                       std::to_string(m_RankMPI) + ", host " + m_Host + ")");
        }
        ptr += w;
        off += static_cast<uint64_t>(w);
        left -= static_cast<size_t>(w);
    }
}

bool BbviewWriter::ReadProgress(const std::string &path, DrainProgress &out)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return false;
    }
    char buf[sizeof(DrainProgress)];
    size_t got = 0;
    while (got < sizeof(buf))
    {
        const ssize_t n = read(fd, buf + got, sizeof(buf) - got);
        if (n <= 0)
        {
            close(fd);
            return false; // not (fully) written yet
        }
        got += static_cast<size_t>(n);
    }
    close(fd);
    std::memcpy(&out, buf, sizeof(buf));
    // seqlock validation: disagreeing sequence numbers mean the record was
    // caught mid-update; the caller polls and will retry
    return out.Magic == DrainMagicValue && out.Version == 1 && out.Seq == out.SeqCopy;
}

void BbviewWriter::CheckDrainError()
{
    std::exception_ptr err;
    {
        std::lock_guard<std::mutex> lck(m_Mutex);
        err = m_DrainError;
    }
    if (err)
    {
        std::rethrow_exception(err);
    }
}

void BbviewWriter::DrainAndCommitAll()
{
    CheckDrainError();
    {
        std::unique_lock<std::mutex> lck(m_Mutex);
        m_DataCV.wait(lck,
                      [this] { return m_DrainedWatermark >= m_NextDataSeq || m_DrainError; });
    }
    CheckDrainError();

    m_Comm.Barrier();

    if (m_RankMPI == 0)
    {
        std::unique_lock<std::mutex> lck(m_Mutex);
        m_CommitCV.wait(lck, [this] { return m_CommitDone >= m_CommitEnqueued || m_DrainError; });
    }
    CheckDrainError();

    if (m_DrainDataSync && m_DataFd >= 0 && fsync(m_DataFd) != 0)
    {
        ThrowPosix("fsync of data sub-file " + m_DataFileName);
    }
    m_Comm.Barrier();
}

/* ---- metadata ------------------------------------------------------------ */

void BbviewWriter::AggregateAndEnqueueCommit(format::BP5Serializer::TimestepInfo &TSInfo,
                                             uint64_t finalPos, size_t dataSize)
{
    core::iovec m{TSInfo.MetaEncodeBuffer->Data(), TSInfo.MetaEncodeBuffer->m_FixedSize};
    core::iovec a{nullptr, 0};
    if (TSInfo.AttributeEncodeBuffer)
    {
        a = {TSInfo.AttributeEncodeBuffer->Data(), TSInfo.AttributeEncodeBuffer->m_FixedSize};
    }

    std::vector<char> metaBuffer = m_BP5Serializer.CopyMetadataToContiguous(
        TSInfo.NewMetaMetaBlocks, {m}, {a}, {static_cast<uint64_t>(dataSize)},
        {finalPos}); // finalPos: file position of this step's data byte 0

    const int size = m_Comm.Size();
    std::vector<char> recvBuf;
    std::vector<size_t> counts;
    if (size > 1)
    {
        counts = m_Comm.GatherValues(metaBuffer.size(), 0);
        if (m_RankMPI == 0)
        {
            uint64_t total = 0;
            for (auto n : counts)
            {
                total += n;
            }
            recvBuf.resize(total);
        }
        m_Comm.GathervArrays(metaBuffer.data(), metaBuffer.size(), counts.data(), counts.size(),
                             recvBuf.data(), 0);
    }

    if (m_RankMPI != 0)
    {
        return; // rank 0 owns the metadata files and the committer
    }

    if (size == 1)
    {
        counts.push_back(metaBuffer.size());
        recvBuf.swap(metaBuffer);
    }

    std::vector<format::BP5Base::MetaMetaInfoBlock> uniqueMMB;
    std::vector<core::iovec> attributeBlocks;
    std::vector<uint64_t> dataSizes;
    std::vector<uint64_t> writerDataPos;
    auto metadataBlocks = m_BP5Serializer.BreakoutContiguousMetadata(
        recvBuf, counts, uniqueMMB, attributeBlocks, dataSizes, writerDataPos);

    if (writerDataPos.size() != static_cast<size_t>(size))
    {
        helper::Throw<std::logic_error>("Engine", "BbviewWriter", "AggregateAndEnqueueCommit",
                                        "expected " + std::to_string(size) +
                                            " writer data positions, got " +
                                            std::to_string(writerDataPos.size()));
    }

    DrainJob job;
    job.Step = m_WriterStep;
    job.IsCommit = true;

    // mmd.0 records: [idLen][infoLen][id][info] per new meta-metadata block
    {
        size_t pos = 0;
        for (const auto &b : uniqueMMB)
        {
            job.MMDBytes.resize(job.MMDBytes.size() + 2 * sizeof(uint64_t) + b.MetaMetaIDLen +
                                b.MetaMetaInfoLen);
            uint64_t idLen = b.MetaMetaIDLen;
            uint64_t infoLen = b.MetaMetaInfoLen;
            helper::CopyToBuffer(job.MMDBytes, pos, &idLen);
            helper::CopyToBuffer(job.MMDBytes, pos, &infoLen);
            helper::CopyToBuffer(job.MMDBytes, pos, b.MetaMetaID, idLen);
            helper::CopyToBuffer(job.MMDBytes, pos, b.MetaMetaInfo, infoLen);
        }
    }

    // md.0 step record: [totalSize][metaBlockSizes...][attrBlockSizes...][blocks...]
    // exactly as BP5Writer::WriteMetadata produces
    {
        std::vector<uint64_t> sizeVector, attrSizeVector;
        uint64_t totalSize = 0;
        for (const auto &b : metadataBlocks)
        {
            sizeVector.push_back(b.iov_len);
            totalSize += sizeof(uint64_t) + b.iov_len;
        }
        for (const auto &b : attributeBlocks)
        {
            attrSizeVector.push_back(b.iov_len);
            totalSize += sizeof(uint64_t) + b.iov_len;
        }
        size_t pos = 0;
        job.MDRecord.resize(sizeof(uint64_t) * (1 + sizeVector.size() + attrSizeVector.size()));
        helper::CopyToBuffer(job.MDRecord, pos, &totalSize);
        helper::CopyToBuffer(job.MDRecord, pos, sizeVector.data(), sizeVector.size());
        helper::CopyToBuffer(job.MDRecord, pos, attrSizeVector.data(), attrSizeVector.size());
        for (const auto &b : metadataBlocks)
        {
            if (!b.iov_base)
            {
                continue;
            }
            const size_t old = job.MDRecord.size();
            job.MDRecord.resize(old + b.iov_len);
            std::memcpy(job.MDRecord.data() + old, b.iov_base, b.iov_len);
        }
        for (const auto &b : attributeBlocks)
        {
            if (!b.iov_base)
            {
                continue;
            }
            const size_t old = job.MDRecord.size();
            job.MDRecord.resize(old + b.iov_len);
            std::memcpy(job.MDRecord.data() + old, b.iov_base, b.iov_len);
        }
    }
    const uint64_t metaDataSize = static_cast<uint64_t>(job.MDRecord.size());
    const uint64_t metaDataPos = m_MetaDataPos;
    m_MetaDataPos += metaDataSize;

    // md.idx step record: [s][len][mdPos][mdSize][flushCount=0][writerDataPos...]
    {
        size_t pos = 0;
        // payload after the record byte: length + mdPos + mdSize + flushCount
        // + W positions; the length value itself counts 3+W uint64s (as BP5)
        job.IdxRecord.resize(1 + (4 + static_cast<size_t>(size)) * sizeof(uint64_t));
        const unsigned char record = static_cast<unsigned char>(StepRecord);
        helper::CopyToBuffer(job.IdxRecord, pos, &record, 1);
        uint64_t d = (3 + static_cast<uint64_t>(size)) * sizeof(uint64_t);
        helper::CopyToBuffer(job.IdxRecord, pos, &d);
        helper::CopyToBuffer(job.IdxRecord, pos, &metaDataPos);
        helper::CopyToBuffer(job.IdxRecord, pos, &metaDataSize);
        d = 0; // no intra-step flushes
        helper::CopyToBuffer(job.IdxRecord, pos, &d);
        helper::CopyToBuffer(job.IdxRecord, pos, writerDataPos.data(), writerDataPos.size());
    }

    {
        std::lock_guard<std::mutex> lck(m_Mutex);
        m_CommitQueue.push_back(std::move(job));
        ++m_CommitEnqueued;
    }
    m_CommitCV.notify_one();
}

void BbviewWriter::MakeBbHeader(std::vector<char> &buffer, size_t &position,
                                const std::string & /*fileType*/, const bool isActive)
{
    // Identical byte layout to BP5Writer::MakeHeader so bp5 readers accept it.
    if (buffer.size() < m_IndexHeaderSize)
    {
        buffer.resize(m_IndexHeaderSize);
    }

    const std::string majorVersion(std::to_string(ADIOS2_VERSION_MAJOR));
    const char minorVersionChar = '0' + ADIOS2_VERSION_MINOR;
    const std::string minorVersion(1, minorVersionChar);
    const std::string patchVersion(std::to_string(ADIOS2_VERSION_PATCH));

    std::string versionLongTag("ADIOS-BP v" + majorVersion + "." + minorVersion + "." +
                               patchVersion + " ");
    size_t maxTypeLen = m_VersionTagLength - versionLongTag.size();
    const std::string fileTypeStr = std::string("BB");
    versionLongTag += fileTypeStr.substr(0, maxTypeLen);
    const size_t versionLongTagSize = versionLongTag.size();
    if (versionLongTagSize < m_VersionTagLength)
    {
        helper::CopyToBuffer(buffer, position, versionLongTag.c_str(), versionLongTagSize);
        position += m_VersionTagLength - versionLongTagSize;
    }
    else if (versionLongTagSize > m_VersionTagLength)
    {
        helper::CopyToBuffer(buffer, position, versionLongTag.c_str(), m_VersionTagLength);
    }
    else
    {
        helper::CopyToBuffer(buffer, position, versionLongTag.c_str(), m_VersionTagLength);
    }

    const uint8_t major = static_cast<uint8_t>(ADIOS2_VERSION_MAJOR);
    const uint8_t minor = static_cast<uint8_t>(ADIOS2_VERSION_MINOR);
    const uint8_t patch = static_cast<uint8_t>(ADIOS2_VERSION_PATCH);
    helper::CopyToBuffer(buffer, position, &major);
    helper::CopyToBuffer(buffer, position, &minor);
    helper::CopyToBuffer(buffer, position, &patch);
    position = m_EndianFlagPosition;

    const uint8_t endianness = helper::IsLittleEndian() ? 0 : 1;
    helper::CopyToBuffer(buffer, position, &endianness);
    position = m_BPVersionPosition;
    const uint8_t version = 5;
    helper::CopyToBuffer(buffer, position, &version);
    position = m_BPMinorVersionPosition;
    const uint8_t minorversion = m_BP5MinorVersion;
    helper::CopyToBuffer(buffer, position, &minorversion);
    position = m_ActiveFlagPosition;
    const uint8_t activeFlag = (isActive ? 1 : 0);
    helper::CopyToBuffer(buffer, position, &activeFlag);

    const uint8_t columnMajor = (m_IO.m_ArrayOrder == ArrayOrdering::ColumnMajor) ? 'y' : 'n';
    helper::CopyToBuffer(buffer, position, &columnMajor);
    const uint8_t flatten = m_Parameters.FlattenSteps ? 1 : 0;
    helper::CopyToBuffer(buffer, position, &flatten);

    position = m_FileUUIDPosition;
    std::random_device rd;
    uint32_t fileUUID = static_cast<uint32_t>(rd());
    while (fileUUID == 0)
    {
        fileUUID = static_cast<uint32_t>(rd());
    }
    helper::CopyToBuffer(buffer, position, &fileUUID);
    position = m_IndexHeaderSize;
}

void BbviewWriter::WriteIndexHeaderAndWriterMap()
{
    // md.idx: 64-byte Index Table header + a static WriterMapRecord mapping
    // every writer rank to its own data sub-file (EveryoneWrites style).
    std::vector<char> buf;
    size_t pos = 0;
    MakeBbHeader(buf, pos, "Index Table", true);

    const uint64_t W = static_cast<uint64_t>(m_Comm.Size());
    // payload: length field + WriterCount + AggregatorCount + SubStreams + W map
    // entries (the record's length value counts 3+W payload uint64s, matching
    // BP5Writer::WriteMetadataFileIndex)
    const size_t recSize = 1 + (4 + W) * sizeof(uint64_t);
    buf.resize(buf.size() + recSize);
    const unsigned char record = static_cast<unsigned char>(WriterMapRecord);
    helper::CopyToBuffer(buf, pos, &record, 1);
    uint64_t d = (3 + W) * sizeof(uint64_t);
    helper::CopyToBuffer(buf, pos, &d);
    helper::CopyToBuffer(buf, pos, &W);          // WriterCount
    helper::CopyToBuffer(buf, pos, &W);          // AggregatorCount == writers
    helper::CopyToBuffer(buf, pos, &W);          // SubStreams == writers
    for (uint64_t w = 0; w < W; ++w)
    {
        helper::CopyToBuffer(buf, pos, &w); // writer w -> sub-file w
    }
    m_MDIndexFile->Write(buf.data(), buf.size());
    m_MDIndexFile->Flush();
}

/* ---- put ----------------------------------------------------------------- */

void BbviewWriter::PutCommon(VariableBase &variable, const void *values, bool sync)
{
    if (!m_BetweenStepPairs)
    {
        BeginStep(StepMode::Update);
    }

    auto memSpace = variable.GetMemorySpace(values);
    if (memSpace != MemorySpace::Host)
    {
        sync = true;
    }

    size_t *Shape = nullptr;
    size_t *Start = nullptr;
    size_t *Count = nullptr;
    const size_t DimCount = variable.m_Count.size();

    if (variable.m_ShapeID == ShapeID::GlobalArray)
    {
        Shape = variable.m_Shape.data();
        Count = variable.m_Count.data();
        Start = variable.m_Start.data();
    }
    else if (variable.m_ShapeID == ShapeID::LocalArray)
    {
        Count = variable.m_Count.data();
    }
    else if (variable.m_ShapeID == ShapeID::JoinedArray)
    {
        Count = variable.m_Count.data();
        Shape = variable.m_Shape.data();
    }

    if (!variable.m_MemoryCount.empty())
    {
        helper::Throw<std::invalid_argument>("Engine", "BbviewWriter", "PutCommon",
                                             "Put with MemoryCount/MemoryStart sub-blocks is "
                                             "not supported by the bbview engine");
    }

    if (variable.m_Type == DataType::String)
    {
        std::string &source = *(std::string *)values;
        void *p = &(source[0]);
        m_BP5Serializer.Marshal((void *)&variable, variable.m_Name.c_str(), variable.m_Type,
                                variable.m_ElementSize, DimCount, Shape, Count, Start, &p, sync,
                                nullptr);
    }
    else
    {
        m_BP5Serializer.Marshal((void *)&variable, variable.m_Name.c_str(), variable.m_Type,
                                variable.m_ElementSize, DimCount, Shape, Count, Start, values,
                                sync, nullptr);
    }
}

template <class T>
void BbviewWriter::PutCommonSpan(Variable<T> &variable, typename Variable<T>::Span &span,
                                 const bool initialize, const T &value)
{
    format::BufferV::BufferPos bp5span(0, 0, 0);

    size_t *Shape = nullptr;
    size_t *Start = nullptr;
    size_t *Count = nullptr;
    size_t DimCount = 0;

    if (!m_BetweenStepPairs)
    {
        BeginStep(StepMode::Update);
    }
    if (variable.m_ShapeID == ShapeID::GlobalArray)
    {
        DimCount = variable.m_Shape.size();
        Shape = variable.m_Shape.data();
        Start = variable.m_Start.data();
        Count = variable.m_Count.data();
    }
    else if (variable.m_ShapeID == ShapeID::JoinedArray)
    {
        Shape = variable.m_Shape.data();
        DimCount = variable.m_Count.size();
        Count = variable.m_Count.data();
    }
    else if (variable.m_ShapeID == ShapeID::LocalArray)
    {
        DimCount = variable.m_Count.size();
        Count = variable.m_Count.data();
    }

    m_BP5Serializer.Marshal((void *)&variable, variable.m_Name.c_str(), variable.m_Type,
                            variable.m_ElementSize, DimCount, Shape, Count, Start, nullptr, false,
                            &bp5span);

    span.m_PayloadPosition = bp5span.posInBuffer;
    span.m_BufferIdx = bp5span.bufferIdx;
    span.m_Value = value;

    if (initialize)
    {
        const size_t ElemCount = m_BP5Serializer.CalcSize(DimCount, Count);
        T *itBegin =
            reinterpret_cast<T *>(m_BP5Serializer.GetPtr(span.m_BufferIdx, span.m_PayloadPosition));
        for (size_t i = 0; i < ElemCount; ++i)
        {
            itBegin[i] = value;
        }
    }
}

void BbviewWriter::MarshalAttributes()
{
    if (!m_MarshalAttributesNecessary)
    {
        return;
    }
    m_MarshalAttributesNecessary = false;

    const auto &attributes = m_IO.GetAttributes();
    for (const auto &attributePair : attributes)
    {
        const std::string name(attributePair.first);
        auto baseAttr = &attributePair.second;
        const DataType type((*baseAttr)->m_Type);
        int element_count = -1;

        if (!attributePair.second->m_IsSingleValue)
        {
            element_count = (int)(*baseAttr)->m_Elements;
        }

        if (type == DataType::None)
        {
        }
        else if (type == helper::GetDataType<std::string>())
        {
            core::Attribute<std::string> &attribute = *m_IO.InquireAttribute<std::string>(name);
            void *data_addr;
            if (attribute.m_IsSingleValue)
            {
                data_addr = (void *)attribute.m_DataSingleValue.c_str();
            }
            else
            {
                const char **tmp = (const char **)malloc(sizeof(char *) * element_count);
                for (int i = 0; i < element_count; i++)
                {
                    tmp[i] = attribute.m_DataArray[i].c_str();
                }
                data_addr = (void *)tmp;
                m_BP5Serializer.MarshalAttribute(name.c_str(), type, sizeof(char *), element_count,
                                                 data_addr);
                free(data_addr);
                continue;
            }
            m_BP5Serializer.MarshalAttribute(name.c_str(), type, sizeof(char *), element_count,
                                             data_addr);
        }
#define declare_type(T)                                                                            \
    else if (type == helper::GetDataType<T>())                                                     \
    {                                                                                              \
        core::Attribute<T> &attribute = *m_IO.InquireAttribute<T>(name);                            \
        int element_count = -1;                                                                    \
        void *data_addr = &attribute.m_DataSingleValue;                                            \
        if (!attribute.m_IsSingleValue)                                                            \
        {                                                                                          \
            element_count = (int)attribute.m_Elements;                                             \
            data_addr = attribute.m_DataArray.data();                                              \
        }                                                                                          \
        m_BP5Serializer.MarshalAttribute(attribute.m_Name.c_str(), type, sizeof(T), element_count, \
                                         data_addr);                                               \
    }

        ADIOS2_FOREACH_PRIMITIVE_STDTYPE_1ARG(declare_type)
#undef declare_type
    }
}

/* ---- close --------------------------------------------------------------- */

void BbviewWriter::DoClose(const int /*transportIndex*/)
{
    if (!m_IsOpen)
    {
        return;
    }

    // TEMPORARY: close-phase profiling; see the matching block in
    // InitTransports. Remove after diagnosis.
    const auto tCloseStart = std::chrono::steady_clock::now();
    const auto closePhase = [this, tCloseStart](const char *phase) {
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - tCloseStart)
                              .count();
        std::cerr << "bbview rank " << m_RankMPI << " close-phase " << phase << " @"
                  << static_cast<long long>(ms) << " ms\n";
        std::cerr.flush();
    };

    std::exception_ptr err;
    try
    {
        DrainAndCommitAll();
    }
    catch (...)
    {
        err = std::current_exception();
        {
            std::lock_guard<std::mutex> lck(m_Mutex);
            if (!m_DrainError)
            {
                m_DrainError = std::current_exception();
            }
        }
    }
    closePhase("drain-commit-all");

    StopDrainThreads();
    closePhase("stop-threads");

    if (!err)
    {
        try
        {
            CheckDrainError();
            if (m_RankMPI == 0)
            {
                // final active flag = 0 in md.idx header
                const char inactive = 0;
                m_MDIndexFile->Write(&inactive, 1, m_ActiveFlagPosition);
                m_MDIndexFile->Flush();
            }
        }
        catch (...)
        {
            err = std::current_exception();
        }
    }
    closePhase("inactive-flag");

    if (m_DataFd >= 0)
    {
        close(m_DataFd);
        m_DataFd = -1;
    }
    if (m_ProgressFd >= 0)
    {
        close(m_ProgressFd);
        m_ProgressFd = -1;
    }
    if (m_MDFile)
    {
        m_MDFile->Close();
    }
    if (m_MMDFile)
    {
        m_MMDFile->Close();
    }
    if (m_MDIndexFile)
    {
        m_MDIndexFile->Close();
    }
    closePhase("close-files");

    // drain progress markers are only meaningful while the writer lives
    std::error_code ec;
    if (!m_ProgressPath.empty())
    {
        std::filesystem::remove(m_ProgressPath, ec);
    }
    m_Comm.Barrier();
    if (!m_StagingDir.empty())
    {
        std::filesystem::remove(m_StagingDir, ec); // succeeds only if empty
    }
    closePhase("marker-cleanup");

    m_IsOpen = false;

    if (!err)
    {
        try
        {
            CheckDrainError();
        }
        catch (...)
        {
            err = std::current_exception();
        }
    }
    if (err)
    {
        std::rethrow_exception(err);
    }
}

void BbviewWriter::DestructorClose(bool /*Verbose*/) noexcept
{
    // Abandoned open engine: staged-but-undrained data is lost, which is the
    // documented burst-buffer contract (Close() is the durability point).
    // Still, the background threads and fds must be released so that member
    // destruction cannot terminate the process.
    StopDrainThreads();
    if (m_DataFd >= 0)
    {
        close(m_DataFd);
        m_DataFd = -1;
    }
    if (m_ProgressFd >= 0)
    {
        close(m_ProgressFd);
        m_ProgressFd = -1;
    }
    m_IsOpen = false;
}

/* ---- put dispatch -------------------------------------------------------- */

#define declare_type(T)                                                                            \
    void BbviewWriter::DoPutSync(Variable<T> &variable, const T *data)                             \
    {                                                                                              \
        PutCommon(variable, data, true);                                                           \
    }                                                                                              \
    void BbviewWriter::DoPutDeferred(Variable<T> &variable, const T *data)                         \
    {                                                                                              \
        PutCommon(variable, data, false);                                                          \
    }                                                                                              \
    size_t BbviewWriter::PutCount(Variable<T> &variable)                                           \
    {                                                                                              \
        return m_BP5Serializer.PutCount((void *)&variable);                                        \
    }

ADIOS2_FOREACH_STDTYPE_1ARG(declare_type)
#undef declare_type

#define declare_type(T)                                                                            \
    void BbviewWriter::DoPut(Variable<T> &variable, typename Variable<T>::Span &span,              \
                             const bool initialize, const T &value)                                \
    {                                                                                              \
        PutCommonSpan(variable, span, initialize, value);                                          \
    }

ADIOS2_FOREACH_PRIMITIVE_STDTYPE_1ARG(declare_type)
#undef declare_type

#define declare_type(T, L)                                                                         \
    T *BbviewWriter::DoBufferData_##L(const int bufferIdx, const size_t payloadPosition,           \
                                      const size_t bufferID) noexcept                              \
    {                                                                                              \
        return reinterpret_cast<T *>(m_BP5Serializer.GetPtr(bufferIdx, payloadPosition));          \
    }

ADIOS2_FOREACH_PRIMITVE_STDTYPE_2ARGS(declare_type)
#undef declare_type

void BbviewWriter::DoPutStructSync(VariableStruct &variable, const void *data)
{
    PutCommon(variable, data, true);
}

void BbviewWriter::DoPutStructDeferred(VariableStruct &variable, const void *data)
{
    PutCommon(variable, data, false);
}

} // end namespace engine
} // end namespace core
} // end namespace adios2
