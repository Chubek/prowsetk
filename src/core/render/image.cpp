// image.cpp — images stratum of the graphical core.
//
// Decodes raster images with `third_party/stb` and caches the result across
// repaints. Two rules make this safe to have in the core:
//
//  1. The engine never fetches. Decoding is a pure function of bytes, and
//     retrieval is a caller-supplied `ImageLoader` that applies the host's
//     session policy (cookies, redirects, TLS verification, response limits).
//     A render with no loader decodes nothing and every image degrades to its
//     alt text.
//  2. Decoding is bounded. A corrupt or oversized image cannot allocate an
//     unbounded buffer: the byte budget is enforced before decoding and the
//     pixel budget before stb commits any row memory.

#include <prowsetk/error.hpp>
#include <prowsetk/render.hpp>

#include <cstdint>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#if PROWSETK_HAVE_STB
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO            // no file loader: bytes only
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_FAILURE_STRINGS  // no page-controlled strings on error paths
#include <stb_image.h>
#endif

namespace prowsetk {
namespace {

#if PROWSETK_HAVE_STB
// stb reports dimensions before it allocates, so the pixel budget is enforced
// here, ahead of any row memory being committed.
stbi_uc* decode_with_budget(const std::uint8_t* data, std::size_t length,
                            int& out_width, int& out_height) {
    if (length > static_cast<std::size_t>(INT32_MAX)) return nullptr;
    int width = 0, height = 0, channels = 0;
    if (stbi_info_from_memory(data, static_cast<int>(length), &width, &height,
                              &channels) == 0 ||
        width <= 0 || height <= 0 || channels <= 0) {
        return nullptr;
    }
    if (static_cast<long long>(width) * height >
        static_cast<long long>(std::min(max_render_image_pixels, max_render_image_bytes / 4))) {
        return nullptr;
    }
    void* decoded = stbi_load_from_memory(data, static_cast<int>(length), &width,
                                          &height, &channels, 4);
    if (decoded == nullptr) return nullptr;
    if (static_cast<long long>(width) * height >
        static_cast<long long>(max_render_image_pixels)) {
        stbi_image_free(decoded);
        return nullptr;
    }
    out_width = width;
    out_height = height;
    return static_cast<stbi_uc*>(decoded);
}
#endif

}  // namespace

std::shared_ptr<const ImageBitmap> ImageBitmap::decode(
    std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return nullptr;
#if PROWSETK_HAVE_STB
    if (bytes.size() > max_render_image_bytes) return nullptr;
    int width = 0, height = 0;
    const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> storage(
        decode_with_budget(bytes.data(), bytes.size(), width, height), &stbi_image_free);
    const auto* decoded = storage.get();
    if (decoded == nullptr || width <= 0 || height <= 0) return nullptr;
    // Copy out of stb's buffer so the bitmap owns its pixels and the decoder may
    // be recompiled with a different configuration without invalidating it.
    const std::size_t count = static_cast<std::size_t>(width) *
                               static_cast<std::size_t>(height) * 4u;
    if (count > max_render_image_bytes) {
        return nullptr;
    }
    struct Bitmap final : ImageBitmap {
        Bitmap(unsigned w, unsigned h, std::vector<std::uint8_t> pixels)
            : ImageBitmap(w, h, std::move(pixels)) {}
    };
    try {
        return std::make_shared<Bitmap>(static_cast<unsigned>(width), static_cast<unsigned>(height),
                                       std::vector<std::uint8_t>(decoded, decoded + count));
    } catch (const std::bad_alloc&) { return nullptr; }
#else
    // Without stb the engine reports the absence rather than pretending: the
    // paint stratum falls back to alt text, and web_platform() says so too.
    (void)bytes;
    return nullptr;
#endif
}

// --- ImageCache --------------------------------------------------------------

struct ImageCache::Impl {
    struct Entry {
        // null records a failure, so a broken image is not refetched every frame.
        std::shared_ptr<const ImageBitmap> bitmap;
        std::size_t bytes = 0;
    };
    std::unordered_map<std::string, Entry> entries;
    std::size_t total_bytes = 0;
    std::size_t dropped = 0;

    void insert(std::string key, std::shared_ptr<const ImageBitmap> bitmap,
                std::size_t bytes) {
        if (bytes > max_render_image_bytes - total_bytes || entries.size() >= max_render_images) {
            ++dropped;
            return;
        }
        entries.emplace(std::move(key), Entry{std::move(bitmap), bytes});
        total_bytes += bytes;
    }
};

ImageCache::ImageCache() : impl_(std::make_unique<Impl>()) {}
ImageCache::~ImageCache() = default;

std::shared_ptr<const ImageBitmap> ImageCache::get(ImageLoader& loader,
                                                   std::string_view absolute_url) {
    if (absolute_url.empty()) return nullptr;
    if (absolute_url.size() > 8192) { ++impl_->dropped; return nullptr; }
    const auto found = impl_->entries.find(std::string(absolute_url));
    if (found != impl_->entries.end()) return found->second.bitmap;
    if (impl_->entries.size() >= max_render_images || impl_->total_bytes >= max_render_image_bytes) {
        ++impl_->dropped;
        return nullptr;
    }

    auto bytes = loader.load(absolute_url);
    if (bytes.empty() || bytes.size() > max_render_image_bytes) {
        impl_->insert(std::string(absolute_url), nullptr, 0);
        return nullptr;
    }
    auto bitmap = ImageBitmap::decode(
        std::span<const std::uint8_t>(bytes.data(), bytes.size()));
    if (bitmap && bitmap->byte_size() > max_render_image_bytes - impl_->total_bytes) {
        ++impl_->dropped;
        bitmap.reset();
    }
    impl_->insert(std::string(absolute_url), bitmap, bitmap ? bitmap->byte_size() : 0);
    return bitmap;
}

void ImageCache::clear() {
    impl_->entries.clear();
    impl_->total_bytes = 0;
    impl_->dropped = 0;
}

std::size_t ImageCache::size() const noexcept { return impl_->entries.size(); }

std::size_t ImageCache::byte_size() const noexcept { return impl_->total_bytes; }

std::size_t ImageCache::dropped() const noexcept { return impl_->dropped; }

}  // namespace prowsetk
