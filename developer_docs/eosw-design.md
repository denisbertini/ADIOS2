# feat-eosw: BP5 writes straight to an XRootD/EOS archive

Goal: `io.SetEngine("BP5"); io.SetParameters({{"Library","xrootd"},
{"Endpoint","helios.hpc.gsi.de"}, {"Bucket","/eos/helios/Users/u/run0042/unit00100"},
{"SessionToken",""}})` makes the BP5 **writer** produce the unit directly on
EOS — the FileDAOS precedent (`toolkit/transport/file/FileDaos.cpp`: writes
via `daos_array_write`, no POSIX) applied to XRootD's native semantics.
DAOS lesson: use store-native concepts, do not fake a filesystem.

## Current map (as of branch point c6c3cf97f)

Writer file lifecycle — every open is already polymorphic:

```
BP5Writer::OpenFiles / BeginStepProlonging
  transport::OpenFile(comm, name, Mode, params)      toolkit/transport/OpenFile.cpp
    Library param picks: posix | fstream | stdio | daos | xrootd | ...
  BP5Writer members: m_MetadataFile, m_MetadataIndexFile,
                     m_MetaMetadataFile, AggTransportData::m_DataSubstream
                     (all std::shared_ptr<Transport>)   engine/bp5/BP5Writer.h:82-84
```

POSIX touchpoints in the BP5 *writer* path (BP4's lock files and rename-seal
are gone in BP5 — verified by grep: no LockFile/rename):

| touchpoint | today | EOS-mode behavior |
|---|---|---|
| `helper::CreatePath` / `MkDirsBarrier` (local `std::filesystem`) | creates the dataset dir locally | harmless empty local dir; remote namespace is the per-unit `Bucket` prefix (reader already works this way). Optional later: route `Transport::MkDir` to xrdfs mkdir. |
| transport `Open/Write/Flush/Close/Seek/GetSize` | FilePOSIX | **the work of this branch** |
| `.bp5` "seal" | none in BP5 (last Close flushes md) | keep; an explicit final marker (`.complete`) may ride as an attribute or a zero-byte object later — leos already treats markers as truth |

`FileXRootD` today (transport/file/FileXRootD.cpp):
- `Open()` hard-refuses anything but `Mode::Read` and opens with
  `OpenFlags::Read` (line 121) — **the line this feature deletes**;
- `Write/Delete/Truncate` throw; `Flush` is a no-op;
- URL building (`BuildUrl`) already implements the hard-won site rules:
  double slash `root://host//eos/...`, `?authz=` CGI on the complete object
  path, `EOSAUTHZ` env fallback, token trim. Reuse verbatim for writes.
- size capture uses handle `File.Stat` (not PathInfo) — same trick works
  after create (or just track m_Size locally, writes define it).

## Write-mode design (transport)

| Transport call | XRootD mapping | notes |
|---|---|---|
| `Open(name, Mode::Write/Create)` | `File::Open(url, OpenFlags::Update)` — update creates the object when absent (same call path xrdcp uploads use) | `m_Size = 0` on create; server-side `oss` notes (e.g. `&oss.storageClass`) via extra params later |
| `Write(buf, size, start)` | positioned `File::Write(offset, len, buf)`, chunked ≤ `m_ChunkSize` like Read | BP5 appends (`start == MaxSizeT` → append at `m_Size`); random offsets are legal (md index rewrite!) and must keep working |
| `Flush()` | `File::Sync()` | BP5 calls Flush per step-flush; cost = one round-trip — acceptable; measure, add write-buffering knob later |
| `Close()` | `File::Close()` | terminal; EOS objects are complete after Close (no multipart subtleties server-side for the File API) |
| `GetSize/SeekToEnd/Seek` | local `m_Size` bookkeeping (writes advance it) | avoids any stat dependence — metadata RPCs stay out of the hot path entirely |
| `Truncate` | throw (no protocol op) | BP5 writer does not use it; if proven otherwise, rewrite = re-open+rewrite, decide then |
| `Delete` | `XrdCl::DirList`? no — keep throwing; deletion is leos' job (namespace policy) | |

Semantics we adopt (honest, not faked):
1. **Objects appear empty-then-growing**; there is no local atomicity. A
   crashed writer leaves partial objects — mark this mode "trusted job
   environment"; sweeper/marker policy is the archive's side (leos janitor).
2. **No rename, no locks** — BP5 needs neither. If any code path is found
   that wants them, that is a bug in this design, not a feature to fake.
3. **Per-unit `Bucket` prefix** is the unit's EOS directory; basenames stay
   `md.0/data.0/...` exactly like on POSIX, so the reader reads the result
   unchanged — round-trip is the acceptance test.

## Phases

- **P0 (this branch, first):** transport write mode + unit test against a
  mock is impossible offline → integration test mirroring
  `testing/adios2/engine/bp/xrootd/` (read tests) with a writer variant;
  locally verify against any xrootd stand-in if available, else compile +
  logic review, field-run on Helios via container build.
- **P1:** multi-rank write (aggregator modes) + throughput pass: Flush-per-
  step round-trips, chunk size, `EOS_DATA_MAXOPEN` analogs; interaction
  with the fork's bbstage rerouting layer (writer there buffers per-step —
  good fit: each flush becomes one Sync).
- **P2:** upstream RFC (the DAOS engine is the template reviewers will ask
  for), `.complete` marker support, xrdfs-side mkdir, CI story.

## Acceptance (definition of done for P0)

BP5 Series written with `Library=xrootd` to a scratch prefix on Helios,
then read back **in the same process** via the existing read path
(FileXRootD) → scalars identical to a POSIX-written reference series
(the 0.000e+00 discipline from the apeos chain), and the result readable
by stock tools (`bpls`) through `root://` without any staging.
