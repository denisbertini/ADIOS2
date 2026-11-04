/*
 * SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "BbstageWriter.h"

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
#include <unordered_map>
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

// Staging file names (cf. the original bbview's flattened
// "path-<rank>-<view_index>" scheme), hardened: cap the length and mix in a
// hash to avoid PATH_MAX issues and collisions.
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

void ThrowPosix(const std::string &what)
{
    // helper::Throw (adiosLog.h) always throws, so control never returns from
    // here; it carries no [[noreturn]] attribute, so neither does this
    // wrapper (compilers cannot prove the throw through the template).
    helper::Throw<std::runtime_error>("Engine", "BbstageWriter", what,
                                      what + ": " + std::string(std::strerror(errno)));
}

// BBSTAGE_TIMING=1 enables per-phase Open/Close timing lines on stderr
// (diagnostics for slow Open/Close on a new system; off by default so
// production application logs stay clean).
bool BbstageTimingEnabled()
{
    std::string v;
    return helper::GetEnv("BBSTAGE_TIMING", v) && (v == "1" || v == "true" || v == "on");
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

BbstageWriter::BbstageWriter(IO &io, const std::string &name, const Mode mode, helper::Comm comm)
: Engine("BbstageWriter", io, name, mode, std::move(comm))
{
    m_IO.m_ReadStreaming = false;
    Init();
    m_IsOpen = true;
}

BbstageWriter::~BbstageWriter()
{
    if (m_IsOpen)
    {
        DestructorClose(m_FailVerbose);
    }
    m_IsOpen = false;
}

// PRIVATE --------------------------------------------------------------------

void BbstageWriter::Init()
{
    InitParameters();
    m_RankMPI = m_Comm.Rank();

    if (m_OpenMode != Mode::Write)
    {
        helper::Throw<std::invalid_argument>("Engine", "BbstageWriter", "Init",
                                             "the bbstage engine supports write mode only "
                                             "(Append and Read are not implemented; read "
                                             "output with the bp5 engine)");
    }

    m_BP5Serializer.m_Engine = this;
    m_BP5Serializer.m_StatsLevel = static_cast<int>(m_Parameters.StatsLevel);

    if (m_Parameters.DirectIO)
    {
        if (m_Parameters.verbose > 0)
        {
            std::cout << "bbstage engine: DirectIO is not yet supported, parameter ignored\n";
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

    // Aggregate must be identical on every rank: it drives node collectives
    // at Open/EndStep, so a mismatch would deadlock (or silently miscompute
    // offsets) with no visible failure. Validate collectively before any
    // aggregate collective; runs on every rank, also when aggregate is off,
    // because it is a collective itself.
    {
        const int agg = m_Aggregate ? 1 : 0;
        int aggMin = 0;
        int aggMax = 0;
        m_Comm.Allreduce(&agg, &aggMin, 1, helper::Comm::Op::Min, "bbstage aggregate uniformity");
        m_Comm.Allreduce(&agg, &aggMax, 1, helper::Comm::Op::Max, "bbstage aggregate uniformity");
        if (aggMin != aggMax)
        {
            helper::Throw<std::invalid_argument>("Engine", "BbstageWriter", "Init",
                                                 "Aggregate must be 'on' or 'off' on every "
                                                 "rank: rank " +
                                                     std::to_string(m_RankMPI) + " has '" +
                                                     (m_Aggregate ? "on" : "off") +
                                                     "' but at least one other rank differs");
        }
    }

    // Aggregate mode: resolve the node topology (grouping, leader, per-node
    // communicator) while MPI is alive. It must run before InitTransports,
    // which opens the data sub-files based on the node grouping.
    if (m_Aggregate)
    {
        const bool doTiming = BbstageTimingEnabled();
        if (doTiming)
        {
            std::cerr << "bbstage rank " << m_RankMPI << " node-topology-start\n";
            std::cerr.flush();
        }
        const auto tTopoStart = std::chrono::steady_clock::now();
        SetupNodeTopology();
        if (doTiming)
        {
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - tTopoStart)
                                  .count();
            std::cerr << "bbstage rank " << m_RankMPI << " node-topology @"
                      << static_cast<long long>(ms) << " ms\n";
            std::cerr.flush();
        }
    }

    InitTransports();
}

void BbstageWriter::InitParameters()
{
    ParseParams(m_IO, m_Parameters);

    // bbstage-specific parameters (case-insensitive)
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
            helper::Throw<std::invalid_argument>("Engine", "BbstageWriter", "InitParameters",
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
    // Environment variable fallbacks: bbstage is often reached through
    // middleware (e.g. the WarpX checkpoint path via openPMD-api) where the
    // application cannot pass engine parameters. Precedence: engine
    // parameter > environment variable > built-in default.
    std::string closeMode;
    if (!helper::GetParameter(lower, "closemode", closeMode))
    {
        std::string envCloseMode;
        if (helper::GetEnv("BBSTAGE_CLOSE_MODE", envCloseMode))
        {
            closeMode = envCloseMode;
        }
    }
    if (!closeMode.empty())
    {
        if (closeMode == "staged" || closeMode == "async" || closeMode == "stage")
        {
            m_CloseModeStaged = true;
        }
        else if (closeMode == "durable" || closeMode == "default")
        {
            m_CloseModeStaged = false;
        }
        else
        {
            helper::Throw<std::invalid_argument>("Engine", "BbstageWriter", "InitParameters",
                                                 "CloseMode must be 'durable' or 'staged', got '" +
                                                     closeMode + "'");
        }
    }
    uint64_t watermarkMB = 0;
    bool haveWatermark = helper::GetParameter(lower, "stagingwatermarkmb", watermarkMB);
    if (!haveWatermark)
    {
        std::string envWatermark;
        if (helper::GetEnv("BBSTAGE_STAGING_WATERMARK_MB", envWatermark) &&
            !envWatermark.empty())
        {
            char *end = nullptr;
            const unsigned long long v = std::strtoull(envWatermark.c_str(), &end, 10);
            if (end == envWatermark.c_str() || *end != '\0')
            {
                helper::Throw<std::invalid_argument>("Engine", "BbstageWriter", "InitParameters",
                                                     "BBSTAGE_STAGING_WATERMARK_MB must be a "
                                                     "non-negative integer, got '" +
                                                         envWatermark + "'");
            }
            watermarkMB = v;
            haveWatermark = true;
        }
    }
    if (haveWatermark)
    {
        m_StagingWatermarkBytes = watermarkMB * 1024ULL * 1024ULL;
    }
    // Aggregate: node-aggregated drain (see the class documentation). Must be
    // uniform across ranks: it drives MPI collectives at Open/EndStep.
    std::string agg;
    if (!helper::GetParameter(lower, "aggregate", agg))
    {
        std::string envAgg;
        if (helper::GetEnv("BBSTAGE_AGGREGATE", envAgg) && !envAgg.empty())
        {
            agg = envAgg;
        }
    }
    if (!agg.empty())
    {
        if (agg == "on" || agg == "true" || agg == "1")
        {
            m_Aggregate = true;
        }
        else if (agg == "off" || agg == "false" || agg == "0")
        {
            m_Aggregate = false;
        }
        else
        {
            helper::Throw<std::invalid_argument>("Engine", "BbstageWriter", "InitParameters",
                                                  "Aggregate must be 'on' or 'off', got '" + agg + "'");
        }
    }
}

void BbstageWriter::SetupNodeTopology()
{
    const int W = m_Comm.Size();

    // Single rank, or a non-MPI (serial) communicator: one node, this rank is
    // its leader.
    if (W <= 1 || !m_Comm.IsMPI())
    {
        m_NodeIndex = 0;
        m_NodeLocalRank = 0;
        m_NodeSize = 1;
        m_NodeCount = 1;
        m_IsLeader = true;
        m_NodeMembers = {m_RankMPI};
        m_NodeIndexPerRank.assign(1, 0);
        return;
    }

    // Node identity key: SLURM_NODENAME when available, otherwise the
    // hostname. Both are unique per physical node.
    char myKey[256] = {0};
    std::string slurmNode;
    if (helper::GetEnv("SLURM_NODENAME", slurmNode) && !slurmNode.empty())
    {
        std::strncpy(myKey, slurmNode.c_str(), sizeof(myKey) - 1);
    }
    else
    {
        if (gethostname(myKey, sizeof(myKey) - 1) != 0)
        {
            std::strncpy(myKey, "unknown", sizeof(myKey) - 1);
        }
    }
    myKey[sizeof(myKey) - 1] = '\0';

    // Gather every rank's key to rank 0 (fixed-size char arrays).
    std::vector<char> recvKeys;
    if (m_RankMPI == 0)
    {
        recvKeys.resize(static_cast<size_t>(W) * sizeof(myKey));
    }
    m_Comm.GatherArrays(myKey, sizeof(myKey), recvKeys.data(), 0);

    // Rank 0 assigns a compact node id (0..N-1) to each distinct key, in
    // order of first appearance (i.e. by the lowest global rank on each node),
    // and records every rank's node id.
    std::vector<uint64_t> nodeIndexPerRank(W, 0);
    if (m_RankMPI == 0)
    {
        std::unordered_map<std::string, int> keyToNode;
        int nodeCount = 0;
        for (int r = 0; r < W; ++r)
        {
            const std::string key(recvKeys.data() + static_cast<size_t>(r) * sizeof(myKey));
            auto it = keyToNode.find(key);
            int ni;
            if (it == keyToNode.end())
            {
                ni = nodeCount++;
                keyToNode.emplace(key, ni);
            }
            else
            {
                ni = it->second;
            }
            nodeIndexPerRank[r] = static_cast<uint64_t>(ni);
        }
        m_NodeCount = nodeCount;
        m_NodeIndexPerRank = nodeIndexPerRank;
    }

    // Scatter each rank's node id back (one uint64 per rank) and broadcast the
    // node count.
    uint64_t myNodeIndex = 0;
    m_Comm.Scatter(nodeIndexPerRank.data(), 1, &myNodeIndex, 1, 0);
    m_NodeIndex = static_cast<int>(myNodeIndex);
    m_NodeCount = m_Comm.BroadcastValue(m_NodeCount, 0);

    // Per-node communicator: ranks sharing a node id, ordered by global rank,
    // so local rank 0 is the node's lowest global rank (the leader).
    m_NodeComm = m_Comm.Split(m_NodeIndex, m_RankMPI, "bbstage node communicator");
    m_NodeLocalRank = m_NodeComm.Rank();
    m_NodeSize = m_NodeComm.Size();
    m_IsLeader = (m_NodeLocalRank == 0);
    m_NodeMembers = m_NodeComm.AllGatherValues(m_RankMPI);

    // The committer's in-memory fast path covers rank 0's own node and reads
    // progress files for the other nodes; that is only correct if rank 0 is
    // node 0's leader. It holds by construction (node ids are assigned in
    // first-appearance order, so rank 0 opens node 0, and the node-local
    // order follows the global rank), but enforce it at Open instead of
    // trusting it: a violation would silently skip a node's progress gate.
    if (m_RankMPI == 0 && (!m_IsLeader || m_NodeIndex != 0))
    {
        helper::Throw<std::logic_error>("Engine", "BbstageWriter", "SetupNodeTopology",
                                        "rank 0 is not the leader of node 0 (leader=" +
                                            std::string(m_IsLeader ? "yes" : "no") +
                                            ", nodeIndex=" + std::to_string(m_NodeIndex) +
                                            "); the committer's own-node fast path would be "
                                            "invalid");
    }
}

void BbstageWriter::InitTransports()
{
    // Opt-in per-phase Open timing (BBSTAGE_TIMING=1): one line per phase per
    // rank to stderr; see BbstageTimingEnabled().
    const bool doTiming = BbstageTimingEnabled();
    const auto tOpenStart = std::chrono::steady_clock::now();
    const auto openPhase = [this, tOpenStart, doTiming](const char *phase) {
        if (!doTiming)
        {
            return;
        }
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - tOpenStart)
                              .count();
        std::cerr << "bbstage rank " << m_RankMPI << " open-phase " << phase << " @"
                  << static_cast<long long>(ms) << " ms\n";
        std::cerr.flush();
    };

    const std::string finalDir = m_Name;
    const std::string mdFile = GetBPMetadataFileName(m_Name);
    const std::string mmdFile = GetBPMetaMetadataFileNames({m_Name})[0];
    const std::string mdIdxFile = GetBPMetadataIndexFileName(m_Name);
    // Aggregate mode: one data sub-file per node, named by the node id; the
    // node leader is the only rank that writes it. Otherwise: one sub-file per
    // writer rank (EveryoneWrites).
    m_DataFileName = m_Aggregate ? GetBPSubStreamName(m_Name, static_cast<size_t>(m_NodeIndex))
                                 : GetBPSubStreamName(m_Name, static_cast<size_t>(m_RankMPI));

    if (m_OpenMode == Mode::Write)
    {
        std::vector<std::string> filesToKeep;
        filesToKeep.push_back(std::filesystem::path(mdFile).filename().string());
        filesToKeep.push_back(std::filesystem::path(mmdFile).filename().string());
        filesToKeep.push_back(std::filesystem::path(mdIdxFile).filename().string());
        const int nSubFiles = m_Aggregate ? m_NodeCount : m_Comm.Size();
        for (int r = 0; r < nSubFiles; ++r)
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

    // final data sub-file: raw fd, only drain threads pwrite into it. In
    // aggregate mode the node leader is the only rank that opens it (it drains
    // every rank on the node); non-leaders keep m_DataFd = -1.
    if (!(m_Aggregate && !m_IsLeader))
    {
        m_DataFd = open(m_DataFileName.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (m_DataFd < 0)
        {
            ThrowPosix("open of data sub-file " + m_DataFileName);
        }
    }
    openPhase("data-fd");

    // node-local burst buffer staging directory; file names carry the rank so
    // that multiple ranks sharing a node never clobber each other's steps
    m_StagingDir = m_BurstBufferPath + "/bbstage-" + SanitizeForPath(m_Name);
    std::error_code ec;
    // remove stale staging files left by a crashed previous run of the same
    // name: the staging dir is a node-local cache, and leftovers would
    // accumulate and block the end-of-run directory removal
    if (std::filesystem::exists(m_StagingDir, ec))
    {
        for (auto it = std::filesystem::directory_iterator(m_StagingDir, ec);
             !ec && it != std::filesystem::directory_iterator{}; it.increment(ec))
        {
            if (it->is_regular_file(ec))
            {
                std::filesystem::remove(it->path(), ec);
            }
        }
    }
    std::filesystem::create_directories(m_StagingDir, ec);
    if (ec)
    {
        helper::Throw<std::runtime_error>("Engine", "BbstageWriter", "InitTransports",
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

    // drain progress marker for this rank: a single fixed-size seqlock
    // record. Created lazily by the first real progress update (first drain
    // completion) instead of here: the initial create+write is a pure MDT
    // round trip on the open critical path, and on the test cluster its stall
    // tail (observed 0.1-2 s on Lustre) dominated Open. The committer treats
    // a missing progress file as "progress 0", which is exactly the initial
    // state, so nothing depends on the file existing after Open.
    // Aggregate mode: one progress record per node (bbstage.drain.<nodeId>),
    // published by the node leader; otherwise one per writer rank.
    const int progressId = m_Aggregate ? m_NodeIndex : m_RankMPI;
    m_ProgressPath = finalDir + "/bbstage.drain." + std::to_string(progressId);
    openPhase("progress-file");
    m_MetaDataPos = 0; // BP5 md.0 carries no header in this format version

    m_Comm.Barrier();
    openPhase("barrier2");
    if (m_CloseModeStaged)
    {
        // staged close: no per-instance drain workers; the process-level
        // finalizer takes the session over at Close() and does the draining.
        // Aggregate mode: the watermark is a per-node budget (the leader
        // tracks the node's total staged-but-undrained bytes), so scale the
        // per-rank setting by the number of ranks on this node.
        // m_NodeSize is m_NodeComm.Size() and therefore >= 1.
        const uint64_t watermark =
            m_Aggregate ? m_StagingWatermarkBytes * static_cast<uint64_t>(m_NodeSize)
                        : m_StagingWatermarkBytes;
        BbstageStagedFinalizer::Instance().SetStagingWatermark(watermark);
    }
    else
    {
        StartDrainThreads();
    }
    openPhase("threads-started");

    if (m_Parameters.verbose > 0)
    {
        std::cout << "bbstage rank " << m_RankMPI << " opened stream " << m_Name
                  << " staging=" << m_StagingDir << " drainThreads=" << m_DrainThreads
                  << " closeMode=" << (m_CloseModeStaged ? "staged" : "durable") << "\n";
    }
}

/* ---- engine step API ------------------------------------------------------ */

