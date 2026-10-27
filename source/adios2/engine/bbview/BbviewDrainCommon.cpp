/*
 * SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "BbviewDrainCommon.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <unistd.h>

#include <fcntl.h>

namespace adios2
{
namespace core
{
namespace engine
{

bool BbviewReadProgress(const std::string &path, BbviewDrainProgress &out)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return false;
    }
    char buf[sizeof(BbviewDrainProgress)];
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
    return out.Magic == BbviewDrainMagicValue && out.Version == 1 && out.Seq == out.SeqCopy;
}

void BbviewWriteProgressRecord(int fd, const std::string &path, uint64_t rank,
                               uint64_t drainedSteps, uint64_t drainedBytes,
                               uint64_t &seq, const std::string &host)
{
    BbviewDrainProgress p;
    p.Rank = rank;
    p.DrainedSteps = drainedSteps;
    p.DrainedBytes = drainedBytes;
    p.Seq = ++seq;
    p.SeqCopy = p.Seq;

    // single fixed-offset pwrite of the whole record: no tmp file, no rename
    const char *ptr = reinterpret_cast<const char *>(&p);
    size_t off = 0;
    size_t left = sizeof(p);
    while (left > 0)
    {
        const ssize_t w = pwrite(fd, ptr, left, static_cast<off_t>(off));
        if (w < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            throw std::runtime_error("bbview: write progress " + path + " (rank " +
                                     std::to_string(rank) + ", host " + host + "): " +
                                     std::strerror(errno));
        }
        ptr += w;
        off += static_cast<uint64_t>(w);
        left -= static_cast<size_t>(w);
    }
}

} // end namespace engine
} // end namespace core
} // end namespace adios2
