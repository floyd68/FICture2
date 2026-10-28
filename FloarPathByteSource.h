#pragma once

#include "IPathByteSource.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>

class ArchiveEntryHandoff;

namespace Floar
{
class ArchiveReaderCache;
}

// Filesystem queries used by FloarPathByteSource::ClassifyPath. System()
// calls the Win32 functions; tests inject a fake to count and fake them.
class PathClassificationQueries
{
public:
    virtual ~PathClassificationQueries() = default;

    // GetDriveTypeW(root), root like L"C:\\" (a DRIVE_* value).
    virtual std::uint32_t DriveType(const std::wstring& root) const = 0;
    // GetFileAttributesW(path); INVALID_FILE_ATTRIBUTES on failure.
    virtual std::uint32_t FileAttributes(const std::wstring& path) const = 0;

    static const PathClassificationQueries& System();
};

// Host-side path classification, shared by FloarPathByteSource::ClassifyPath
// (ImageCore's I/O lanes) and the shell thumbnail routing of online-only cloud
// files (CloudThumbnailRouter). Only cheap signals, synchronously, in this order:
// - Archive: the path parses as a Floar::VirtualPath inside an archive
//   (lexical; a folder whose name ends in an archive extension counts, as it
//   does for Floar's reads).
// - Remote: a UNC path (lexical), or a drive whose GetDriveTypeW is
//   DRIVE_REMOTE. The drive type is cached per drive letter for the life of
//   the classifier (at most one query per letter).
// - CloudPlaceholder: only on a DRIVE_FIXED drive, GetFileAttributesW reports
//   FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS or FILE_ATTRIBUTE_OFFLINE (a metadata
//   query that does not hydrate the file). Remote and UNC paths never reach a
//   filesystem query.
// - Local: any other file on a fixed drive.
// - Unknown: an empty path, another drive type, no drive letter, or a failing
//   query.
// Accepted risk: GetDriveTypeW and GetFileAttributesW have no documented time
// bound, so the first query of a drive letter (for example a disconnected
// mapped drive) and the attribute query on a local fixed drive can take a
// while on the caller's thread. Classification never queries a remote or UNC
// path.
enum class HostPathKind
{
    Unknown,
    Local,
    Archive,
    Remote,
    CloudPlaceholder,
};

class HostPathClassifier
{
public:
    // Classifies with `queries` (which must outlive the classifier), or with
    // PathClassificationQueries::System() when null.
    explicit HostPathClassifier(const PathClassificationQueries* queries = nullptr)
        : m_queries(queries)
    {
    }

    HostPathKind Classify(const std::wstring& path) const;

private:
    const PathClassificationQueries& Queries() const;
    std::uint32_t DriveTypeOf(wchar_t letter) const;

    const PathClassificationQueries* m_queries { nullptr };

    // GetDriveTypeW results by drive letter (A..Z), guarded by m_driveMutex.
    // The query runs under the mutex, so each letter is queried at most once.
    mutable std::mutex m_driveMutex;
    mutable std::array<std::optional<std::uint32_t>, 26> m_driveTypes {};
};

// Adapts Floar VFS path parsing/reads to ImageCore's IPathByteSource.
//
// Reads go through VirtualFileSystem::ReadFile with the request's stop token
// and the archive reader cache given at construction (default
// ArchiveReaderCache::Shared(); tests inject one with their own opener). Once
// the token is requested the read returns empty and nothing partial: no open,
// extraction or file open starts after a pre-canceled token, a caller waiting
// for a shared archive open leaves at once (the open itself stops only when no
// other caller waits for it), and an extraction stops at its next block or
// chunk boundary. ImageCore discards the result of a stopped read and never
// decodes or re-reads the request (no path fallback).
//
// Archive entries go through an ArchiveEntryHandoff (default
// ArchiveEntryHandoff::Shared(), where image bindings register their
// demands): a demanded thumbnail in a solid 7z/RAR is served from parked bytes
// or by a forward pass that also parks the other demanded thumbnails before
// it, a main image is read at High cursor priority, and every other read is
// VirtualFileSystem::ReadFile as before.
//
// Path classification (ImageCore's I/O lanes, floyd68/ImageCore#15) is
// HostPathClassifier's, with a cloud placeholder reported as Remote and
// Unknown as ImageCore's conservative default (scheduled as Local). The
// accepted risk documented there applies on ImageCore's Enqueue thread.
class FloarPathByteSource : public ImageCore::IPathByteSource
{
public:
    // Uses ArchiveReaderCache::Shared(), looked up per read, and ArchiveEntryHandoff::Shared().
    FloarPathByteSource() = default;
    // Uses `cache`, which must outlive this source, and ArchiveEntryHandoff::Shared().
    explicit FloarPathByteSource(Floar::ArchiveReaderCache& cache);
    // Uses `cache` and `handoff`, which must outlive this source.
    FloarPathByteSource(Floar::ArchiveReaderCache& cache, ArchiveEntryHandoff& handoff);
    // Classifies with `queries` (which must outlive this source); reads as the default.
    explicit FloarPathByteSource(const PathClassificationQueries& queries);

    std::wstring ResolveHostPath(const std::wstring& path) const override;
    // Untokened read (legacy callers): not interruptible.
    std::vector<uint8_t> ReadAll(const std::wstring& path) const override;
    std::vector<uint8_t> ReadAll(const std::wstring& path, std::stop_token stop) const override;
    // See the class comment.
    ImageCore::PathSourceKind ClassifyPath(const std::wstring& path) const override;

private:
    Floar::ArchiveReaderCache& Cache() const;
    ArchiveEntryHandoff& Handoff() const;

    Floar::ArchiveReaderCache* m_cache { nullptr };
    ArchiveEntryHandoff* m_handoff { nullptr };
    HostPathClassifier m_classifier {};
};