StepStatus BbstageWriter::BeginStep(StepMode /*mode*/, const float /*timeoutSeconds*/)
{
    if (m_BetweenStepPairs)
    {
        helper::Throw<std::logic_error>("Engine", "BbstageWriter", "BeginStep",
                                        "BeginStep() called twice without EndStep()");
    }
    CheckDrainError();
    BbstageStagedFinalizer::Instance().CheckError(); // staged mode: earlier sessions
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
            "BbstageWriter", false, m_BP5Serializer.m_BufferAlign, m_BP5Serializer.m_BufferBlockSize,
            m_Parameters.InitialBufferSize, m_Parameters.GrowthFactor));
    }
    else
    {
        m_BP5Serializer.InitStep(new format::ChunkV("BbstageWriter", false, m_BP5Serializer.m_BufferAlign,
                                            m_BP5Serializer.m_BufferBlockSize,
                                            m_Parameters.BufferChunkSize));
    }
    return StepStatus::OK;
}

size_t BbstageWriter::CurrentStep() const { return m_WriterStep; }

void BbstageWriter::PerformPuts() { m_BP5Serializer.PerformPuts(false); }

void BbstageWriter::EndStep()
{
    CheckDrainError();
    BbstageStagedFinalizer::Instance().CheckError(); // staged mode: earlier sessions
    m_BetweenStepPairs = false;

    MarshalAttributes();
    auto TSInfo = m_BP5Serializer.CloseTimestep(static_cast<int>(m_WriterStep), false);

    const size_t dataSize = TSInfo.DataBuffer->Size();

    uint64_t finalPos = 0;
    uint64_t nodeFilePos = 0;         // aggregate: this step's aligned node-file base
    std::vector<uint64_t> nodeSizes;  // aggregate: per-rank sizes, node-local rank order

    if (m_Aggregate)
    {
        // The node leader holds the node data sub-file's append position.
        // Align it and broadcast it to the node: every rank derives its own
        // intra-node offset from it, so the offsets are final at staging time
        // and the drain stays a pure byte copy (the bbstage invariant).
        nodeFilePos = m_DataPos;
        if (m_Parameters.StripeSize > 1)
        {
            nodeFilePos += helper::PaddingToAlignOffset(nodeFilePos, m_Parameters.StripeSize);
        }
        m_NodeComm.Bcast(&nodeFilePos, 1, 0);
        // Each rank contributes its step size; the node file lays the slices
        // out in global-rank order (== node-local rank order).
        nodeSizes.resize(static_cast<size_t>(m_NodeSize));
        const uint64_t mySize = dataSize;
        m_NodeComm.Allgather(&mySize, 1, nodeSizes.data(), 1);
        uint64_t prefix = 0;
        for (int l = 0; l < m_NodeSize; ++l)
        {
            if (l == m_NodeLocalRank)
            {
                finalPos = nodeFilePos + prefix;
            }
            prefix += nodeSizes[static_cast<size_t>(l)];
        }
        if (m_IsLeader)
        {
            m_DataPos = nodeFilePos + prefix; // prefix == this step's node total
            // Backpressure: the leader gates the whole node (its drain jobs
            // carry the node total); non-leaders rely on that gate (step
            // progression is collective). Durable mode needs no such guard.
            if (m_CloseModeStaged)
            {
                BbstageStagedFinalizer::Instance().AccountStaged(prefix);
            }
        }
    }
    else
    {
        // EveryoneWrites: each rank owns its sub-file and drains FIFO, so the
        // offset assigned here is final.
        finalPos = m_DataPos;
        if (m_Parameters.StripeSize > 1)
        {
            finalPos += helper::PaddingToAlignOffset(finalPos, m_Parameters.StripeSize);
        }
        // staged close: backpressure (per-rank). Durable mode needs no guard.
        if (m_CloseModeStaged)
        {
            BbstageStagedFinalizer::Instance().AccountStaged(dataSize);
        }
    }

    // Synchronous spill of the step payload to node-local SSD: the bbstage
    // critical path. Cheap, contiguous, local; memory is freed immediately.
    // (Unchanged by aggregate mode: everyone still stages its own file.)
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

    // Aggregate: the node leader's drain worker reads every rank's staging
    // file, so all of them must be written before the job is handed off. This
    // node-local barrier guarantees that (everyone on the node calls it).
    if (m_Aggregate)
    {
        m_NodeComm.Barrier();
    }

    if (!m_Aggregate)
    {
        m_DataPos = finalPos + dataSize;
    }

    // Hand off to the in-process drain workers (original bbview: xattr +
    // socket message to bbviewd).
    if (m_Aggregate)
    {
        // Only the leader enqueues: one job that drains every rank on the node
        // into the shared node sub-file. Non-leaders stage locally and let the
        // leader drain their files.
        if (m_IsLeader)
        {
            DrainJob job;
            job.Seq = m_NextDataSeq++;
            job.Step = m_WriterStep;
            uint64_t prefix = 0;
            for (int l = 0; l < m_NodeSize; ++l)
            {
                BbstageDrainMember mem;
                mem.StagingPath = m_StagingDir + "/step_r" +
                                  std::to_string(m_NodeMembers[static_cast<size_t>(l)]) + "_" +
                                  std::to_string(m_WriterStep);
                mem.FinalPos = nodeFilePos + prefix;
                mem.DataSize = nodeSizes[static_cast<size_t>(l)];
                prefix += nodeSizes[static_cast<size_t>(l)];
                job.Members.push_back(std::move(mem));
            }
            job.DataSize = prefix; // node total (drain watermark / progress)
            {
                std::lock_guard<std::mutex> lck(m_Mutex);
                m_DataQueue.push_back(std::move(job));
            }
            m_DataCV.notify_one();
        }
    }
    else
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
    // The step number is passed explicitly: ++m_WriterStep happens right
    // after this call, and the commit gate compares against step + 1.
    AggregateAndEnqueueCommit(TSInfo, finalPos, dataSize, m_WriterStep);

    ++m_WriterStep;
}

