#ifndef PREPAREDMATCHTEMPLATES_H
#define PREPAREDMATCHTEMPLATES_H

#include "preparedmatchsettings.h"
#include "scenariotemplates.h"
#include <array>
#include <filesystem>
#include <optional>

namespace hooks::prepared {

inline std::string asciiLower(std::string value)
{
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}

struct TemplateVersion {
    std::array<unsigned, 6> parts{};
    unsigned revision{};
    unsigned release{3}; // alpha < beta < rc < release; letters are legacy revisions.
    unsigned prerelease{};
};

inline int compareTemplateVersions(const TemplateVersion& a, const TemplateVersion& b)
{
    if (a.parts != b.parts) return a.parts < b.parts ? -1 : 1;
    if (a.release != b.release) return a.release < b.release ? -1 : 1;
    if (a.prerelease != b.prerelease) return a.prerelease < b.prerelease ? -1 : 1;
    if (a.revision != b.revision) return a.revision < b.revision ? -1 : 1;
    return 0;
}

// Recognize only explicit versions. Unknown suffixes (e.g. 3.3K(2))
// are NOT discarded: they can identify a fork rather than a newer release.
inline std::optional<TemplateVersion> parseTemplateVersion(const std::string& value)
{
    if (value.empty()) return {};
    std::size_t at = value[0] == 'v' ? 1 : 0;
    auto number = [&](unsigned& out) {
        const auto begin = at;
        while (at < value.size() && value[at] >= '0' && value[at] <= '9') {
            if (at - begin >= 9) return false;
            out = out * 10 + value[at++] - '0';
        }
        return at != begin;
    };
    TemplateVersion result;
    unsigned count{};
    do {
        if (count == result.parts.size() || !number(result.parts[count++])) return {};
        if (at == value.size() || value[at] != '.') break;
        ++at;
    } while (true);
    if (at < value.size() && value[at] >= 'a' && value[at] <= 'z')
        result.revision = value[at++] - 'a' + 1;
    if (at < value.size()) {
        if (value[at] != ' ' && value[at] != '_' && value[at] != '-') return {};
        while (at < value.size() && (value[at] == ' ' || value[at] == '_' || value[at] == '-')) ++at;
        const auto begin = at;
        while (at < value.size() && value[at] >= 'a' && value[at] <= 'z') ++at;
        const auto label = value.substr(begin, at - begin);
        if (label == "alpha") result.release = 0;
        else if (label == "beta") result.release = 1;
        else if (label == "rc") result.release = 2;
        else return {};
        if (at < value.size() && !number(result.prerelease)) return {};
    }
    if (at != value.size()) return {};
    return result;
}

struct TemplateFamily {
    std::string name;
    std::optional<TemplateVersion> version;
};

inline std::string trimTemplateWhitespace(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n\f\v") - first + 1);
}

inline TemplateFamily templateFamily(const std::string& value)
{
    // Same suffix grammar for title and filename stem (each number <= 9 digits):
    // [ _-]+v?(digits(.digits){0,5})([a-z]?)([ _-]+(alpha|beta|rc)(digits*))?$
    // Preserve variant words/punctuation and internal whitespace. Only ASCII trim/casefold.
    const auto name = trimTemplateWhitespace(value);
    for (std::size_t i = 1; i + 1 < name.size(); ++i) {
        if (name[i] != ' ' && name[i] != '_' && name[i] != '-') continue;
        auto end = i + 1;
        while (end < name.size() && (name[end] == ' ' || name[end] == '_' || name[end] == '-')) ++end;
        if (auto version = parseTemplateVersion(name.substr(end)))
            return {asciiLower(trimTemplateWhitespace(name.substr(0, i))), version};
        i = end - 1;
    }
    return {asciiLower(name), {}};
}

struct LocalTemplateSelection {
    const ScenarioTemplate* cached{};
    std::string filename;
    rsg::MapTemplateSettings settings;
};

// Only cached metadata is inspected. No Lua execution, disk read, native UI or
// catalog mutation. localTitle/localFilename have already been converted to CP1251.
inline LocalTemplateSelection selectLocalTemplate(const Offer& offer, const ScenarioTemplates& catalog,
                                                 const std::string& localTitle,
                                                 const std::string& localFilename)
{
    const auto display = templateFamily(localTitle);
    const auto file = templateFamily(std::filesystem::path(localFilename).stem().string());
    const bool latest = display.version.has_value();
    const ScenarioTemplate* selected{};
    std::string selectedFilename;
    std::optional<TemplateVersion> selectedVersion;
    bool ambiguous{};
    for (const auto& item : catalog) {
        const auto basename = std::filesystem::path(item.filename).filename().string();
        const auto candidate = templateFamily(item.settings.name);
        if (latest) {
            const auto candidateFile = templateFamily(std::filesystem::path(basename).stem().string());
            if (!candidate.version || candidate.name != display.name || candidateFile.name != file.name) continue;
        } else if (basename != localFilename) continue;
        const int order = selected && latest ? compareTemplateVersions(*candidate.version, *selectedVersion) : 0;
        if (!selected || order > 0) {
            selected = &item;
            selectedFilename = basename;
            selectedVersion = candidate.version;
            ambiguous = false;
        } else if (order == 0) ambiguous = true;
    }
    if (!selected) throw std::runtime_error("missing-template");
    if (ambiguous) throw std::runtime_error("ambiguous-template");
    // Validate the chosen latest version without silently falling back, clamping
    // agreed values, or comparing uploaded Lua hashes/positional spin labels.
    return {selected, std::move(selectedFilename), applySettings(offer, selected->settings)};
}

} // namespace hooks::prepared
#endif // PREPAREDMATCHTEMPLATES_H
