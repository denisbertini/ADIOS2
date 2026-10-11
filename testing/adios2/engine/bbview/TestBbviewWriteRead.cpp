/*
 * SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <adios2.h>
#if ADIOS2_USE_MPI
#include <mpi.h>
#endif

#include <gtest/gtest.h>

#include <filesystem>
#include <numeric>
#include <string>
#include <vector>

namespace
{

const std::string TestName = "BbviewTest.bp";
const std::string StagingDir = "bbview-staging-test";

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

class EngineBbview : public ::testing::Test
{
public:
    EngineBbview() = default;
    ~EngineBbview() override {}

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

// Write with the bbview engine (steps land on the node-local staging tier and
// drain in the background), close drains everything, then read back with the
// standard bp5 engine to prove full output compatibility.
TEST_F(EngineBbview, WriteDrainReadBackBP5)
{
    constexpr size_t n = 100;
    constexpr int nSteps = 3;
    const int rank = MpiRank();
    const int size = MpiSize();
    const size_t globalN = n * static_cast<size_t>(size);

    {
        adios2::IO io = m_Adios.DeclareIO("bbview-writer");
        io.SetEngine("bbview");
        io.SetParameters({{"BurstBufferPath", StagingDir}, {"verbose", "0"}});

        // each rank owns a disjoint block of the global array
        auto gvar = io.DefineVariable<float>("gdata", {globalN},
                                             {static_cast<size_t>(rank) * n}, {n});
        io.DefineAttribute<std::string>("written_by", "bbview-engine");

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
            EXPECT_EQ(std::string(e.path().filename()).rfind("bbview.drain.", 0),
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
        EXPECT_EQ(attr.Data()[0], "bbview-engine");

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

    // and with engine "bbview" in read mode (registers the BP5 reader)
    {
        adios2::IO io = m_Adios.DeclareIO("bbview-reader");
        io.SetEngine("bbview");
        adios2::Engine reader = io.Open(TestName, adios2::Mode::ReadRandomAccess);
        EXPECT_EQ(reader.Steps(), static_cast<size_t>(nSteps));
        reader.Close();
    }
}

// Streaming read: the engine only commits metadata after the step data has
// drained from the burst buffer to the final store, so every step a streaming
// reader observes is complete.
TEST_F(EngineBbview, StreamingReadSeesOnlyDrainedSteps)
{
    constexpr size_t n = 50;
    constexpr int nSteps = 4;
    const int rank = MpiRank();
    const int size = MpiSize();
    const size_t globalN = n * static_cast<size_t>(size);

    adios2::IO wio = m_Adios.DeclareIO("bbview-stream-writer");
    wio.SetEngine("bbview");
    wio.SetParameters({{"BurstBufferPath", StagingDir}});
    auto wvar = wio.DefineVariable<float>("gdata", {globalN},
                                          {static_cast<size_t>(rank) * n}, {n});

    adios2::Engine writer = wio.Open(TestName, adios2::Mode::Write);

    adios2::IO rio = m_Adios.DeclareIO("bbview-stream-reader");
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
