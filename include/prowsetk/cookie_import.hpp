#ifndef PROWSETK_COOKIE_IMPORT_HPP
#define PROWSETK_COOKIE_IMPORT_HPP

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace prowsetk {

class CookieJar;

struct CookieImportResult {
    std::size_t imported = 0;
    std::size_t skipped = 0;
    std::vector<std::string> warnings;
};

// Imports browser-exported cookie JSON into `jar`. Supported shapes are an
// array of cookie objects and storage-state style objects with a `cookies`
// array. Cookie values are treated as secret-bearing and are never returned in
// warnings.
CookieImportResult import_cookies_json(CookieJar& jar,
                                       std::string_view contents,
                                       std::string_view default_origin = {});

CookieImportResult import_cookies_json_file(CookieJar& jar,
                                            const std::filesystem::path& path,
                                            std::string_view default_origin = {});

}  // namespace prowsetk

#endif  // PROWSETK_COOKIE_IMPORT_HPP
