/*
 * SPDX-FileCopyrightText: 2024 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ADIOS2_TOOLKIT_TRANSPORT_FILE_FILEXROOTD_H_
#define ADIOS2_TOOLKIT_TRANSPORT_FILE_FILEXROOTD_H_

#include <memory>
#include <mutex>
#include <string>

#include "adios2/common/ADIOSConfig.h"
#include "adios2/toolkit/transport/Transport.h"

namespace adios2
{
namespace helper
{
class Comm;
}
namespace transport
{

/**
 * Read-only file transport for objects stored on an XRootD/EOS archive.
 *
 * Speaks *plain object* semantics (open + positional read) through libXrdCl
 * -- the same access class used by xrdcp and leos -- so it works against a
 * stock EOS deployment with no server-side ADIOS2 plugin (unlike the
 * _adios remote protocol in toolkit/remote).
 *
 * Parameters (lower-case keys, as delivered by transport::OpenFile):
 *   endpoint      XRootD server host[:port], e.g. helios.example.org
 *   bucket        path prefix under which the dataset files live,
 *                 e.g. /eos/helios/Users/alice/warpx/diags/openpmd_000050.bp5
 *   sessiontoken  capability token carried as ?authz=<token> on every URL
 *                 (EOS convention); defaults to $EOSAUTHZ when unset.
 *                 The zteos64/scitokens forms are opaque and passed verbatim.
 *   chunksize     maximum bytes per XRootD read request (default 16 MiB);
 *                 larger Read() calls are split in sequential chunks.
 *
 * The object name is the *base name* of the file the engine opens (BP5 data
 * files are flat inside the dataset directory); local directories and the
 * dataset path itself are stripped, so a reader configured for a local unit
 * needs no name manipulation: the same "openpmd_000050.bp5" directory opened
 * with DataFileTransport=xrootd + endpoint/bucket streams payload in place.
 *
 * File size is captured from the file handle at Open().  Token expiry while
 * files are open causes read errors with the server's message propagated --
 * mint tokens longer than the expected reader run time.
 *
 * Maintenance note: this file compiles against container/host libXrdCl
 * headers (verified on 5.9.x).  A major XRootD bump may require small API
 * fixups in FileXRootD.cpp only.
 */
class FileXRootD : public Transport
{
public:
    FileXRootD(helper::Comm const &comm);
    ~FileXRootD() final;

    void SetParameters(const Params &parameters) final;

    void Open(const std::string &name, const Mode openMode, const bool async = false,
              const bool directio = false) final;

    void OpenChain(const std::string &name, Mode openMode, const helper::Comm &chainComm,
                   const bool async = false, const bool directio = false) final;

    /** read-only transport: any write is a hard error */
    void Write(const char *buffer, size_t size, size_t start = MaxSizeT) final;

    void Read(char *buffer, size_t size, size_t start = 0) final;

    size_t GetSize() final;

    /** Does nothing; reads are synchronous and complete on return. */
    void Flush() final;

    void Close() final;

    void Delete() final;

    void SeekToEnd() final;
    void SeekToBegin() final;
    void Seek(const size_t start = MaxSizeT) final;
    size_t CurrentPos() final { return m_SeekPos; }

    void Truncate(const size_t length) final;

    /** No-op: the namespace is managed by the archive (leos). */
    void MkDir(const std::string &fileName) final;

private:
    struct Impl;                    // hides all XrdCl types from this header
    std::unique_ptr<Impl> m_Impl;   // one open XrdCl::File + URL
    std::mutex m_FileMutex;         // XrdCl::File is not thread-safe

    std::string m_Endpoint;         // host[:port] of the XRootD server
    std::string m_Prefix;           // archive-side directory of the dataset
    std::string m_Token;            // capability token (?authz=), or empty
    size_t m_ChunkSize = 16 * 1024 * 1024;

    std::string m_Name;             // archive-side full URL actually opened
    size_t m_Size = 0;              // captured at Open()
    size_t m_SeekPos = 0;
    bool m_IsOpen = false;

    std::string BuildUrl(const std::string &name) const;
};

} // end namespace transport
} // end namespace adios2

#endif /* ADIOS2_TOOLKIT_TRANSPORT_FILE_FILEXROOTD_H_ */
