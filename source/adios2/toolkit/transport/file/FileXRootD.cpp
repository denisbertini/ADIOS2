/*
 * SPDX-FileCopyrightText: 2024 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "FileXRootD.h"

#include "adios2/helper/adiosLog.h"
#include "adios2/helper/adiosString.h"

#include <XrdCl/XrdClFile.hh>
#include <XrdCl/XrdClStatInfo.hh>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace adios2
{
namespace transport
{

struct FileXRootD::Impl
{
    XrdCl::File File;
};

FileXRootD::FileXRootD(helper::Comm const &comm) : Transport("File", "XRootD", comm), m_Impl(std::make_unique<Impl>()) {}

FileXRootD::~FileXRootD() = default;

void FileXRootD::SetParameters(const Params &parameters)
{
    // Keys arrive lower-cased from transport::OpenFile.  The S3 plumbing in
    // the engines already forwards endpoint/bucket/sessiontoken verbatim.
    helper::SetParameterValue("endpoint", parameters, m_Endpoint);
    helper::SetParameterValue("bucket", parameters, m_Prefix);
    helper::SetParameterValue("sessiontoken", parameters, m_Token);

    std::string chunkStr;
    helper::SetParameterValue("chunksize", parameters, chunkStr);
    if (!chunkStr.empty())
        m_ChunkSize = static_cast<size_t>(std::stoull(chunkStr));

    // EOS convention: the capability token may come from the environment
    // (same variable leos uses), so credentials never need to appear in
    // config files or command lines.
    if (m_Token.empty())
    {
        if (const char *envToken = std::getenv("EOSAUTHZ"))
            m_Token = envToken;
    }
    // tokens often arrive via files/environment: strip surrounding whitespace
    // (a trailing newline silently 403s every request)
    const auto trim = [](std::string &s) {
        const size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos)
            s.clear();
        else
        {
            if (b > 0)
                s.erase(0, b);
            helper::RightTrim(s);
        }
    };
    trim(m_Token);
    trim(m_Endpoint);
    trim(m_Prefix);
    while (!m_Endpoint.empty() && m_Endpoint.back() == '/')
        m_Endpoint.pop_back();
    const std::string rootPrefix = "root://";
    if (m_Endpoint.rfind(rootPrefix, 0) == 0)
        m_Endpoint = m_Endpoint.substr(rootPrefix.size());
}

std::string FileXRootD::BuildUrl(const std::string &name) const
{
    // BP5 data files are flat inside the dataset directory: the engine hands
    // us "<dataset-dir>/<file>"; the archive-side location is the base name
    // under the configured prefix.
    std::string base = std::filesystem::path(name).filename().string();
    if (base.empty())
        helper::Throw<std::invalid_argument>("Toolkit", "transport::file::FileXRootD", "Open",
                                             "empty file name: " + name);

    std::string url = "root://" + m_Endpoint;
    if (m_Prefix.empty())
        url += "/";
    else if (m_Prefix[0] != '/')
        url += "/" + m_Prefix;
    else
        url += m_Prefix; // yields the canonical root://host//eos/... form
    if (url.back() != '/')
        url += "/";
    url += base;
    // EOS capability token: a query on the *complete* object path, which is
    // the form stock EOS accepts for reads (site-verified with xrdcp/leos).
    if (!m_Token.empty())
        url += "?authz=" + m_Token;
    return url;
}

void FileXRootD::Open(const std::string &name, const Mode openMode, const bool /*async*/,
                      const bool /*directio*/)
{
    if (openMode != Mode::Read)
        helper::Throw<std::ios_base::failure>("Toolkit", "transport::file::FileXRootD", "Open",
                                              "is a read-only transport, open mode not supported: " +
                                                  name);
    if (m_Endpoint.empty())
        helper::Throw<std::invalid_argument>(
            "Toolkit", "transport::file::FileXRootD", "Open",
            "transport parameter 'endpoint' (XRootD server host[:port]) is required, file: " +
                name);

    std::lock_guard<std::mutex> lock(m_FileMutex);
    m_Name = BuildUrl(name);

    XrdCl::XRootDStatus status = m_Impl->File.Open(m_Name, XrdCl::OpenFlags::Read);
    if (!status.IsOk())
        helper::Throw<std::ios_base::failure>(
            "Toolkit", "transport::file::FileXRootD", "Open",
            "could not open " + m_Endpoint + " object for " + name +
                " (check endpoint/prefix, and token validity -- expired or invalid tokens answer "
                "'permission denied'): " +
                status.ToString());

    // Size at open time from the authenticated handle.  Deliberately not
    // xrdfs/PathInfo stat: token-carrying stat on some EOS builds is
    // refused even when open+read on the same URL works.
    XrdCl::StatInfo *statInfo = nullptr;
    status = m_Impl->File.Stat(&statInfo);
    if (status.IsOk() && statInfo != nullptr)
    {
        m_Size = static_cast<size_t>(statInfo->GetSize());
        delete statInfo;
    }
    else
    {
        if (statInfo != nullptr)
            delete statInfo;
        m_Impl->File.Close();
        helper::Throw<std::ios_base::failure>(
            "Toolkit", "transport::file::FileXRootD", "Open",
            "opened but could not stat " + m_Endpoint + " object for " + name + ": " +
                status.ToString());
    }

    m_SeekPos = 0;
    m_IsOpen = true;
}

