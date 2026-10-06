#pragma once
#include <cstddef>
#include <filesystem>
#include <string>

namespace prowsetk::qute {
std::string read_fifo(const std::filesystem::path& path, std::size_t size);
}
