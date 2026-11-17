/*
 * SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ADIOS2_ENGINE_BBSTAGE_BBSTAGEDRAINCOMMON_H_
#define ADIOS2_ENGINE_BBSTAGE_BBSTAGEDRAINCOMMON_H_

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace adios2
{
namespace core
{
namespace engine
{

/** One unit of deferred work handed to the in-process drain worker threads
 * (the daemonless stand-in for bbviewd).
 * Data jobs: copy staging file -> final data file at FinalPos.
 * Commit jobs (rank 0 only): append per-step metadata once the step is
 * drained on every writer rank.
 * Shared by the engine (durable close mode) and the process-level finalizer
 * (staged close mode, BbstageStagedFinalizer). */

struct BbstageDrainJob
{
    size_t Seq = 0; // data job sequence number (one data job per step)
    size_t Step = 0;
    // data job: this rank's own slice. Aggregate (co-write) mode: the slice
    // sits at its pre-assigned offset in the shared data.<node> sub-file,
    // which every rank on the node co-writes with disjoint offsets; each
    // rank drains only its own slices.
    std::string StagingPath;
    uint64_t FinalPos = 0; // offset of the slice in the final data sub-file
    size_t DataSize = 0;
    // commit job (rank 0):
    std::vector<char> MMDBytes; // meta-metadata records for mmd.0
    std::vector<char> MDRecord; // one md.0 step record
    std::vector<char> IdxRecord; // one md.idx step record
};

/** Fixed-size drain progress record, the in-process replacement for
 * the original bbview's out-of-band xattr handoff. Published by each writer
 * rank with
 * a single pwrite at offset 0 of a pre-created file in the output
 * directory, guarded by a sequence lock (Seq/SeqCopy). tmp+rename was
 * avoided on purpose: on network/FUSE final stores a rename of a
 * just-created tmp file can transiently fail with ENOENT due to stale
 * lookups. Readers reject records whose sequence numbers disagree;
 * tearing can therefore only make progress look older, never newer. */
constexpr uint64_t BbstageDrainMagicValue = 0x42425344ULL; // "BBSD"

struct BbstageDrainProgress
{
    uint64_t Magic = BbstageDrainMagicValue;
    uint64_t Version = 1;
    uint64_t Rank = 0;
    uint64_t Seq = 0;          // incremented on every publish
    uint64_t DrainedSteps = 0; // number of leading steps fully drained
    uint64_t DrainedBytes = 0;
    uint64_t SeqCopy = 0; // must equal Seq for a stable record
};

/** Read one progress record. Returns false when the file is missing or the
 * record is not (fully) written / is mid-update; the caller polls. */
bool BbstageReadProgress(const std::string &path, BbstageDrainProgress &out);

/** Publish one progress record to an open fd (the caller owns the fd and
 * the per-session sequence counter). Single fixed-offset pwrite of the
 * whole record: no tmp file, no rename. Throws std::runtime_error on a
 * persistent pwrite failure. */
void BbstageWriteProgressRecord(int fd, const std::string &path, uint64_t rank,
                               uint64_t drainedSteps, uint64_t drainedBytes,
                               uint64_t &seq, const std::string &host);

/** A failed POSIX drain/store operation. Still a std::runtime_error (every
 * existing catch site keeps working, message format unchanged) but it
 * carries the errno so the store breaker can tell transient store errors
 * (retry when the store is back) from permanent ones (fail immediately
 * regardless of breaker state). */
struct BbstagePosixError : std::runtime_error
{
    int Errno;
    BbstagePosixError(int err, const std::string &msg) : std::runtime_error(msg), Errno(err) {}
};

/** errno of a thrown exception, 0 when it is not a BbstagePosixError
 * (logic/config errors are never store weather). */
inline int BbstagePosixErrnoOf(const std::exception_ptr &e)
{
    try
    {
        std::rethrow_exception(e);
    }
    catch (const BbstagePosixError &pe)
    {
        return pe.Errno;
    }
    catch (...)
    {
        return 0;
    }
}

/** Retryability for the store breaker: a store error mid-run on a path that
 * passed the Open-time preflight is weather (EIO/ETIMEDOUT/ESTALE/EAGAIN/
 * EPIPE/EACCES/EROFS/...) UNLESS it names a condition retrying cannot fix
 * (no space, no quota, oversized request, bad arguments/path, vanished
 * staging file). Conservative default: unknown errnos are retryable, and
 * the caller's retry budget caps any misclassification. */
inline bool BbstageStoreErrorRetryable(int err)
{
    switch (err)
    {
    case ENOSPC:
    case EDQUOT:
    case EFBIG:
    case EINVAL:
    case ENAMETOOLONG:
    case ENOENT:
    case ENOTDIR:
        return false;
    default:
        return true;
    }
}

} // end namespace engine
} // end namespace core
} // end namespace adios2

#endif // ADIOS2_ENGINE_BBSTAGE_BBSTAGEDRAINCOMMON_H_