void FileXRootD::OpenChain(const std::string &name, Mode openMode,
                           const helper::Comm & /*chainComm*/, const bool async,
                           const bool directio)
{
    Open(name, openMode, async, directio);
}

void FileXRootD::Read(char *buffer, size_t size, size_t start)
{
    std::lock_guard<std::mutex> lock(m_FileMutex);
    if (!m_IsOpen)
        helper::Throw<std::ios_base::failure>("Toolkit", "transport::file::FileXRootD", "Read",
                                              "file is not open");

    size_t offset = (start == MaxSizeT) ? m_SeekPos : start;
    if (offset + size > m_Size)
        helper::Throw<std::ios_base::failure>(
            "Toolkit", "transport::file::FileXRootD", "Read",
            "can't read " + std::to_string(size) + " bytes from position " +
                std::to_string(offset) + " of " + m_Name + " whose size is " +
                std::to_string(m_Size));

    ProfilerStart("read");
    size_t done = 0;
    while (done < size)
    {
        const uint64_t chunk = static_cast<uint64_t>(std::min(size - done, m_ChunkSize));
        XrdCl::iovec iov;
        iov.buffer = buffer + done;
        iov.buffer_length = chunk;
        XrdCl::iovec *out = &iov;
        uint64_t got = 0;
        XrdCl::XRootDStatus status = m_Impl->File.Read(static_cast<off_t>(offset + done), chunk,
                                                       &out, &got);
        if (!status.IsOk())
        {
            ProfilerStop("read");
            helper::Throw<std::ios_base::failure>(
                "Toolkit", "transport::file::FileXRootD", "Read",
                "positional read failed at offset " + std::to_string(offset + done) + " of " +
                    m_Name + " (expired token or unreachable server?): " + status.ToString());
        }
        if (got == 0)
        {
            ProfilerStop("read");
            helper::Throw<std::ios_base::failure>(
                "Toolkit", "transport::file::FileXRootD", "Read",
                "unexpected end of object: " + m_Name + " at offset " +
                    std::to_string(offset + done));
        }
        done += static_cast<size_t>(got);
    }
    ProfilerStop("read");

    m_SeekPos = offset + done;
}

size_t FileXRootD::GetSize()
{
    if (!m_IsOpen)
        helper::Throw<std::ios_base::failure>("Toolkit", "transport::file::FileXRootD", "GetSize",
                                              "file is not open");
    return m_Size;
}

void FileXRootD::Flush() {}

void FileXRootD::Close()
{
    std::lock_guard<std::mutex> lock(m_FileMutex);
    if (m_IsOpen)
    {
        m_Impl->File.Close();
        m_IsOpen = false;
    }
}

void FileXRootD::Write(const char * /*buffer*/, size_t /*size*/, size_t /*start*/)
{
    helper::Throw<std::ios_base::failure>("Toolkit", "transport::file::FileXRootD", "Write",
                                          "does not support writing -- use leos/bbstage drain for "
                                          "archive writes: " +
                                              m_Name);
}

void FileXRootD::Delete()
{
    helper::Throw<std::ios_base::failure>("Toolkit", "transport::file::FileXRootD", "Delete",
                                          "does not support deletion -- the archive namespace is "
                                          "managed by leos: " +
                                              m_Name);
}

void FileXRootD::SeekToEnd() { m_SeekPos = m_Size; }

void FileXRootD::SeekToBegin() { m_SeekPos = 0; }

void FileXRootD::Seek(const size_t start)
{
    if (start != MaxSizeT)
        m_SeekPos = start;
    else
        SeekToEnd();
}

void FileXRootD::Truncate(const size_t /*length*/)
{
    helper::Throw<std::ios_base::failure>("Toolkit", "transport::file::FileXRootD", "Truncate",
                                          "does not support truncating: " + m_Name);
}

void FileXRootD::MkDir(const std::string & /*fileName*/) {}

} // end namespace transport
} // end namespace adios2
