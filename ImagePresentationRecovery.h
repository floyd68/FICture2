#pragma once

// What the main image needs to do after a graphics invalidation, decided from
// the presentation's own resource domain rather than the invalidation reason:
//   - an SRV belongs to the D3D device: it survives a target-only recreation
//     (keeping GPU/channel presentation) and is lost on a device change;
//   - a D2D bitmap belongs to the render target and is lost on any target
//     change (the target generation also moves with the device).
// Lost content is re-uploaded from the retained CPU payload when there is one
// (as an SRV again if a D3D device is available), otherwise reloaded.

enum class PresentationKind
{
    None,
    ShaderResource,
    Bitmap,
};

enum class PresentationRecovery
{
    Keep,
    Reupload,
    Reload,
};

// `domainChanged`: the generation of the presentation's domain (device for an
// SRV, target for a bitmap) differs from the one it was created under.
inline PresentationRecovery DecidePresentationRecovery(
    PresentationKind kind,
    bool domainChanged,
    bool hasCpuPayload)
{
    if (kind == PresentationKind::None || !domainChanged)
    {
        return PresentationRecovery::Keep;
    }
    return hasCpuPayload ? PresentationRecovery::Reupload : PresentationRecovery::Reload;
}
