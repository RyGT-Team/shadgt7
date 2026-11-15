// FIX-GT7-175: see ryty_app0.h. Standard library only, so it is unit-tested on its own
// (tests/test_ryty_app0.cpp).

#include "ryty_gpu/ryty_app0.h"

#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <system_error>

namespace Ryty {
namespace {

std::filesystem::path FromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

bool LayersDisabled() {
    const char* value = std::getenv("RYGT_SCE_SYS_LAYERS");
    return value && std::string_view(value) == "0";
}

// The update and base game folders, update first, as AppOverlay's ConfiguredApp0Layers finds them.
std::vector<std::filesystem::path> GameFolders(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> candidates;
#ifdef _WIN32
    const wchar_t* layers = _wgetenv(L"RYGT_APP0_LAYERS");
    if (layers && *layers) {
        const std::wstring list(layers);
        std::size_t start = 0;
        while (start <= list.size()) {
            std::size_t end = list.find(L';', start);
            if (end == std::wstring::npos) {
                end = list.size();
            }
            if (end > start) {
                candidates.emplace_back(list.substr(start, end - start));
            }
            start = end + 1;
        }
    }
#else
    const char* layers = std::getenv("RYGT_APP0_LAYERS");
    if (layers && *layers) {
        const std::string_view list(layers);
        std::size_t start = 0;
        while (start <= list.size()) {
            std::size_t end = list.find(';', start);
            if (end == std::string_view::npos) {
                end = list.size();
            }
            if (end > start) {
                candidates.push_back(FromUtf8(list.substr(start, end - start)));
            }
            start = end + 1;
        }
    }
#endif
    else {
        // The launcher's settings (UTF-8): game= is the base game folder, update= its update.
        std::ifstream ini(root / "RyGT.ini", std::ios::binary);
        std::string line;
        std::filesystem::path game, update;
        while (std::getline(ini, line)) {
            std::string_view text(line);
            if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) {
                text.remove_prefix(3); // BOM
            }
            const auto equals = text.find('=');
            if (equals == std::string_view::npos) {
                continue;
            }
            const auto key = Trim(text.substr(0, equals));
            const auto value = Trim(text.substr(equals + 1));
            if (value.empty()) {
                continue;
            }
            if (key == "game") {
                game = FromUtf8(value);
            } else if (key == "update") {
                update = FromUtf8(value);
            }
        }
        if (!update.empty()) {
            candidates.push_back(update);
        }
        if (!game.empty()) {
            candidates.push_back(game);
        }
    }
    return candidates;
}

std::filesystem::path::string_type Fold(std::filesystem::path::string_type text) {
    for (auto& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<std::filesystem::path::value_type>(c - 'A' + 'a');
        }
    }
    return text;
}

} // namespace

std::vector<std::filesystem::path> App0Layers(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> layers{root / "app0"};
    if (LayersDisabled()) {
        return layers;
    }
    std::error_code error;
    const auto app0 = std::filesystem::weakly_canonical(root / "app0", error);
    for (const auto& candidate : GameFolders(root)) {
        std::error_code folder_error;
        if (!std::filesystem::is_directory(candidate, folder_error)) {
            continue;
        }
        auto canonical = std::filesystem::weakly_canonical(candidate, folder_error);
        if (folder_error) {
            canonical = candidate;
        }
        if (canonical == app0) {
            continue;
        }
        bool duplicate = false;
        for (const auto& layer : layers) {
            duplicate = duplicate || layer == canonical;
        }
        if (!duplicate) {
            layers.push_back(canonical);
        }
    }
    return layers;
}

std::filesystem::path App0File(const std::filesystem::path& root,
                               const std::filesystem::path& relative) {
    const auto layers = App0Layers(root);
    for (const auto& layer : layers) {
        std::error_code error;
        const auto host = layer / relative;
        if (std::filesystem::is_regular_file(host, error)) {
            return host;
        }
    }
    return layers.front() / relative;
}

std::vector<std::filesystem::path> App0ListFiles(const std::filesystem::path& root,
                                                 const std::filesystem::path& relative) {
    std::vector<std::filesystem::path> files;
    std::set<std::filesystem::path::string_type> seen;
    for (const auto& layer : App0Layers(root)) {
        std::error_code error;
        for (std::filesystem::directory_iterator it(layer / relative, error), end;
             !error && it != end; it.increment(error)) {
            std::error_code type_error;
            if (!it->is_regular_file(type_error)) {
                continue;
            }
            if (seen.insert(Fold(it->path().filename().native())).second) {
                files.push_back(it->path());
            }
        }
    }
    return files;
}

} // namespace Ryty
