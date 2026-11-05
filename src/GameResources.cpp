#include "GameResources.hpp"

#include <neoshared/bif/ResourceDocument.hpp>
#include <neoshared/game/OverrideAliases.hpp>
#include <neoshared/material/Material.hpp>
#include <neoshared/texture/FileUtil.hpp>
#include <twoda/TwoDAFile.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>

namespace neogui {
namespace {
using Key = std::pair<std::string, std::uint16_t>;
const std::vector<std::uint16_t> assetTypes{3, 2017, 2022, 2033, 3006, 3007};
const std::map<std::string, std::uint16_t> assetExtensions{
    {".tga", 3}, {".2da", 2017}, {".txi", 2022}, {".dds", 2033}, {".txb", 3006}, {".tpc", 3007}};
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return value;
}
constexpr auto keyLayer = std::numeric_limits<std::size_t>::max();
struct Location {
    std::size_t layer;
    neoshared::bif::ResourceSelection resource;
    std::size_t directory{};
    std::filesystem::path file{};
};
struct GameResources {
    neoshared::bif::KeyBifArchive key;
    std::vector<neoshared::bif::LooseArchiveCatalog> layers;
    std::map<Key, Location> index;

    void add(const std::string& name, std::uint16_t type, Location location) {
        const Key key{neoshared::material::textureResourceKey(name), type};
        const auto found = index.find(key);
        if (found == index.end() || std::pair{location.layer, location.directory} < std::pair{found->second.layer, found->second.directory}) index[key] = location;
    }
    void indexLayer() {
        const auto layer = layers.size() - 1;
        for (const auto& resource : layers.back().resources())
            add(resource.resref, resource.type, {layer, {neoshared::bif::ResourceSource::LooseArchive, resource.index},
                layers.back().archives()[resource.archiveIndex].overridePrecedence});
    }
    std::optional<neoshared::ResourceDocument> resolve(const std::string& name, std::uint16_t type,
                                                     const neoshared::bif::JobControl& job) const {
        const auto normalized = neoshared::material::textureResourceKey(name);
        const auto found = index.find({normalized, type});
        if (found == index.end()) return {};
        if (neoshared::isTextureType(type)) {
            const auto requested = std::find(std::begin(neoshared::textureTypes), std::end(neoshared::textureTypes), type);
            for (auto preferred = std::begin(neoshared::textureTypes); preferred != std::end(neoshared::textureTypes); ++preferred)
                if (const auto other = index.find({normalized, *preferred}); other != index.end() &&
                    std::tuple{other->second.layer, other->second.directory, preferred} <
                    std::tuple{found->second.layer, found->second.directory, requested}) return {};
        }
        const auto& location = found->second;
        if (location.layer == 0) {
            job.check();
            auto result = neoshared::ResourceDocument{
                neoshared::genericPathToUtf8(location.file), neoshared::pathToUtf8(location.file.filename()),
                neoshared::genericPathToUtf8(location.file), type,
                neoshared::texture::readFileBytes(location.file), {location.file}};
            result.sourcePath = location.file;
            result.priority = neoshared::ResourcePriority::Directory;
            return result;
        }
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
        return found == directoryAliases.end() ? root / name : found->second;
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
    for (auto& entry : overrides) entry.maximumDepth = k1 ? 0 : 1;
    std::vector<aliases::OverrideRoot> roots;
    if (k1) for (const auto* name : {"streammusic", "streamwaves", "movies", "portraits", "servervault", "rims", "errortex"})
        roots.push_back({name, directory(name), {}, 0, false, 0});
    else for (const auto* name : {"movies", "rims"}) roots.push_back({name, directory(name), {}, 0, false, 0});
    roots.insert(roots.end(), overrides.begin(), overrides.end());
    roots.push_back({"tempclient", directory("tempclient"), {}, 0, false, 0});
    for (std::size_t i = 0; i < roots.size(); ++i) roots[i].precedence = i;
    resources->layers.emplace_back();
    for (const auto& directory : roots) {
        std::map<std::string, std::filesystem::path> files;
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator entry(directory.path, error), end;
             entry != end; entry.increment(error)) {
            if (entry->is_directory()) {
                if (directory.maximumDepth >= 0 && entry.depth() >= directory.maximumDepth)
                    entry.disable_recursion_pending();
            } else if (assetExtensions.count(lower(neoshared::pathToUtf8(entry->path().extension()))) && entry->is_regular_file()) {
                files.emplace(lower(neoshared::genericPathToUtf8(entry->path())), entry->path());
            }
        }
        if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory)
            throw std::filesystem::filesystem_error("Cannot enumerate GUI assets", directory.path, error);
        for (const auto& [name, path] : files)
            resources->add(neoshared::pathToUtf8(path.stem()), assetExtensions.at(lower(neoshared::pathToUtf8(path.extension()))),
                           {0, {}, directory.precedence, path});
    }

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
    if (k1) addArchive(root / "override" / "textures.erf");
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
