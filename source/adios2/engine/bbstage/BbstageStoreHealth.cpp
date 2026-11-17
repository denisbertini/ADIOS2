/*
 * SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "BbstageStoreHealth.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace adios2::core::engine;
using namespace std::chrono_literals;

namespace
{
// one probe round-trip carries this many bytes through the store
constexpr size_t ProbeBytes = 8;

int64_t SteadyNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const char *ErrName(int err)
{
    const char *s = std::strerror(err);
    return s != nullptr ? s : "unknown error";
}
} // namespace

BbstageStoreHealth &BbstageStoreHealth::Instance()
{
    static BbstageStoreHealth instance;
    return instance;
}

BbstageStoreHealth::~BbstageStoreHealth()
{
    m_StoreCV.notify_all();
    m_Stop.store(true);
    if (m_ProbeThread.joinable())
    {
        m_ProbeThread.join(); // a probe stuck in D state hangs teardown with
                              // the store, exactly as the pending drain does:
                              // consistent with the staged durability barrier
    }
    if (m_SentinelFd >= 0)
    {
        ::close(m_SentinelFd);
    }
}

void BbstageStoreHealth::ResetForTest()
{
    m_Stop.store(true);
    m_StoreCV.notify_all();
    if (m_ProbeThread.joinable())
    {
        m_ProbeThread.join();
    }
    if (m_SentinelFd >= 0)
    {
        ::close(m_SentinelFd);
        m_SentinelFd = -1;
    }
    m_Config = Config();
    m_Probe = nullptr;
    m_LastError.clear();
    m_LastProbeEndNs.store(0);
    m_BadSinceNs.store(0);
    m_ConsecutiveOk.store(0);
    m_LatencyEwmaNs.store(0);
    m_Down.store(0);
    m_DownSinceNs.store(0);
    m_ProbeIntervalNs.store(0);
    m_Stop.store(false);
}

bool BbstageStoreHealth::RoundTripFd(int fd, int &err)
{
    // pwrite -> fdatasync -> pread: metadata-free but real I/O. fdatasync is
    // the part that cannot be answered from the client page cache, so a
    // store whose servers stopped answering blocks the probe the same way
    // it blocks the drain.
    uint8_t buf[ProbeBytes];
    for (size_t i = 0; i < ProbeBytes; ++i)
    {
        buf[i] = static_cast<uint8_t>(0xA5 ^ i);
    }
    if (::pwrite(fd, buf, ProbeBytes, 0) != static_cast<ssize_t>(ProbeBytes))
    {
        err = errno;
        return false;
    }
    if (::fdatasync(fd) != 0)
    {
        err = errno;
        return false;
    }
    uint8_t back[ProbeBytes];
    if (::pread(fd, back, ProbeBytes, 0) != static_cast<ssize_t>(ProbeBytes))
    {
        err = errno;
        return false;
    }
    if (std::memcmp(buf, back, ProbeBytes) != 0)
    {
        err = EIO;
        return false;
    }
    return true;
}

bool BbstageStoreHealth::DefaultProbe(int &err)
{
    // The sentinel can be unlinked under us: a fresh engine Open of the same
    // checkpoint name clears untracked files from the final directory
    // (CleanupBPDirectory). Writes to a deleted inode are answered from the
    // client and stop proving anything about the store — so notice it and
    // recreate the file. One fstat on one open fd per probe is not the
    // per-probe metadata churn the persistent-sentinel design avoids.
    struct stat st;
    if (::fstat(m_SentinelFd, &st) == 0 && st.st_nlink == 0)
    {
        ::close(m_SentinelFd);
        m_SentinelFd = ::open(m_Config.ProbePath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC,
                              static_cast<mode_t>(0600));
        if (m_SentinelFd < 0)
        {
            err = errno;
            return false;
        }
    }
    return RoundTripFd(m_SentinelFd, err);
}

bool BbstageStoreHealth::Configure(const Config &cfg)
{
    if (m_Config.Enabled)
    {
        return true; // first configuration wins: one final store per process
    }
    if (!cfg.Enabled)
    {
        return true; // inert
    }
    if (cfg.ProbePath.empty())
    {
        m_LastError = "StoreBreaker=on requires a probe path (final directory)";
        return false;
    }
    m_Config = cfg;
    if (m_Probe)
    {
        // test-injected probe (CI cannot hang a filesystem): same contract,
        // same preflight discipline as the real one
        int err = 0;
        if (!m_Probe(err))
        {
            m_LastError = "breaker probe preflight failed: " + std::string(ErrName(err));
            m_Config.Enabled = false;
            return false;
        }
    }
    else
    {
        // persistent sentinel: opened once, probed with data I/O only, so
        // healthy-state load is one fd's worth of pwrite/fdatasync/pread
        // (no per-probe create/unlink metadata churn on the MDT)
        const int fd = ::open(cfg.ProbePath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC,
                              static_cast<mode_t>(0600));
        if (fd < 0)
        {
            m_LastError = "cannot open breaker probe file " + cfg.ProbePath + ": " +
                          ErrName(errno);
            m_Config.Enabled = false;
            return false;
        }
        m_SentinelFd = fd;
        int err = 0;
        const bool ok = RoundTripFd(fd, err);
        if (!ok)
        {
            m_LastError = "breaker probe round-trip on " + cfg.ProbePath + ": " + ErrName(err);
            ::close(fd);
            m_SentinelFd = -1;
            m_Config.Enabled = false;
            return false;
        }
    }
    m_ProbeIntervalNs.store(static_cast<int64_t>(cfg.ProbeIntervalSecs * 1e9));
    m_LastProbeEndNs.store(SteadyNs()); // warm-up grace before stale-heartbeat trips
    m_ProbeThread = std::thread(&BbstageStoreHealth::ProbeLoop, this);
    return true;
}

BbstageStoreHealthState BbstageStoreHealth::State() const
{
    if (!m_Config.Enabled)
    {
        return BbstageStoreHealthState::Healthy;
    }
    const int64_t now = SteadyNs();

    // Stale probe heartbeat: the probe thread is either gone or (on a real
    // stall) blocked in the kernel like everyone else. This is the only
    // visible trace of a hung store, and it is deliberately evaluated in
    // the consumer, where nobody is stuck.
    const int64_t lastEnd = m_LastProbeEndNs.load();
    const int64_t interval = m_ProbeIntervalNs.load();
    const int64_t downAfterNs = static_cast<int64_t>(m_Config.DownAfterSecs * 1e9);
    const bool heartbeatStale =
        (lastEnd != 0) && (now - lastEnd > downAfterNs + 4 * interval);

    if (m_Down.load() == 0)
    {
        const int64_t badSince = m_BadSinceNs.load();
        const bool errorStreak = (badSince != 0) && (now - badSince > downAfterNs);
        if (heartbeatStale || errorStreak)
        {
            int expected = 0;
            if (m_Down.compare_exchange_strong(expected, 1))
            {
                m_DownSinceNs.store(now);
                std::fprintf(stderr,
                             "[bbstage breaker rank=%d] final store DOWN (%s; will stop "
                             "issuing store writes and hold checkpoints on staging)\n",
                             m_Config.Rank, heartbeatStale ? "probe heartbeat stale"
                                                           : "store errors sustained");
                std::fflush(stderr);
            }
        }
    }
    if (m_Down.load() != 0)
    {
        // Leaving Down requires UpAfterProbes consecutive clean round-trips:
        // one lucky fast answer on a flapping store must not reopen the flood
        // gates. The probe thread performs the release when the count is
        // reached; consumers never clear the flag themselves.
        return BbstageStoreHealthState::Down;
    }
    const int64_t ewma = m_LatencyEwmaNs.load();
    if (ewma > static_cast<int64_t>(m_Config.SlowAfterSecs * 1e9))
    {
        return BbstageStoreHealthState::Slow;
    }
    return BbstageStoreHealthState::Healthy;
}

void BbstageStoreHealth::ObserveFailure()
{
    if (!m_Config.Enabled)
    {
        return;
    }
    int64_t expected = 0;
    m_BadSinceNs.compare_exchange_strong(expected, SteadyNs());
    m_StoreCV.notify_all(); // let gated workers re-evaluate promptly
}

void BbstageStoreHealth::ProbeLoop()
{
    while (!m_Stop.load())
    {
        const int64_t interval = m_ProbeIntervalNs.load();
        {
            std::unique_lock<std::mutex> lck(m_Mutex);
            m_StoreCV.wait_for(lck, std::chrono::nanoseconds(interval),
                               [this] { return m_Stop.load(); });
        }
        if (m_Stop.load())
        {
            return;
        }

        const int64_t t0 = SteadyNs();
        int err = 0;
        const bool ok = (m_Probe ? m_Probe(err) : DefaultProbe(err));
        const int64_t dur = SteadyNs() - t0;

        m_LastProbeEndNs.store(SteadyNs()); // heartbeat advances on failure too:
                                           // a probe that COMPLETED and got an
                                           // error proves the thread is alive
        if (ok)
        {
            // EWMA with 1/8 step: a single fast answer on a store that was
            // slow for minutes cannot instantly erase the Slow evidence
            const int64_t prev = m_LatencyEwmaNs.load();
            m_LatencyEwmaNs.store(prev - prev / 8 + dur / 8);
            m_BadSinceNs.store(0);
            const int okNow = m_ConsecutiveOk.fetch_add(1) + 1;
            // Release only on the quorum: a single lucky fast answer on a
            // flapping store must not reopen the flood gates, and the flag
            // is never released-then-reset (a consumer must not glimpse a
            // false UP between the two).
            if (m_Down.load() != 0 && static_cast<unsigned int>(okNow) >= m_Config.UpAfterProbes)
            {
                int expected = 1;
                if (m_Down.compare_exchange_strong(expected, 0))
                {
                    const int64_t downSince = m_DownSinceNs.exchange(0);
                    std::fprintf(stderr,
                                 "[bbstage breaker rank=%d] final store back UP after %.1f s "
                                 "(resuming drain at full rate)\n",
                                 m_Config.Rank,
                                 static_cast<double>(SteadyNs() - downSince) / 1e9);
                    std::fflush(stderr);
                }
            }
        }
        else
        {
            m_ConsecutiveOk.store(0);
            int64_t expected = 0;
            m_BadSinceNs.compare_exchange_strong(expected, SteadyNs());
            const int64_t badSince = m_BadSinceNs.load();
            const int64_t downAfterNs = static_cast<int64_t>(m_Config.DownAfterSecs * 1e9);
            if ((SteadyNs() - badSince) > downAfterNs)
            {
                int expectedDown = 0;
                if (m_Down.compare_exchange_strong(expectedDown, 1))
                {
                    m_DownSinceNs.store(SteadyNs());
                    std::fprintf(stderr,
                                 "[bbstage breaker rank=%d] final store DOWN (probe error: %s; "
                                 "will stop issuing store writes and hold checkpoints on "
                                 "staging)\n",
                                 m_Config.Rank, ErrName(err));
                    std::fflush(stderr);
                }
            }
        }

        // Cadence: exponential backoff while Down (a sick store does not
        // need retry storms), reset to the healthy interval otherwise.
        const int64_t base = static_cast<int64_t>(m_Config.ProbeIntervalSecs * 1e9);
        const int64_t cap = static_cast<int64_t>(m_Config.MaxProbeIntervalSecs * 1e9);
        const int64_t next =
            (m_Down.load() != 0) ? std::min<int64_t>(interval * 2, cap) : base;
        m_ProbeIntervalNs.store(next > 0 ? next : base);

        m_StoreCV.notify_all();
    }
}
