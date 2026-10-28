#pragma once

#include "IPathByteSource.h"

#include <stop_token>

class ArchiveEntryHandoff;

namespace Floar
{
class ArchiveReaderCache;
}

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
class FloarPathByteSource : public ImageCore::IPathByteSource
{
public:
    // Uses ArchiveReaderCache::Shared(), looked up per read, and ArchiveEntryHandoff::Shared().
    FloarPathByteSource() = default;
    // Uses `cache`, which must outlive this source, and ArchiveEntryHandoff::Shared().
    explicit FloarPathByteSource(Floar::ArchiveReaderCache& cache);
    // Uses `cache` and `handoff`, which must outlive this source.
    FloarPathByteSource(Floar::ArchiveReaderCache& cache, ArchiveEntryHandoff& handoff);

    std::wstring ResolveHostPath(const std::wstring& path) const override;
    // Untokened read (legacy callers): not interruptible.
    std::vector<uint8_t> ReadAll(const std::wstring& path) const override;
    std::vector<uint8_t> ReadAll(const std::wstring& path, std::stop_token stop) const override;

private:
    Floar::ArchiveReaderCache& Cache() const;
    ArchiveEntryHandoff& Handoff() const;

    Floar::ArchiveReaderCache* m_cache { nullptr };
    ArchiveEntryHandoff* m_handoff { nullptr };
};
