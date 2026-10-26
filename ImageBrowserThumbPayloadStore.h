#pragma once

#include "ImageAsyncBinding.h"

#include <string>
#include <unordered_map>

// Decoded thumbnail payloads retained per item, keyed by the item's normalized
// path rather than by a tile. A tile retains its payload when it presents it;
// a tile bound to the item later (a recycled tile, or the item scrolling back
// into the materialized range) rebinds from it without a new ImageCore request.
// Only target-bound bitmaps are rebuilt.
//
// Unbounded, like the per-tile payloads on every tile it replaces (#39 bounds
// it); cleared when the strip lists another folder. The payload blocks are
// shared with the tile that presents them, not copied. UI thread only.
class ThumbPayloadStore
{
public:
    using Payload = ImageAsyncBinding::Payload;

    void Retain(const Payload& payload)
    {
        if (payload.blocks && !payload.sourcePath.empty())
        {
            m_payloads[payload.sourcePath] = payload;
        }
    }

    // The retained payload of normalized path `path`, or nullptr.
    const Payload* Find(const std::wstring& path) const
    {
        const auto it = m_payloads.find(path);
        return (it != m_payloads.end()) ? &it->second : nullptr;
    }

    // Keeps the payloads when `folderKey` is the folder they were retained
    // for (a rebuild of the same listing), and drops them otherwise.
    void ResetForFolder(const std::wstring& folderKey)
    {
        if (folderKey != m_folderKey)
        {
            m_folderKey = folderKey;
            m_payloads.clear();
        }
    }

    size_t Count() const { return m_payloads.size(); }

private:
    std::wstring m_folderKey {};
    std::unordered_map<std::wstring, Payload> m_payloads {};
};