void BbstageWriter::Flush(const int /*transportIndex*/)
{
    if (m_CloseModeStaged)
    {
        helper::Throw<std::logic_error>("Engine", "BbstageWriter", "Flush",
                                        "Flush() is not supported in staged close mode: data "
                                        "is staged at EndStep() and finalized in the background "
                                        "after Close()");
    }
    if (m_BetweenStepPairs)
    {
        helper::Throw<std::logic_error>("Engine", "BbstageWriter", "Flush",
                                        "Flush() inside a step is not supported by the bbstage "
                                        "engine; call it between EndStep() and BeginStep()");
    }
    DrainAndCommitAll();
}

void BbstageWriter::NotifyEngineAttribute(std::string /*name*/, AttributeBase *Attr,
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

/* ---- drain machinery (daemonless stand-in for bbviewd) --------------------- */

void BbstageWriter::StartDrainThreads()
{
    // Aggregate mode: only the node leader drains (it copies every rank on the
    // node); non-leaders have no data to drain, so they start no workers.
    if (!(m_Aggregate && !m_IsLeader))
    {
        for (unsigned int i = 0; i < m_DrainThreads; ++i)
        {
            m_DrainWorkers.emplace_back(&BbstageWriter::WorkerLoop, this);
        }
    }
    if (m_RankMPI == 0)
    {
        m_CommitterThread = std::thread(&BbstageWriter::CommitterLoop, this);
    }
}

void BbstageWriter::StopDrainThreads() noexcept
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

void BbstageWriter::WorkerLoop()
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
                // Aggregate mode: this job drained every member of the node,
                // so publishing progress here means all of the node's data is
                // on the final store. This is the invariant the rank-0
                // committer relies on when it gates commits on node progress
                // records.
                // Publish under the lock so sequence numbers stay serialized
                // across drain threads; the update itself is one small
                // pwrite. m_PublishedSteps is only advanced after a
                // successful write, so a failing publish stays retryable.
                try
                {
                    WriteProgress(publishSteps, publishBytes);
                    m_PublishedSteps = publishSteps;
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

void BbstageWriter::RunDataJob(DrainJob &job)
{
    // Copy one staging file into the (node) data sub-file at its final offset.
    // In aggregate mode the leader calls this once per node member; otherwise
    // once for this rank's own slice.
    auto drainOne = [this](const std::string &stagingPath, uint64_t finalPos, size_t dataSize) {
        const int sfd = open(stagingPath.c_str(), O_RDONLY | O_CLOEXEC);
        if (sfd < 0)
        {
            ThrowPosix("open staging file for drain " + stagingPath);
        }
        // Verify the staging file size before draining: a file left short by
        // a crash (or overwritten by external cleanup) would otherwise
        // surface as a confusing short read mid-copy; fail fast with both
        // sizes instead.
        struct stat st;
        if (fstat(sfd, &st) != 0)
        {
            close(sfd);
            ThrowPosix("fstat staging file for drain " + stagingPath);
        }
        if (static_cast<size_t>(st.st_size) != dataSize)
        {
            close(sfd);
            helper::Throw<std::runtime_error>("Engine", "BbstageWriter", "RunDataJob",
                                              "staging file " + stagingPath + " has size " +
                                                  std::to_string(st.st_size) + ", expected " +
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
                helper::Throw<std::runtime_error>("Engine", "BbstageWriter", "RunDataJob",
                                                  "short read of staging file " + stagingPath +
                                                      " at " + std::to_string(inOff) + " (" +
                                                          std::to_string(remain) +
                                                              " bytes still due)");
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
            std::filesystem::remove(stagingPath, ec); // best effort
        }
    };

    if (!job.Members.empty())
    {
        for (auto &mem : job.Members)
        {
            drainOne(mem.StagingPath, mem.FinalPos, mem.DataSize);
        }
    }
    else
    {
        drainOne(job.StagingPath, job.FinalPos, job.DataSize);
    }

    if (m_Parameters.verbose > 2)
    {
        const size_t nSlices = job.Members.empty() ? 1 : job.Members.size();
        std::cout << "bbstage rank " << m_RankMPI << " drained step " << job.Step << " ("
                  << job.DataSize << " bytes, " << nSlices << " slice(s))\n";
    }
}

void BbstageWriter::CommitterLoop()
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

void BbstageWriter::RunCommitJob(const DrainJob &job)
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
        std::cout << "bbstage committed step " << job.Step << " metadata at md pos "
                  << " size " << job.MDRecord.size() << "\n";
    }
}

void BbstageWriter::WaitForAllRanksDrained(size_t step)
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
                helper::Throw<std::logic_error>("Engine", "BbstageWriter",
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
            helper::Throw<std::runtime_error>("Engine", "BbstageWriter",
                                              "WaitForAllRanksDrained",
                                              "timeout waiting for own drain of step " +
                                                  std::to_string(step));
        }
        std::this_thread::sleep_for(poll);
    }

    // remote peers via their atomically-updated progress files. Aggregate
    // mode: one progress record per node (bbstage.drain.<nodeId>), so gate on
    // the other nodes; otherwise one per writer rank. This rank (0) is always
    // its node's leader, so its own node is covered by the in-memory check above.
    const int size = m_Aggregate ? m_NodeCount : m_Comm.Size();
    for (int r = 1; r < size; ++r)
    {
        const std::string path = m_Name + "/bbstage.drain." + std::to_string(r);
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
            if (BbstageReadProgress(path, p) && p.DrainedSteps >= step + 1)
            {
                break;
            }
            {
                std::lock_guard<std::mutex> lck(m_Mutex);
                if (m_Finishing)
                {
                    helper::Throw<std::logic_error>("Engine", "BbstageWriter",
                                                    "WaitForAllRanksDrained",
                                                    "engine is closing; commit of step " +
                                                        std::to_string(step) + " abandoned");
                }
            }
            if (!forever && (std::chrono::steady_clock::now() - start) > timeout)
            {
                helper::Throw<std::runtime_error>("Engine", "BbstageWriter",
                                                  "WaitForAllRanksDrained",
                                                  "timeout waiting for drain of step " +
                                                      std::to_string(step) + " on rank " +
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

void BbstageWriter::WriteProgress(uint64_t drainedSteps, uint64_t drainedBytes)
{
    // Called under m_Mutex, so no two threads can race the lazy create. The
    // open is deliberately off the open critical path: it happens on a drain
    // thread after the first drained step, mid-run.
    if (m_ProgressFd < 0)
    {
        m_ProgressFd =
            open(m_ProgressPath.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (m_ProgressFd < 0)
        {
            ThrowPosix("create progress file " + m_ProgressPath + " (rank " +
                       std::to_string(m_RankMPI) + ", host " + m_Host + ")");
        }
    }
    BbstageWriteProgressRecord(m_ProgressFd, m_ProgressPath, static_cast<uint64_t>(m_RankMPI),
                              drainedSteps, drainedBytes, m_ProgressSeq, m_Host);
}

void BbstageWriter::CheckDrainError()
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

void BbstageWriter::DrainAndCommitAll()
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

void BbstageWriter::AggregateAndEnqueueCommit(format::BP5Serializer::TimestepInfo &TSInfo,
                                             uint64_t finalPos, size_t dataSize, size_t step)
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
        helper::Throw<std::logic_error>("Engine", "BbstageWriter", "AggregateAndEnqueueCommit",
                                        "expected " + std::to_string(size) +
                                            " writer data positions, got " +
                                            std::to_string(writerDataPos.size()));
    }

    DrainJob job;
    job.Step = step;

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

void BbstageWriter::MakeBbHeader(std::vector<char> &buffer, size_t &position,
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
        position += m_VersionTagLength;
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

void BbstageWriter::WriteIndexHeaderAndWriterMap()
{
    // md.idx: 64-byte Index Table header + a static WriterMapRecord mapping
    // every writer rank to its own data sub-file (EveryoneWrites style).
    std::vector<char> buf;
    size_t pos = 0;
    MakeBbHeader(buf, pos, "Index Table", true);

    const uint64_t W = static_cast<uint64_t>(m_Comm.Size());
    // payload: length field + WriterCount + AggregatorCount + SubStreams + W map
    // entries (the record's length value counts 3+W payload uint64s, matching
    // BP5Writer::WriteMetadataFileIndex). Aggregate mode routes every writer to
    // its node's sub-file (SubStreams = node count); otherwise each writer maps
    // to its own sub-file (EveryoneWrites).
    const uint64_t subStreams = m_Aggregate ? static_cast<uint64_t>(m_NodeCount) : W;
    const size_t recSize = 1 + (4 + W) * sizeof(uint64_t);
    buf.resize(buf.size() + recSize);
    const unsigned char record = static_cast<unsigned char>(WriterMapRecord);
    helper::CopyToBuffer(buf, pos, &record, 1);
    uint64_t d = (3 + W) * sizeof(uint64_t);
    helper::CopyToBuffer(buf, pos, &d);
    helper::CopyToBuffer(buf, pos, &W);              // WriterCount
    helper::CopyToBuffer(buf, pos, &W);              // AggregatorCount == writers
    helper::CopyToBuffer(buf, pos, &subStreams);     // SubStreams
    for (uint64_t w = 0; w < W; ++w)
    {
        const uint64_t sub = m_Aggregate ? m_NodeIndexPerRank[w] : w;
        helper::CopyToBuffer(buf, pos, &sub); // writer w -> sub-file
    }
    m_MDIndexFile->Write(buf.data(), buf.size());
    m_MDIndexFile->Flush();
}

/* ---- put ----------------------------------------------------------------- */

void BbstageWriter::PutCommon(VariableBase &variable, const void *values, bool sync)
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
        helper::Throw<std::invalid_argument>("Engine", "BbstageWriter", "PutCommon",
                                             "Put with MemoryCount/MemoryStart sub-blocks is "
                                             "not supported by the bbstage engine");
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
void BbstageWriter::PutCommonSpan(Variable<T> &variable, typename Variable<T>::Span &span,
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

void BbstageWriter::MarshalAttributes()
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

void BbstageWriter::DoClose(const int /*transportIndex*/)
{
    if (!m_IsOpen)
    {
        return;
    }

    // Opt-in per-phase Close timing (BBSTAGE_TIMING=1); see
    // BbstageTimingEnabled().
    const bool doTimingClose = BbstageTimingEnabled();
    const auto tCloseStart = std::chrono::steady_clock::now();
    const auto closePhase = [this, tCloseStart, doTimingClose](const char *phase) {
        if (!doTimingClose)
        {
            return;
        }
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - tCloseStart)
                              .count();
        std::cerr << "bbstage rank " << m_RankMPI << " close-phase " << phase << " @"
                  << static_cast<long long>(ms) << " ms\n";
        std::cerr.flush();
    };

    if (m_CloseModeStaged)
    {
        // Staged close: the application-visible checkpoint cost ends here.
        // The session (undrained staging files, data sub-file fd, metadata
        // transports, commit queue) is handed to the process-level finalizer,
        // which drains to the final store and commits the metadata in the
        // background while the application computes. The finalizer's
        // destructor (clean process exit) is the durability barrier; a crash
        // loses the sessions that were not finalized yet (no bbstage.final
        // marker).
        BbstageStagedFinalizer::Instance().CheckError();

        BbstageStagedFinalizer::Session session;
        session.FinalDir = m_Name;
        session.DataFileName = m_DataFileName;
        session.StagingDir = m_StagingDir;
        session.ProgressPath = m_ProgressPath;
        session.Host = m_Host;
        session.Rank = m_RankMPI;
        session.WriterCount = m_Comm.Size();
        session.Verbose = m_Parameters.verbose;
        session.Aggregate = m_Aggregate;
        session.NodeCount = m_NodeCount;
        session.DataFd = m_DataFd;
        m_DataFd = -1;
        session.ProgressFd = m_ProgressFd;
        m_ProgressFd = -1;
        session.ProgressSeq = m_ProgressSeq;
        session.MDFile = std::move(m_MDFile);
        session.MMDFile = std::move(m_MMDFile);
        session.MDIndexFile = std::move(m_MDIndexFile);
        {
            std::lock_guard<std::mutex> lck(m_Mutex);
            session.DataQueue = std::move(m_DataQueue);
            session.CommitQueue = std::move(m_CommitQueue);
            session.NextDataSeq = m_NextDataSeq;
            session.DrainedWatermark = m_DrainedWatermark;
            session.PublishedSteps = m_PublishedSteps;
            session.ProgressBytes = m_ProgressBytes;
            session.DoneDataSeqs = std::move(m_DoneDataSeqs);
            session.CommitEnqueued = m_CommitEnqueued;
            session.CommitDone = m_CommitDone;
        }
        session.DrainDataSync = m_DrainDataSync;
        session.DeleteStagedFiles = m_DeleteStagedFiles;
        session.CommitPollMs = m_CommitPollMs;
        session.CommitTimeoutSecs = m_CommitTimeoutSecs;
        BbstageStagedFinalizer::Instance().RegisterSession(std::move(session));
        m_StagingDir.clear(); // owned by the finalizer now
        closePhase("staged-handoff");
        m_Comm.Barrier();
        closePhase("staged-barrier");
        m_IsOpen = false;
        return;
    }

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

void BbstageWriter::DestructorClose(bool /*Verbose*/) noexcept
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
    void BbstageWriter::DoPutSync(Variable<T> &variable, const T *data)                             \
    {                                                                                              \
        PutCommon(variable, data, true);                                                           \
    }                                                                                              \
    void BbstageWriter::DoPutDeferred(Variable<T> &variable, const T *data)                         \
    {                                                                                              \
        PutCommon(variable, data, false);                                                          \
    }                                                                                              \
    size_t BbstageWriter::PutCount(Variable<T> &variable)                                           \
    {                                                                                              \
        return m_BP5Serializer.PutCount((void *)&variable);                                        \
    }

ADIOS2_FOREACH_STDTYPE_1ARG(declare_type)
#undef declare_type

#define declare_type(T)                                                                            \
    void BbstageWriter::DoPut(Variable<T> &variable, typename Variable<T>::Span &span,              \
                             const bool initialize, const T &value)                                \
    {                                                                                              \
        PutCommonSpan(variable, span, initialize, value);                                          \
    }

ADIOS2_FOREACH_PRIMITIVE_STDTYPE_1ARG(declare_type)
#undef declare_type

#define declare_type(T, L)                                                                         \
    T *BbstageWriter::DoBufferData_##L(const int bufferIdx, const size_t payloadPosition,           \
                                      const size_t bufferID) noexcept                              \
    {                                                                                              \
        return reinterpret_cast<T *>(m_BP5Serializer.GetPtr(bufferIdx, payloadPosition));          \
    }

ADIOS2_FOREACH_PRIMITVE_STDTYPE_2ARGS(declare_type)
#undef declare_type

void BbstageWriter::DoPutStructSync(VariableStruct &variable, const void *data)
{
    PutCommon(variable, data, true);
}

void BbstageWriter::DoPutStructDeferred(VariableStruct &variable, const void *data)
{
    PutCommon(variable, data, false);
}

} // end namespace engine
} // end namespace core
} // end namespace adios2
