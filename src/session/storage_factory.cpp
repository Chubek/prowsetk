#include "prowsetk/storage.hpp"

namespace prowsetk {

std::unique_ptr<Storage> make_tcb_storage(const std::filesystem::path& base_path) {
    return std::make_unique<TCBStorage>(base_path);
}

}  // namespace prowsetk
