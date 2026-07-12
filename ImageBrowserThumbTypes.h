#pragma once

#include "VirtualPath.h"

#include <memory>

namespace FD2D
{
    class Wnd;
    class ThumbImage;
}

class ThumbNavTile;
class ThumbImageTile;

enum class ThumbItemKind
{
    Up,
    Folder,
    Image
};

struct ThumbItem
{
    ThumbItemKind kind { ThumbItemKind::Image };
    Floar::VirtualPath path {};
    std::shared_ptr<FD2D::Wnd> focus {};
    std::shared_ptr<FD2D::ThumbImage> image {};
    std::shared_ptr<ThumbNavTile> navTile {};
    std::shared_ptr<ThumbImageTile> imageTile {};
};
