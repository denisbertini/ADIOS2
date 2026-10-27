/*
 * SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ADIOS2_ENGINE_BBVIEW_BBVIEWDRAINCOMMON_H_
#define ADIOS2_ENGINE_BBVIEW_BBVIEWDRAINCOMMON_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <vector>

namespace adios2
{
namespace core
{
namespace engine
{

/** One unit of deferred work handed to the in-process "bbviewd" threads.
 * Data jobs: copy staging file -> final data file at FinalPos.
 * Commit jobs (rank 0 only): append per-step metadata once the step is
 * drained on every writer rank.
 * Shared by the engine (durable close mode) and the process-level finalizer
 * (staged close mode, BbviewStagedFinalizer). */
struct BbviewDrainJob
{
    size_t Seq = 0; // data job sequence number (one data job per step)
    size_t Step = 0;
    bool IsCommit = false;
    // data job:
    std::string StagingPath;
    uint64_t FinalPos = 0; // offset of the step in the final data sub-file
    size_t DataSize = 0;
    // commit job (rank 0):
    std::vector<char> MMDBytes; // meta-metadata records for mmd.0
    std::vector<char> MDRecord; // one md.0 step record
    std::vector<char> IdxRecord; // one md.idx step record
};

/** Fixed-size drain progress record, the in-process replacement for
 * bbview's out-of-band xattr handoff. Published by each writer rank with
 * a single pwrite at offset 0 of a pre-created file in the output
 * directory, guarded by a sequence lock (Seq/SeqCopy). tmp+rename was
 * avoided on purpose: on network/FUSE final stores a rename of a
 * just-created tmp file can transiently fail with ENOENT due to stale
 * lookups. Readers reject records whose sequence numbers disagree;
 * tearing can therefore only make progress look older, never newer. */
constexpr uint64_t BbviewDrainMagicValue = 0x42425644ULL; // "BBVD"

struct BbviewDrainProgress
{
    uint64_t Magic = BbviewDrainMagicValue;
    uint64_t Version = 1;
    uint64_t Rank = 0;
    uint64_t Seq = 0;          // incremented on every publish
    uint64_t DrainedSteps = 0; // number of leading steps fully drained
    uint64_t DrainedBytes = 0;
    uint64_t SeqCopy = 0; // must equal Seq for a stable record
};

/** Read one progress record. Returns false when the file is missing or the
 * record is not (fully) written / is mid-update; the caller polls. */
bool BbviewReadProgress(const std::string &path, BbviewDrainProgress &out);

/** Publish one progress record to an open fd (the caller owns the fd and
 * the per-session sequence counter). Single fixed-offset pwrite of the
 * whole record: no tmp file, no rename. Throws std::runtime_error on a
 * persistent pwrite failure. */
void BbviewWriteProgressRecord(int fd, const std::string &path, uint64_t rank,
                               uint64_t drainedSteps, uint64_t drainedBytes,
                               uint64_t &seq, const std::string &host);

} // end namespace engine
} // end namespace core
} // end namespace adios2

#endif // ADIOS2_ENGINE_BBVIEW_BBVIEWDRAINCOMMON_H_
