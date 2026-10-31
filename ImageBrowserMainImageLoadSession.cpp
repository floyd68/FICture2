#include "ImageBrowserMainImageLoadSession.h"

void MainImageLoadSession::SetSource(const std::wstring& path)
{
    m_path = path;
    m_request.source = path;
    m_request.mipLevel = 0;
}

void MainImageLoadSession::Clear()
{
    m_path.clear();
    m_request.source.clear();
    ResetMip();
}

void MainImageLoadSession::ResetMip()
{
    m_mipAfterReload = 0;
    m_selectingMip = false;
    m_request.mipLevel = 0;
}

void MainImageLoadSession::BeginReload()
{
    ResetCpuDecode();
    m_selectingMip = false;
    m_request.mipLevel = 0;
}

void MainImageLoadSession::BeginMipSelection(uint32_t mip)
{
    m_request.mipLevel = mip;
    m_selectingMip = true;
    ResetCpuDecode();
}

uint32_t MainImageLoadSession::TakeMipAfterReload(uint32_t shownMip)
{
    if (m_mipAfterReload == 0 || shownMip != 0)
    {
        return 0;
    }
    const uint32_t mip = m_mipAfterReload;
    m_mipAfterReload = 0;
    return mip;
}

bool MainImageLoadSession::FailMipSelection(uint32_t shownMip)
{
    if (!m_selectingMip)
    {
        return false;
    }
    m_selectingMip = false;
    m_request.mipLevel = shownMip;
    return true;
}

const ImageCore::ImageRequest& MainImageLoadSession::PrepareRequest(const ImageSourceIdentity& identity, bool gpuAvailable)
{
    m_request.source = m_path;
    m_request.purpose = ImageCore::ImagePurpose::FullResolution;
    // The mip is owned by SetSource / BeginMipSelection / BeginReload.
    m_requestIdentity = identity;
    m_request.allowGpuCompressedDDS = gpuAvailable && !CpuDecodeForced();
    return m_request;
}
