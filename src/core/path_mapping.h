#pragma once

#include "core/config.h"
#include <min_paths.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <algorithm>

namespace MR {

/// Returns "win", "mac", or "lin" for the current OS.
inline std::string currentOsTag()
{
#if defined(_WIN32)
    return "win";
#elif defined(__APPLE__)
    return "mac";
#else
    return "lin";
#endif
}

/// Get the path prefix for the given OS tag from a PathMapping.
inline const std::string& mappingPrefix(const PathMapping& m, const std::string& os)
{
    if (os == "win") return m.win;
    if (os == "mac") return m.mac;
    return m.lin;
}

/// Normalize a path to forward slashes for comparison.
inline std::string normalizePath(const std::string& path)
{
    std::string result = path;
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

/// Convert a path to native OS separators.
inline std::string toNativeSeparators(const std::string& path, const std::string& os)
{
    std::string result = path;
    if (os == "win")
        std::replace(result.begin(), result.end(), '/', '\\');
    else
        std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

/// A string from min-paths' C ABI, freed here.
inline std::string takeMp(char* s)
{
    if (!s) return {};
    std::string out(s);
    mp_free(s);
    return out;
}

/// Translate a path from sourceOS to targetOS using the given mappings — min-paths `map::translate` (minplatform canon
/// §22, row 121): the first enabled mapping with both roots set wins, a root matches only on a COMPONENT BOUNDARY
/// (`/Vol/share` no longer matches `/Vol/share-extra` — the check this header lacked), Windows sources compare
/// case-insensitively with the drive letter stripped, the result takes the target OS's separators. No mapping matched
/// (or an unknown OS tag) → separators swapped only, as before.
inline std::string translatePath(
    const std::string& sourceOS,
    const std::string& targetOS,
    const std::string& path,
    const std::vector<PathMapping>& mappings)
{
    const std::string rows = nlohmann::json(mappings).dump();
    return takeMp(mp_translate_with(sourceOS.c_str(), targetOS.c_str(), path.c_str(), rows.c_str()));
}

/// Translate a path from the current OS form to canonical Windows form.
/// On Windows this is a no-op (separator normalization only). Used at
/// the submission boundary on macOS to canonicalize manifest paths
/// before persisting to SQLite or POSTing to the leader.
inline std::string toCanonicalPath(
    const std::string& path,
    const std::vector<PathMapping>& mappings)
{
    return translatePath(currentOsTag(), "win", path, mappings);
}

/// Translate a canonical Windows path back to the current OS form.
/// On Windows this is a no-op. Used at UI display boundaries on macOS
/// so /Volumes/... appears in panels instead of Z:\... .
inline std::string fromCanonicalPath(
    const std::string& path,
    const std::vector<PathMapping>& mappings)
{
    return translatePath("win", currentOsTag(), path, mappings);
}

} // namespace MR
