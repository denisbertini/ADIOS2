/*
 * SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ADIOS2_ENGINE_BBSTAGE_BBSTAGESTOREHEALTH_H_
#define ADIOS2_ENGINE_BBSTAGE_BBSTAGESTOREHEALTH_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace adios2
{
namespace core
{
namespace engine
{

/** Final-store health oracle for the bbstage store circuit breaker
 * (StoreBreaker=on; default off: everything below is inert).
 *
 * Motivation (field): a whole-filesystem Lustre stall turns the staged
 * drain into a pile of writers hammering a sick store while the simulation
 * blocks at the staging watermark. The breaker lets bbstage NOTICE that
 * situation, stop issuing new final-store writes (load shedding), keep the
 * due checkpoints on the staging NVMe, and resume the drain the moment the
 * store is back — instead of paying either a false-positive timeout abort of
 * a healthy-but-stalled run or an unbounded hang.
 *
 * The oracle combines two signals, so a single sensor can never trip it:
 *  - an active probe: a persistent sentinel file on the final store,
 *    round-tripped (pwrite + fdatasync + pread) by a dedicated thread with
 *    exponential backoff while down;
 *  - passive drain telemetry: failed drain jobs feed the same error window
 *    (a probe on a small file can look fine while multi-GiB writes crawl,
 *    and vice versa).
 *
 * D-state rule: on a stalled store the probe's own syscalls block in the
 * kernel and cannot be timed out or killed, so liveness is measured by the
 * PROBE THREAD'S HEARTBEAT, not by a syscall return: consumers derive "Down"
 * when the heartbeat went stale. For the same reason every field a consumer
 * reads is an atomic — the probe thread never holds a lock across a store
 * operation, and waiters poll on a bounded interval so a stuck probe thread
 * (unable to notify anyone) still resolves in the consumers, not in the
 * victim.
 */
enum class BbstageStoreHealthState
{
    Healthy,  ///< store answering probes within budget
    Slow,     ///< alive, but probe latency EWMA beyond the budget (DEGRADED)
    Down      ///< stop issuing new final-store writes
};

class BbstageStoreHealth
{
public:
    struct Config
    {
        bool Enabled = false;
        std::string ProbePath;            ///< sentinel file on the final store
        float SlowAfterSecs = 5.f;        ///< probe latency EWMA above this -> Slow
        float DownAfterSecs = 30.f;       ///< sustained probe errors / stale
                                          ///heartbeat above this -> Down
        unsigned int UpAfterProbes = 3;   ///< consecutive clean probes to leave Down
        float ProbeIntervalSecs = 1.f;    ///< healthy cadence; backs off while Down
        float MaxProbeIntervalSecs = 15.f;
        int Rank = -1; ///< log tag only
    };

    static BbstageStoreHealth &Instance();

    // Singleton plus unit tests (the injected probe avoids a hung filesystem
    // in CI: the fake is called with the same contract as the real one).
    BbstageStoreHealth() = default;

    /** One-time enable. Runs a synchronous preflight round-trip of the
     *  sentinel: an unusable probe path is a configuration error and must
     *  surface at Open, after which every probe failure is weather. On
     *  failure, returns false and sets LastError(); the breaker stays off.
     *  Later calls while already enabled are no-ops (first configuration
     *  wins: a process writes to one final store). */
    bool Configure(const Config &cfg);

    const std::string &LastError() const { return m_LastError; }
    const Config &GetConfig() const { return m_Config; }
    bool Enabled() const { return m_Config.Enabled; }

    /** Current health. Cheap: pure atomic reads, safe from any thread —
     *  including while the probe thread itself is blocked in the kernel. */
    BbstageStoreHealthState State() const;

    /** Drain-worker gate: true when new final-store writes may be issued.
     *  Returns true immediately when the breaker is off (fast path). */
    bool StoreReady() const
    {
        return !m_Config.Enabled || State() != BbstageStoreHealthState::Down;
    }

    /** Passive telemetry from either drain executor: a failed store write
     *  opens/advances the error window exactly like a failed probe (it is
     *  the same store refusing us). Successful jobs are ignored: their
     *  multi-GiB transfer time measures our own concurrency, not the
     *  store's responsiveness. */
    void ObserveFailure();

    ~BbstageStoreHealth();
    BbstageStoreHealth(const BbstageStoreHealth &) = delete;
    BbstageStoreHealth &operator=(const BbstageStoreHealth &) = delete;

    /** Test hook: must be set before Configure(). Returns false with errno
     *  set on failure, like the real round-trip. */
    using ProbeFn = std::function<bool(int &err)>;
    void SetProbeForTest(ProbeFn fn) { m_Probe = std::move(fn); }

    /** Test hook: returns the instance to a pristine, disabled state so
     *  sequential tests can re-Configure the process singleton. Must not be
     *  called while the probe may be blocked in the kernel (tests never). */
    void ResetForTest();

private:
    void ProbeLoop();
    /** Real round-trip on the live sentinel fd; recreates the file when an
     *  Open-time directory cleanup of the same checkpoint name unlinked it
     *  (writes to a deleted inode are answered client-side and would stop
     *  measuring the store). */
    bool DefaultProbe(int &err);
    static bool RoundTripFd(int fd, int &err);
    int64_t NowNs() const;

    Config m_Config;
    ProbeFn m_Probe;
    std::string m_LastError;
    int m_SentinelFd = -1;

    // Lock-free consumer state (see class comment): a probe thread stuck in
    // D state holds no lock, and stale heartbeat is itself a Down signal.
    // Mutable: State() is a cheap const query that also performs the
    // (CAS-guarded) health transitions and their one-line logging.
    mutable std::atomic<int64_t> m_LastProbeEndNs{0};  ///< probe-thread heartbeat
    mutable std::atomic<int64_t> m_BadSinceNs{0};      ///< start of error streak, 0=none
    mutable std::atomic<int> m_ConsecutiveOk{0};       ///< clean probes since last failure
    mutable std::atomic<int64_t> m_LatencyEwmaNs{0};   ///< probe latency, 1/8 step
    mutable std::atomic<int> m_Down{0};                ///< sticky until UpAfterProbes
    mutable std::atomic<int64_t> m_DownSinceNs{0};     ///< for the episode log line
    mutable std::atomic<int64_t> m_ProbeIntervalNs{0}; ///< current backoff cadence

    std::mutex m_Mutex;                   ///< CV handshake only, never across
    std::condition_variable m_StoreCV;    ///< a store operation

    std::thread m_ProbeThread;
    std::atomic<bool> m_Stop{false};
};

} // end namespace engine
} // end namespace core
} // end namespace adios2

#endif // ADIOS2_ENGINE_BBSTAGE_BBSTAGESTOREHEALTH_H_
