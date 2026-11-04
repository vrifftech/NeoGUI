#pragma once

#include <neoshared/ResourceResolver.hpp>
#include <neoshared/game/Installation.hpp>

namespace neogui {
neoshared::ResourceResolver captureGameResources(const neoshared::game::GameInstall& installation);
}
