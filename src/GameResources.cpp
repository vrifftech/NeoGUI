#include "GameResources.hpp"

#include <neoshared/bif/ResourceDocument.hpp>
#include <neoshared/game/OverrideAliases.hpp>
#include <neoshared/material/Material.hpp>
#include <twoda/TwoDAFile.hpp>
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>

namespace neogui {
namespace {
using Key = std::pair<std::string, std::uint16_t>;
const std::vector<std::uint16_t> assetTypes{3, 2017, 2022, 2033, 2059, 3007};
constexpr auto keyLayer = std::numeric_limits<std::size_t>::max();
struct Location {
    std::size_t layer;
    neoshared::bif::ResourceSelection resource;
    neoshared::ResourcePriority priority{neoshared::ResourcePriority::Key};
};
struct GameResources {
    neoshared::bif::KeyBifArchive key;
    std::vector<neoshared::bif::LooseArchiveCatalog> layers;
    std::map<Key, Location> index;

    void add(const std::string& name, std::uint16_t type, Location location) {
        const Key key{neoshared::material::textureResourceKey(name), type};
        const auto found = index.find(key);
        if (found == index.end() || location.layer < found->second.layer) index[key] = location;
    }
    void indexLayer() {
        const auto layer = layers.size() - 1;
        for (const auto& resource : layers.back().resources()) {
            if (std::find(assetTypes.begin(), assetTypes.end(), resource.type) == assetTypes.end()) continue;
            const auto& archive = layers.back().archives()[resource.archiveIndex];
            const auto priority = resource.overrideFile ? neoshared::ResourcePriority::Directory :
                archive.kind == neoshared::bif::LooseArchiveKind::Rim ? neoshared::ResourcePriority::Image : neoshared::ResourcePriority::Archive;
            add(resource.resref, resource.type, {layer, {neoshared::bif::ResourceSource::LooseArchive, resource.index}, priority});
        }
    }
    std::optional<neoshared::ResourceDocument> resolve(const std::string& name, std::uint16_t type,
                                                     const neoshared::bif::JobControl& job) const {
        const auto normalized = neoshared::material::textureResourceKey(name);
        const auto found = index.find({normalized, type});
        if (found == index.end()) return {};
        if (neoshared::isTextureType(type)) {
            for (const auto format : neoshared::kotorTextureTypes)
                if (const auto other = index.find({normalized, format}); other != index.end() &&
                    neoshared::texturePreference(other->second.priority, format) <
                    neoshared::texturePreference(found->second.priority, type)) return {};
        }
        const auto& location = found->second;
        return neoshared::bif::readResourceDocument(key,
            layers[location.layer == keyLayer ? 0 : location.layer], location.resource, job);
    }
};
}

neoshared::ResourceResolver captureGameResources(const neoshared::game::GameInstall& installation) {
    namespace game = neoshared::game;
    namespace aliases = neoshared::override_aliases;
    auto resources = std::make_shared<GameResources>();
    const bool k1 = installation.id == "kotor";
    const auto root = game::normalizeConfiguredPath(installation.installPath);
    const auto variant = k1 ? aliases::Game::Kotor : aliases::Game::Kotor2;
    const auto keyPath = game::findExistingPathCaseInsensitive(root / "chitin.key");
    if (!resources->key.open(keyPath, {}, {}, {}, k1, assetTypes)) throw std::runtime_error(resources->key.lastError());
    for (const auto& resource : resources->key.resources())
        resources->add(resource.resref, resource.type, {keyLayer, {neoshared::bif::ResourceSource::KeyBif, resource.index}});

    const auto directoryAliases = aliases::directoryAliases(root, variant);
    const auto directory = [&](const std::string& name) {
        const auto found = directoryAliases.find(name);
        return found == directoryAliases.end() ? game::findExistingPathCaseInsensitive(root / name) : found->second;
    };
    auto overrides = aliases::discover(root, variant);
    if (!k1) {
        std::ifstream talkTable(installation.tlkPath, std::ios::binary);
        talkTable.exceptions(std::ios::failbit | std::ios::badbit);
        talkTable.seekg(8);
        std::array<unsigned char, 4> bytes{};
        talkTable.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        const auto language = std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
            (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
        const std::array<const char*, 6> names{{"English", "French", "German", "Italian", "Spanish", "English"}};
        std::vector<aliases::OverrideRoot> layered;
        for (const auto& content : aliases::contentRoots(root, variant, names[language < names.size() ? language : 0]))
            if (const auto path = game::findExistingPathCaseInsensitive(content / "override"); !path.empty())
                layered.push_back({neoshared::pathToUtf8(content), path, {}, layered.size(), false});
        layered.insert(layered.end(), overrides.begin(), overrides.end());
        overrides = std::move(layered);
    }
    if (!installation.overridePath.empty() && std::none_of(overrides.begin(), overrides.end(), [&](const auto& entry) {
        return entry.path.lexically_normal() == installation.overridePath.lexically_normal();
    })) overrides.insert(overrides.begin(), {"CONFIGURED OVERRIDE", installation.overridePath, {}, 0, false});
    for (auto& entry : overrides) entry.maximumDepth = aliases::overrideDepth(variant);
    std::vector<aliases::OverrideRoot> roots;
    if (k1) for (const auto* name : {"streammusic", "streamwaves", "movies", "portraits", "servervault", "rims", "errortex"})
        roots.push_back({name, directory(name), {}, 0, false, 0});
    else for (const auto* name : {"movies", "rims"}) roots.push_back({name, directory(name), {}, 0, false, 0});
    roots.insert(roots.end(), overrides.begin(), overrides.end());
    roots.push_back({"tempclient", directory("tempclient"), {}, 0, false, 0});
    for (std::size_t i = 0; i < roots.size(); ++i) roots[i].precedence = i;
    auto& directories = resources->layers.emplace_back();
    if (!directories.openFiles(root, {}) || !directories.addOverrideDirectories(roots, {}, {}, true))
        throw std::runtime_error(directories.lastError());
    resources->indexLayer();

    const auto addArchive = [&](const std::filesystem::path& candidate) {
        const auto path = game::findExistingPathCaseInsensitive(candidate);
        if (path.empty()) return;
        resources->layers.emplace_back();
        auto& archive = resources->layers.back();
        if (!archive.openFiles(root, {path})) throw std::runtime_error(archive.lastError());
        if (archive.invalidArchiveCount()) throw std::runtime_error(archive.messages().front());
        resources->indexLayer();
    };
    addArchive(root / "patch.erf");
    if (k1) addArchive(directory("override") / "textures.erf");
    addArchive(directory("rims") / "global.rim");

    neo2da::TwoDAFile packs;
    packs.loadBytes(resources->resolve("texpacks", 2017, {}).value().bytes);
    for (const auto* column : {"Gui", "Texture"}) {
        const auto& name = packs.sourceCell(2, packs.columnIndexByLabel(column));
        if (!name.empty()) addArchive(directory("texturepacks") / (name + ".erf"));
    }
    return [resources = std::move(resources)](const std::string& name, std::uint16_t type,
                                             const neoshared::bif::JobControl& job) {
        return resources->resolve(name, type, job);
    };
}
}
