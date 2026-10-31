#pragma once

#include "ImageCore/ImageRequest.h"
#include "ImageSourceIdentity.h"

#include <atomic>
#include <cstdint>
#include <string>

// What the main image is loading, without UI objects: the requested source
// and mip, the decode request built for it and the source identity recorded
// with it, whether decoding falls back to the CPU (a GPU upload failed),
// whether the request is a mip selection (a failure returns to the shown mip),
// and the mip to select once a replaced source has been reloaded at mip 0.
// The async binding, the staged cache hit, the presented path and the shown
// content stay with the control.
class MainImageLoadSession
{
public:
    // The requested source (empty: none).
    const std::wstring& Path() const { return m_path; }
    uint32_t RequestedMip() const { return m_request.mipLevel; }
    // The source's identity when the current decode was requested.
    const ImageSourceIdentity& RequestIdentity() const { return m_requestIdentity; }
    bool SelectingMip() const { return m_selectingMip; }
    // The mip waiting for a reload to present mip 0 (0 = none).
    uint32_t MipAfterReload() const { return m_mipAfterReload; }

    // A new requested source, at mip 0. The mip selection state is left to
    // ResetMip(), the CPU-decode fallback to ResetCpuDecode().
    void SetSource(const std::wstring& path);
    // No source: also drops the requested mip, the selection and the pending
    // mip after reload.
    void Clear();
    // Back to mip 0 with no selection and no pending mip after reload.
    void ResetMip();
    // The source is decoded again from mip 0, the GPU path allowed again. A
    // pending mip after reload is kept (SelectMip records it before reloading).
    void BeginReload();
    // Requests `mip` of the current source as a mip selection.
    void BeginMipSelection(uint32_t mip);
    // Selects `mip` once a reload has presented mip 0 (0 = none).
    void SetMipAfterReload(uint32_t mip) { m_mipAfterReload = mip; }
    // The pending mip once mip 0 is shown (`shownMip` == 0), which is then
    // dropped; 0 while nothing is pending or another mip is shown.
    uint32_t TakeMipAfterReload(uint32_t shownMip);

    // Content was presented: a mip selection is over.
    void EndMipSelection() { m_selectingMip = false; }
    // The request failed. A mip selection goes back to `shownMip` and returns
    // true; any other request returns false.
    bool FailMipSelection(uint32_t shownMip);

    bool CpuDecodeForced() const { return m_forceCpuDecode.load(); }
    // A GPU upload failed: decode to CPU pixels from now on.
    void ForceCpuDecode() { m_forceCpuDecode.store(true); }
    void ResetCpuDecode() { m_forceCpuDecode.store(false); }

    // The decode request for the current source and mip, recording `identity`
    // (the source as it is now). GPU-compressed DDS is allowed only with a D3D
    // device (`gpuAvailable`) and while CPU decoding is not forced.
    const ImageCore::ImageRequest& PrepareRequest(const ImageSourceIdentity& identity, bool gpuAvailable);

private:
    std::wstring m_path {};
    ImageCore::ImageRequest m_request {};
    ImageSourceIdentity m_requestIdentity {};
    std::atomic<bool> m_forceCpuDecode { false };
    bool m_selectingMip { false };
    uint32_t m_mipAfterReload { 0 };
};
