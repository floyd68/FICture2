#pragma once

#include "IPathByteSource.h"

// Adapts Floar VFS path parsing/reads to ImageCore's IPathByteSource.
class FloarPathByteSource : public ImageCore::IPathByteSource
{
public:
    std::wstring ResolveHostPath(const std::wstring& path) const override;
    std::vector<uint8_t> ReadAll(const std::wstring& path) const override;
};
