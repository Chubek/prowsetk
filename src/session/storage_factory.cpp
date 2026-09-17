#include "prowsetk/storage.hpp"

#include "tokyocabinet_storage.cpp"

namespace prowsetk {

std::unique_ptr<Storage> make_tcb_storage(const std::filesystem::path& base_path) {
    return std::make_unique<TCBStorage>(base_path);
}

std::unique_ptr<Storage> make_encrypted_storage(std::unique_ptr<Storage> backend,
                                                 const std::string& password) {
    return std::make_unique<EncryptedStorage>(std::move(backend), password);
}

}  // namespace prowsetk