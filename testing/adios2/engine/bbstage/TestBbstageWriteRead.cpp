/*
 * SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <adios2.h>
#if ADIOS2_USE_MPI
#include <mpi.h>
#endif

#include "adios2/engine/bbstage/BbstageStagedFinalizer.h"
#include "adios2/engine/bbstage/BbstageStoreHealth.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{

const std::string TestName = "BbstageTest.bp";
const std::string StagingDir = "bbstage-staging-test";

std::vector<float> MakeStep(size_t step, size_t n)
{
    std::vector<float> v(n);
    std::iota(v.begin(), v.end(), static_cast<float>(step) * 1000.f);
    return v;
}

// the full global block of a step: every rank contributes identical local data
// at disjoint offsets, so the global step is that local block repeated once
// per rank
std::vector<float> MakeGlobalStep(size_t step, size_t n, int size)
{
    std::vector<float> all;
    for (int r = 0; r < size; ++r)
    {
        auto v = MakeStep(step, n);
        all.insert(all.end(), v.begin(), v.end());
    }
    return all;
}

int MpiRank()
{
#if ADIOS2_USE_MPI
    int rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    return rank;
#else
    return 0;
#endif
}

int MpiSize()
{
#if ADIOS2_USE_MPI
    int size = 1;
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    return size;
#else
    return 1;
#endif
}

class EngineBbstage : public ::testing::Test
{
public:
    EngineBbstage() = default;
    ~EngineBbstage() override {}

protected:
    void SetUp() override
    {
        std::error_code ec;
        std::filesystem::remove_all(TestName, ec);
        std::filesystem::remove_all(StagingDir, ec);
#if ADIOS2_USE_MPI
        // all ranks must finish removing before any rank re-creates
        MPI_Barrier(MPI_COMM_WORLD);
#endif
    }
    void TearDown() override
    {
#if ADIOS2_USE_MPI
        MPI_Barrier(MPI_COMM_WORLD);
#endif
    }

#if ADIOS2_USE_MPI
    adios2::ADIOS m_Adios{MPI_COMM_WORLD};
#else
    adios2::ADIOS m_Adios;
#endif
};

} // end anonymous namespace

// Write with the bbstage engine (steps land on the node-local staging tier and
// drain in the background), close drains everything, then read back with the
// standard bp5 engine to prove full output compatibility.
TEST_F(EngineBbstage, WriteDrainReadBackBP5)
{
    constexpr size_t n = 100;
    constexpr int nSteps = 3;
    const int rank = MpiRank();
    const int size = MpiSize();
    const size_t globalN = n * static_cast<size_t>(size);

    {
        adios2::IO io = m_Adios.DeclareIO("bbstage-writer");
        io.SetEngine("bbstage");
        io.SetParameters({{"BurstBufferPath", StagingDir}, {"verbose", "0"}});

        // each rank owns a disjoint block of the global array
        auto gvar = io.DefineVariable<float>("gdata", {globalN},
                                             {static_cast<size_t>(rank) * n}, {n});
        io.DefineAttribute<std::string>("written_by", "bbstage-engine");

        adios2::Engine engine = io.Open(TestName, adios2::Mode::Write);
        for (int s = 0; s < nSteps; ++s)
        {
            auto v = MakeStep(s, n);
            engine.BeginStep();
            engine.Put<float>(gvar, v.data());
            engine.EndStep();
        }
        engine.Close();
    }
#if ADIOS2_USE_MPI
    MPI_Barrier(MPI_COMM_WORLD); // every writer must be done before inspection
#endif

    // after Close the burst buffer must be empty and drain markers removed
    {
        ASSERT_TRUE(std::filesystem::exists(TestName));
        auto mdidx = std::filesystem::path(TestName) / "md.idx";
        ASSERT_TRUE(std::filesystem::exists(mdidx));
        for (const auto &e : std::filesystem::directory_iterator(TestName))
        {
            EXPECT_EQ(std::string(e.path().filename()).rfind("bbstage.drain.", 0),
                      std::string::npos)
                << "stale drain progress file left behind: " << e.path();
        }
        if (std::filesystem::exists(StagingDir))
        {
            for (const auto &e : std::filesystem::recursive_directory_iterator(StagingDir))
            {
                FAIL() << "staged file not drained/removed: " << e.path();
            }
        }
    }

    // read back with the stock bp5 engine, random access
    {
        adios2::IO io = m_Adios.DeclareIO("bp5-reader");
        io.SetEngine("bp5");
        adios2::Engine reader = io.Open(TestName, adios2::Mode::ReadRandomAccess);
        EXPECT_EQ(reader.Steps(), static_cast<size_t>(nSteps));
        auto gvar = io.InquireVariable<float>("gdata");
        ASSERT_TRUE(static_cast<bool>(gvar));
        auto attr = io.InquireAttribute<std::string>("written_by");
        ASSERT_TRUE(static_cast<bool>(attr));
        EXPECT_EQ(attr.Data()[0], "bbstage-engine");

        for (size_t s = 0; s < static_cast<size_t>(nSteps); ++s)
        {
            auto expected = MakeGlobalStep(s, n, size);
            std::vector<float> got(globalN);
            gvar.SetStepSelection({s, 1});
            gvar.SetSelection({{0}, {globalN}});
            reader.Get<float>(gvar, got.data(), adios2::Mode::Sync);
            EXPECT_EQ(got, expected) << "step " << s;
        }
        reader.Close();
    }

    // and with engine "bbstage" in read mode (registers the BP5 reader)
    {
        adios2::IO io = m_Adios.DeclareIO("bbstage-reader");
        io.SetEngine("bbstage");
        adios2::Engine reader = io.Open(TestName, adios2::Mode::ReadRandomAccess);
        EXPECT_EQ(reader.Steps(), static_cast<size_t>(nSteps));
        reader.Close();
    }
}

// Streaming read: the engine only commits metadata after the step data has
// drained from the burst buffer to the final store, so every step a streaming
// reader observes is complete.
TEST_F(EngineBbstage, StreamingReadSeesOnlyDrainedSteps)
{
    constexpr size_t n = 50;
    constexpr int nSteps = 4;
    const int rank = MpiRank();
    const int size = MpiSize();
    const size_t globalN = n * static_cast<size_t>(size);

    adios2::IO wio = m_Adios.DeclareIO("bbstage-stream-writer");
    wio.SetEngine("bbstage");
    wio.SetParameters({{"BurstBufferPath", StagingDir}});
    auto wvar = wio.DefineVariable<float>("gdata", {globalN},
                                          {static_cast<size_t>(rank) * n}, {n});

    adios2::Engine writer = wio.Open(TestName, adios2::Mode::Write);

    adios2::IO rio = m_Adios.DeclareIO("bbstage-stream-reader");
    rio.SetEngine("bp5");
    rio.SetParameters({{"OpenTimeoutSecs", "60"}, {"BeginStepPollingFrequencySecs", "0.05"}});
    adios2::Engine reader = rio.Open(TestName, adios2::Mode::Read);

    for (int s = 0; s < nSteps; ++s)
    {
        auto v = MakeStep(s, n);
        writer.BeginStep();
        writer.Put<float>(wvar, v.data());
        writer.EndStep();

        auto status = reader.BeginStep();
        ASSERT_EQ(status, adios2::StepStatus::OK) << "step " << s;
        auto rvar = rio.InquireVariable<float>("gdata");
        ASSERT_TRUE(static_cast<bool>(rvar));
        rvar.SetSelection({{0}, {globalN}});
        std::vector<float> got(globalN);
        reader.Get<float>(rvar, got.data(), adios2::Mode::Sync);
        EXPECT_EQ(got, MakeGlobalStep(static_cast<size_t>(s), n, size)) << "step " << s;
        reader.EndStep();
    }
    writer.Close();
    EXPECT_EQ(reader.BeginStep(), adios2::StepStatus::EndOfStream);
    reader.Close();
}

// Aggregate drain (Aggregate=on): local staging stays per-rank, but the
// SSD -> final-store drain is aggregated per node into a single data.<node>
// sub-file. On a single node (the usual test case) that is one data file for
// all ranks instead of one per rank. The stock bp5 reader follows the writer
// map to the node sub-file and must read back byte-identical data.
TEST_F(EngineBbstage, AggregateDrainReadBackBP5)
{
    constexpr size_t n = 100;
    constexpr int nSteps = 3;
    const int rank = MpiRank();
    const int size = MpiSize();
    const size_t globalN = n * static_cast<size_t>(size);

    {
        adios2::IO io = m_Adios.DeclareIO("bbstage-agg-writer");
        io.SetEngine("bbstage");
        io.SetParameters({{"BurstBufferPath", StagingDir}, {"Aggregate", "on"},
                          {"verbose", "0"}});

        // each rank owns a disjoint block of the global array
        auto gvar = io.DefineVariable<float>("gdata", {globalN},
                                             {static_cast<size_t>(rank) * n}, {n});
        io.DefineAttribute<std::string>("written_by", "bbstage-agg-engine");

        adios2::Engine engine = io.Open(TestName, adios2::Mode::Write);
        for (int s = 0; s < nSteps; ++s)
        {
            auto v = MakeStep(s, n);
            engine.BeginStep();
            engine.Put<float>(gvar, v.data());
            engine.EndStep();
        }
        engine.Close();
    }
#if ADIOS2_USE_MPI
    MPI_Barrier(MPI_COMM_WORLD); // every writer must be done before inspection
#endif

    // Layout: with Aggregate=on the data sub-files are data.<node>, one per
    // node, so there must be at most one per rank (fewer when ranks share a
    // node). No drain progress files may be left behind.
    {
        ASSERT_TRUE(std::filesystem::exists(TestName));
        ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(TestName) / "md.idx"));
        int dataFileCount = 0;
        for (const auto &e : std::filesystem::directory_iterator(TestName))
        {
            const std::string fn = e.path().filename().string();
            EXPECT_EQ(fn.rfind("bbstage.drain.", 0), std::string::npos)
                << "stale drain progress file: " << e.path();
            if (fn.rfind("data.", 0) == 0)
            {
                ++dataFileCount;
            }
        }
        EXPECT_GE(dataFileCount, 1) << "no data sub-file found";
        EXPECT_LE(dataFileCount, size) << "more data sub-files than ranks: " << dataFileCount;
    }

    // Read back with the stock bp5 engine; must be byte-identical to the
    // non-aggregated layout.
    {
        adios2::IO io = m_Adios.DeclareIO("bp5-reader-agg");
        io.SetEngine("bp5");
        adios2::Engine reader = io.Open(TestName, adios2::Mode::ReadRandomAccess);
        EXPECT_EQ(reader.Steps(), static_cast<size_t>(nSteps));
        auto gvar = io.InquireVariable<float>("gdata");
        ASSERT_TRUE(static_cast<bool>(gvar));
        auto attr = io.InquireAttribute<std::string>("written_by");
        ASSERT_TRUE(static_cast<bool>(attr));
        EXPECT_EQ(attr.Data()[0], "bbstage-agg-engine");

        for (size_t s = 0; s < static_cast<size_t>(nSteps); ++s)
        {
            auto expected = MakeGlobalStep(s, n, size);
            std::vector<float> got(globalN);
            gvar.SetStepSelection({s, 1});
            gvar.SetSelection({{0}, {globalN}});
            reader.Get<float>(gvar, got.data(), adios2::Mode::Sync);
            EXPECT_EQ(got, expected) << "step " << s;
        }
        reader.Close();
    }
}

// Staged close + aggregate: the app-visible cost ends at the node-local SSD
// write; the node leader then drains the aggregated data and commits the
// metadata in the process background. Poll the bbstage.final marker (the
// durability/visibility point), then read back with the stock bp5 engine.
TEST_F(EngineBbstage, StagedAggregateDrainReadBackBP5)
{
    constexpr size_t n = 100;
    constexpr int nSteps = 3;
    const int rank = MpiRank();
    const int size = MpiSize();
    const size_t globalN = n * static_cast<size_t>(size);

    {
        adios2::IO io = m_Adios.DeclareIO("bbstage-agg-staged-writer");
        io.SetEngine("bbstage");
        io.SetParameters({{"BurstBufferPath", StagingDir}, {"Aggregate", "on"},
                          {"CloseMode", "staged"}});

        auto gvar = io.DefineVariable<float>("gdata", {globalN},
                                             {static_cast<size_t>(rank) * n}, {n});
        io.DefineAttribute<std::string>("written_by", "bbstage-agg-staged");

        adios2::Engine engine = io.Open(TestName, adios2::Mode::Write);
        for (int s = 0; s < nSteps; ++s)
        {
            auto v = MakeStep(s, n);
            engine.BeginStep();
            engine.Put<float>(gvar, v.data());
            engine.EndStep();
        }
        engine.Close(); // returns as soon as the step is staged to node-local SSD
    }
#if ADIOS2_USE_MPI
    MPI_Barrier(MPI_COMM_WORLD);
#endif

    // Wait for the background finalizer to drain, commit, and publish the
    // durability marker.
    {
        const std::string fin = (std::filesystem::path(TestName) / "bbstage.final").string();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!std::filesystem::exists(fin))
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                FAIL() << "timed out waiting for the bbstage.final marker of " << TestName;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // Read back with the stock bp5 engine; must be byte-identical.
    {
        adios2::IO io = m_Adios.DeclareIO("bp5-reader-agg-staged");
        io.SetEngine("bp5");
        adios2::Engine reader = io.Open(TestName, adios2::Mode::ReadRandomAccess);
        EXPECT_EQ(reader.Steps(), static_cast<size_t>(nSteps));
        auto gvar = io.InquireVariable<float>("gdata");
        ASSERT_TRUE(static_cast<bool>(gvar));
        auto attr = io.InquireAttribute<std::string>("written_by");
        ASSERT_TRUE(static_cast<bool>(attr));
        EXPECT_EQ(attr.Data()[0], "bbstage-agg-staged");

        for (size_t s = 0; s < static_cast<size_t>(nSteps); ++s)
        {
            auto expected = MakeGlobalStep(s, n, size);
            std::vector<float> got(globalN);
            gvar.SetStepSelection({s, 1});
            gvar.SetSelection({{0}, {globalN}});
            reader.Get<float>(gvar, got.data(), adios2::Mode::Sync);
            EXPECT_EQ(got, expected) << "step " << s;
        }
        reader.Close();
    }
}

// Staged close + watermark backpressure with the concurrent drain: one
// session whose run total (16 MiB per rank) exceeds the staging watermark
// (4 MiB), which still exceeds one step (1 MiB). Before the drain workers
// started at Open this write loop would block at the watermark until Close()
// -- i.e. forever; it can only complete because the in-flight drain keeps
// releasing the watermark step by step.
TEST_F(EngineBbstage, StagedWatermarkConcurrentDrain)
{
    constexpr size_t n = 256 * 1024; // 1 MiB of floats per step per rank
    constexpr int nSteps = 16; // 16 MiB total per rank
    const int rank = MpiRank();
    const int size = MpiSize();
    const size_t globalN = n * static_cast<size_t>(size);

    {
        adios2::IO io = m_Adios.DeclareIO("bbstage-staged-wm-writer");
        io.SetEngine("bbstage");
        io.SetParameters({{"BurstBufferPath", StagingDir}, {"CloseMode", "staged"},
                          {"StagingWatermarkMB", "4"}, {"CommitTimeoutSecs", "30"}});

        auto gvar = io.DefineVariable<float>("gdata", {globalN},
                                             {static_cast<size_t>(rank) * n}, {n});
        io.DefineAttribute<std::string>("written_by", "bbstage-staged-wm");

        adios2::Engine engine = io.Open(TestName, adios2::Mode::Write);
        for (int s = 0; s < nSteps; ++s)
        {
            auto v = MakeStep(s, n);
            engine.BeginStep();
            engine.Put<float>(gvar, v.data());
            engine.EndStep();
        }
        engine.Close();
    }
#if ADIOS2_USE_MPI
    MPI_Barrier(MPI_COMM_WORLD);
#endif

    // Wait for the background finalizer to drain the remainder, commit, and
    // publish the durability marker.
    {
        const std::string fin = (std::filesystem::path(TestName) / "bbstage.final").string();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!std::filesystem::exists(fin))
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                FAIL() << "timed out waiting for the bbstage.final marker of " << TestName;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // Read back with the stock bp5 engine; must be byte-identical.
    {
        adios2::IO io = m_Adios.DeclareIO("bp5-reader-staged-wm");
        io.SetEngine("bp5");
        adios2::Engine reader = io.Open(TestName, adios2::Mode::ReadRandomAccess);
        EXPECT_EQ(reader.Steps(), static_cast<size_t>(nSteps));
        auto gvar = io.InquireVariable<float>("gdata");
        ASSERT_TRUE(static_cast<bool>(gvar));
        auto attr = io.InquireAttribute<std::string>("written_by");
        ASSERT_TRUE(static_cast<bool>(attr));
        EXPECT_EQ(attr.Data()[0], "bbstage-staged-wm");

        for (size_t s = 0; s < static_cast<size_t>(nSteps); ++s)
        {
            auto expected = MakeGlobalStep(s, n, size);
            std::vector<float> got(globalN);
            gvar.SetStepSelection({s, 1});
            gvar.SetSelection({{0}, {globalN}});
            reader.Get<float>(gvar, got.data(), adios2::Mode::Sync);
            EXPECT_EQ(got, expected) << "step " << s;
        }
        reader.Close();
    }
}

// The staged watermark backpressure stall error: with no drain progress, an
// AccountStaged() that cannot fit under the watermark must throw after the
// stall timeout instead of blocking forever. Driven at the finalizer
// singleton level (no engine involved), so it is fast and deterministic.
TEST_F(EngineBbstage, StagedWatermarkStallTimeout)
{
    auto &fin = adios2::core::engine::BbstageStagedFinalizer::Instance();
    // known starting state: no pending bytes (clamped), no watermark
    fin.AccountDrained(std::numeric_limits<uint64_t>::max());
    fin.SetStagingWatermark(0);
    fin.SetStagingStallTimeout(0.5f);

    fin.SetStagingWatermark(1024);
    fin.AccountStaged(1000); // fits: 0 pending + 1000 <= 1024
    // 1000 pending + 1000 new > 1024 and no drain is making progress: the
    // stall timeout must fire instead of a silent hang.
    EXPECT_THROW(fin.AccountStaged(1000), std::runtime_error);

    // restore the singleton for the other tests
    fin.AccountDrained(1000);
    fin.SetStagingWatermark(0);
    fin.SetStagingStallTimeout(300.f);
}

// ---- store circuit breaker -------------------------------------------------

// The oracle state machine with a fake probe (a CI box cannot hang a real
// filesystem): error streak -> Down, probe-heartbeat staleness -> Down
// (the stalled-store signature: the probe thread is stuck in the kernel and
// its heartbeat is the ONLY visible trace), quorum of clean probes -> back.
TEST(StoreHealthBreaker, TransitionsAndQuorum)
{
    using SH = adios2::core::engine::BbstageStoreHealth;
    using State = adios2::core::engine::BbstageStoreHealthState;

    // preflight failure surfaces as a configuration error, breaker stays off
    {
        SH h;
        h.SetProbeForTest([](int &err) {
            err = EACCES;
            return false;
        });
        SH::Config cfg;
        cfg.Enabled = true;
        cfg.ProbePath = "/unused-probe-injected";
        EXPECT_FALSE(h.Configure(cfg));
        EXPECT_FALSE(h.LastError().empty());
        EXPECT_FALSE(h.Enabled());
        EXPECT_TRUE(h.StoreReady()); // inert: never blocks anyone
    }

    {
        SH h;
        std::atomic<bool> up{true};
        h.SetProbeForTest([&up](int &err) {
            if (up.load())
            {
                return true;
            }
            err = EIO; // the classic stalled-Lustre errno
            return false;
        });
        SH::Config cfg;
        cfg.Enabled = true;
        cfg.ProbePath = "/unused-probe-injected";
        cfg.ProbeIntervalSecs = 0.02f;
        cfg.DownAfterSecs = 0.15f;
        cfg.UpAfterProbes = 2;
        cfg.SlowAfterSecs = 30.f;
        ASSERT_TRUE(h.Configure(cfg));
        ASSERT_TRUE(h.Enabled());
        EXPECT_EQ(h.State(), State::Healthy);
        EXPECT_TRUE(h.StoreReady());

        // store errors sustained -> Down
        up.store(false);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (h.State() != State::Down && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_EQ(h.State(), State::Down);
        EXPECT_FALSE(h.StoreReady());

        // recovery needs the quorum: one clean probe must not release it
        up.store(true);
        EXPECT_EQ(h.State(), State::Down); // far too early for 2 probes at 20 ms

        // quorum reached -> Healthy again (EWMA latency is microseconds, so
        // not Slow)
        const auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (h.State() == State::Down && std::chrono::steady_clock::now() < deadline2)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_EQ(h.State(), State::Healthy);
        EXPECT_TRUE(h.StoreReady());

        // passive drain failures feed the same error window as probe errors
        // (here the probe is healthy: one observation opens the window, the
        // streak is what trips)
        h.ObserveFailure();
        EXPECT_EQ(h.State(), State::Healthy); // instant: no streak yet

        h.ResetForTest();
        EXPECT_FALSE(h.Enabled());
        EXPECT_TRUE(h.StoreReady());
    }

    // stale heartbeat: a probe blocked in the kernel cannot report errors --
    // staleness of its heartbeat must be what trips the breaker
    {
        SH h;
        int calls = 0;
        h.SetProbeForTest([&calls](int & /*err*/) {
            ++calls;
            if (calls == 2) // first call was the preflight; this one hangs
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            }
            return true;
        });
        SH::Config cfg;
        cfg.Enabled = true;
        cfg.ProbePath = "/unused-probe-injected";
        cfg.ProbeIntervalSecs = 0.02f;
        cfg.DownAfterSecs = 0.2f;
        cfg.UpAfterProbes = 2;
        ASSERT_TRUE(h.Configure(cfg));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (h.State() != State::Down && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_EQ(h.State(), State::Down); // while the probe is still stuck
        const auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (h.State() != State::Healthy && std::chrono::steady_clock::now() < deadline2)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        h.ResetForTest();
    }
}

// Retry classification for breaker-deferred drain jobs: conditions a retry
// can never fix fail the run immediately even with the breaker on.
TEST(StoreHealthBreaker, PermanentErrorsAreNeverWeather)
{
    using namespace adios2::core::engine;
    EXPECT_FALSE(BbstageStoreErrorRetryable(ENOSPC));
    EXPECT_FALSE(BbstageStoreErrorRetryable(EDQUOT));
    EXPECT_FALSE(BbstageStoreErrorRetryable(EINVAL));
    EXPECT_FALSE(BbstageStoreErrorRetryable(ENOENT));
    EXPECT_TRUE(BbstageStoreErrorRetryable(EIO));
    EXPECT_TRUE(BbstageStoreErrorRetryable(ETIMEDOUT));
    EXPECT_TRUE(BbstageStoreErrorRetryable(ESTALE));
    EXPECT_TRUE(BbstageStoreErrorRetryable(EAGAIN));
    EXPECT_EQ(BbstagePosixErrnoOf(std::make_exception_ptr(std::runtime_error("x"))), 0);
    EXPECT_EQ(BbstagePosixErrnoOf(
                  std::make_exception_ptr(BbstagePosixError(EIO, "staged->final"))),
              EIO);
}

// Backpressure with the breaker on: while the store is Down the release
// target expands to the hard watermark (due checkpoints wait on the NVMe),
// and only exhausting THAT still fails the run. Driven at the finalizer
// singleton, like the stall-timeout test: fast and deterministic.
TEST_F(EngineBbstage, StagedBreakerExpandsWatermarkButStaysBounded)
{
    using SH = adios2::core::engine::BbstageStoreHealth;
    auto &fin = adios2::core::engine::BbstageStagedFinalizer::Instance();
    auto &health = SH::Instance();
    health.ResetForTest();

    std::atomic<bool> up{true};
    health.SetProbeForTest([&up](int &err) {
        if (up.load())
        {
            return true;
        }
        err = EIO;
        return false;
    });
    SH::Config cfg;
    cfg.Enabled = true;
    cfg.ProbePath = "/unused-probe-injected";
    cfg.ProbeIntervalSecs = 0.02f;
    cfg.DownAfterSecs = 0.15f;
    ASSERT_TRUE(health.Configure(cfg));

    fin.AccountDrained(std::numeric_limits<uint64_t>::max());
    fin.SetStagingWatermark(4 << 20);
    fin.SetStagingStallTimeout(0.4f);
    fin.SetStoreBreaker(true, 8 << 20); // hard watermark: 8 MiB

    up.store(false);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (health.State() == adios2::core::engine::BbstageStoreHealthState::Healthy &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(health.State(), adios2::core::engine::BbstageStoreHealthState::Down);

    // 3 MiB fits under the nominal watermark
    fin.AccountStaged(3 << 20);
    // 3+3 = 6 MiB exceeds the nominal 4 MiB watermark but fits under the hard
    // 8 MiB while the store is Down: the wait expands instead of blocking
    fin.AccountStaged(3 << 20);
    // 6+3 exceeds even the hard watermark, and no drain can make progress
    // while Down: the fuse must fire (breaker-off behaviour at the hard cap)
    EXPECT_THROW(fin.AccountStaged(3 << 20), std::runtime_error);

    // restore all singletons for the other tests
    fin.AccountDrained(std::numeric_limits<uint64_t>::max());
    fin.SetStagingWatermark(0);
    fin.SetStagingStallTimeout(300.f);
    fin.SetStoreBreaker(false, 0);
    health.ResetForTest();
}

// The end-to-end story with the breaker on and a healthy store: zero harm.
// (The outage story needs a probe injection, which only the singleton
// supports; the outage mechanics themselves are covered by the two tests
// above, the engine mechanics by this one + the existing staged suite.)
TEST_F(EngineBbstage, StoreBreakerHealthyRoundTrip)
{
    adios2::core::engine::BbstageStoreHealth::Instance().ResetForTest();
    const int rank = MpiRank();
    const int size = MpiSize();
    constexpr size_t n = 64 * 1024;
    constexpr int nSteps = 6;
    const size_t globalN = n * static_cast<size_t>(size);

    {
        adios2::IO io = m_Adios.DeclareIO("bbstage-breaker-healthy-writer");
        io.SetEngine("bbstage");
        io.SetParameters({{"BurstBufferPath", StagingDir}, {"CloseMode", "staged"},
                          {"StoreBreaker", "on"}, {"BreakerProbeSeconds", "0.05"}});
        auto gvar = io.DefineVariable<float>("gdata", {globalN},
                                             {static_cast<size_t>(rank) * n}, {n});
        io.DefineAttribute<std::string>("written_by", "bbstage-breaker");
        adios2::Engine engine = io.Open(TestName, adios2::Mode::Write);
        for (int s = 0; s < nSteps; ++s)
        {
            auto v = MakeStep(s, n);
            engine.BeginStep();
            engine.Put<float>(gvar, v.data());
            engine.EndStep();
        }
        engine.Close();
    }
    const std::string fin = (std::filesystem::path(TestName) / "bbstage.final").string();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!std::filesystem::exists(fin))
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            FAIL() << "timed out waiting for the bbstage.final marker";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // the health sentinel is part of the checkpoint while the breaker is on:
    // 8 bytes, written through real I/O, readable
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(TestName) / "bbstage.probe"));

    adios2::IO io = m_Adios.DeclareIO("bp5-reader-breaker-healthy");
    io.SetEngine("bp5");
    adios2::Engine reader = io.Open(TestName, adios2::Mode::ReadRandomAccess);
    EXPECT_EQ(reader.Steps(), static_cast<size_t>(nSteps));
    auto gvar = io.InquireVariable<float>("gdata");
    ASSERT_TRUE(static_cast<bool>(gvar));
    for (size_t s = 0; s < static_cast<size_t>(nSteps); ++s)
    {
        auto expected = MakeGlobalStep(s, n, size);
        std::vector<float> got(globalN);
        gvar.SetStepSelection({s, 1});
        gvar.SetSelection({{0}, {globalN}});
        reader.Get<float>(gvar, got.data(), adios2::Mode::Sync);
        EXPECT_EQ(got, expected) << "step " << s;
    }
    reader.Close();
    adios2::core::engine::BbstageStoreHealth::Instance().ResetForTest();
}

// The full outage through a real engine: store flips down mid-run, drain
// dispatch halts (progress record frozen), the run keeps computing, the
// store returns, everything drains and commits -- byte-identical output.
TEST_F(EngineBbstage, StagedBreakerOutageSurvival)
{
    using SH = adios2::core::engine::BbstageStoreHealth;
    using State = adios2::core::engine::BbstageStoreHealthState;
    auto &health = SH::Instance();
    health.ResetForTest();

    std::atomic<bool> up{true};
    health.SetProbeForTest([&up](int &err) {
        if (up.load())
        {
            return true;
        }
        err = EIO;
        return false;
    });

    constexpr size_t n = 262144; // 1 MiB of floats per step
    constexpr int nSteps = 6;
    const int rank = MpiRank();
    const int size = MpiSize();
    const size_t globalN = n * static_cast<size_t>(size);

    {
        adios2::IO io = m_Adios.DeclareIO("bbstage-breaker-outage-writer");
        io.SetEngine("bbstage");
        io.SetParameters({{"BurstBufferPath", StagingDir}, {"CloseMode", "staged"},
                          {"StagingWatermarkMB", "4"}, {"CommitTimeoutSecs", "30"},
                          {"StoreBreaker", "on"}, {"BreakerProbeSeconds", "0.05"},
                          {"BreakerDownAfterSecs", "0.2"}, {"BreakerUpAfterProbes", "2"}});
        auto gvar = io.DefineVariable<float>("gdata", {globalN},
                                             {static_cast<size_t>(rank) * n}, {n});

        adios2::Engine engine = io.Open(TestName, adios2::Mode::Write);
        const std::string prog = TestName + "/bbstage.drain." + std::to_string(rank);
        auto writeStep = [&](int s) {
            auto v = MakeStep(s, n);
            engine.BeginStep();
            engine.Put<float>(gvar, v.data());
            engine.EndStep();
        };

        // two steps while the weather is fine
        writeStep(0);
        writeStep(1);

        // the storm hits
        up.store(false);
        const auto deadlineDown = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (health.State() != State::Down &&
               std::chrono::steady_clock::now() < deadlineDown)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_EQ(health.State(), State::Down);

        // the run keeps computing: two more steps land on the staging NVMe
        writeStep(2);
        writeStep(3);

        // Dispatch is held: the published record must go quiet (bytes wait on
        // the NVMe, exactly the point of the breaker). A job already dequeued
        // when the storm hit still drains -- the invariant -- and its record
        // publish may land after the trip, so first let the record quiesce,
        // then require the hold window.
        const auto readPublished = [&prog] {
            adios2::core::engine::BbstageDrainProgress q;
            return adios2::core::engine::BbstageReadProgress(prog, q) ? q.DrainedSteps
                                                                      : uint64_t(0);
        };
        uint64_t frozen = readPublished();
        const auto settleDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            const uint64_t now = readPublished();
            if (now == frozen)
            {
                break; // two consecutive reads agree: in-flight work has landed
            }
            frozen = now;
            if (std::chrono::steady_clock::now() > settleDeadline)
            {
                FAIL() << "drain never quiesced after the breaker tripped";
            }
        }
        const auto deadlineFrozen = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadlineFrozen)
        {
            ASSERT_EQ(readPublished(), frozen) << "drain advanced during the outage";
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        // weather clears, the last two steps go through, close commits
        up.store(true);
        writeStep(4);
        writeStep(5);
        engine.Close();
    }

    const std::string marker = (std::filesystem::path(TestName) / "bbstage.final").string();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!std::filesystem::exists(marker))
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            FAIL() << "timed out waiting for bbstage.final after the outage";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    adios2::IO io = m_Adios.DeclareIO("bp5-reader-breaker-outage");
    io.SetEngine("bp5");
    adios2::Engine reader = io.Open(TestName, adios2::Mode::ReadRandomAccess);
    EXPECT_EQ(reader.Steps(), static_cast<size_t>(nSteps));
    auto gvar = io.InquireVariable<float>("gdata");
    ASSERT_TRUE(static_cast<bool>(gvar));
    for (size_t s = 0; s < static_cast<size_t>(nSteps); ++s)
    {
        auto expected = MakeGlobalStep(s, n, size);
        std::vector<float> got(globalN);
        gvar.SetStepSelection({s, 1});
        gvar.SetSelection({{0}, {globalN}});
        reader.Get<float>(gvar, got.data(), adios2::Mode::Sync);
        EXPECT_EQ(got, expected) << "step " << s;
    }
    reader.Close();
    adios2::core::engine::BbstageStagedFinalizer::Instance().SetStoreBreaker(false, 0);
    health.ResetForTest();
}

int main(int argc, char **argv)
{
#if ADIOS2_USE_MPI
    int provided;
    MPI_Init_thread(nullptr, nullptr, MPI_THREAD_MULTIPLE, &provided);
#endif
    ::testing::InitGoogleTest(&argc, argv);
    int result = RUN_ALL_TESTS();
#if ADIOS2_USE_MPI
    MPI_Finalize();
#endif
    return result;
}
