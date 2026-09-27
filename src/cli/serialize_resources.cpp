#include "serialize_resources.hpp"

#include <algorithm>
#include <string>
#include <string_view>

#include <prowsetk/browser.hpp>
#include <prowsetk/url.hpp>

namespace prowsetk::cli {

static std::string encode_base64(std::string_view bytes) {
    constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((bytes.size() + 2U) / 3U * 4U);
    for (std::size_t i = 0; i < bytes.size(); i += 3U) {
        const auto a = static_cast<unsigned char>(bytes[i]);
        const auto b = i + 1U < bytes.size() ?
            static_cast<unsigned char>(bytes[i + 1U]) : 0U;
        const auto c = i + 2U < bytes.size() ?
            static_cast<unsigned char>(bytes[i + 2U]) : 0U;
        encoded += alphabet[a >> 2U];
        encoded += alphabet[((a & 3U) << 4U) | (b >> 4U)];
        encoded += i + 1U < bytes.size() ?
            alphabet[((b & 15U) << 2U) | (c >> 6U)] : '=';
        encoded += i + 2U < bytes.size() ? alphabet[c & 63U] : '=';
    }
    return encoded;
}

// Keep resource requests inside the owning session. A bounded, same-origin
// snapshot makes an IR pipeline self-contained without teaching the C PDF
// renderer to perform network requests or handle session credentials.
void embed_serialized_resources(prowsetk::Session& session,
                                prowsetk::Document& document) {
    const std::string base = document.base_url();
    if (base.empty()) return;
    const prowsetk::Url origin = prowsetk::parse_url(base);
    std::size_t total_bytes = 0U;
    std::size_t requests = 0U;
    const auto fetch = [&](std::string_view reference,
                           std::size_t limit) -> prowsetk::HttpResponse {
        if (reference.empty() || requests >= 24U || total_bytes >= 2U * 1024U * 1024U)
            return {};
        try {
            const auto target = prowsetk::resolve_url(base, reference);
            const auto parsed = prowsetk::parse_url(target);
            if ((parsed.scheme != "http" && parsed.scheme != "https") ||
                parsed.origin() != origin.origin()) return {};
            prowsetk::HttpRequest request;
            request.url = target;
            request.max_response_bytes = (std::min)(limit,
                2U * 1024U * 1024U - total_bytes);
            ++requests;
            auto response = session.request(std::move(request));
            if (!response.ok() || response.body.size() > limit ||
                (!response.final_url.empty() &&
                 prowsetk::parse_url(response.final_url).origin() != origin.origin()))
                return {};
            total_bytes += response.body.size();
            return response;
        } catch (const std::exception&) {
            return {};
        }
    };
    for (const auto& link : document.get_elements_by_tag_name("link")) {
        if (link->attribute("rel") != "stylesheet") continue;
        const auto response = fetch(link->attribute("href"), 256U * 1024U);
        if (response.body.empty() ||
            response.header("content-type").find("text/css") == std::string::npos)
            continue;
        auto style = document.create_element("style");
        style->set_text(response.body);
        auto parent = link->parent();
        if (parent != nullptr) parent->append_child(style);
    }
    for (const auto& image : document.get_elements_by_tag_name("img")) {
        const std::string source = image->attribute("src");
        if (source.starts_with("data:")) continue;
        const auto response = fetch(source, 512U * 1024U);
        const std::string content_type = response.header("content-type");
        const char* type = content_type.find("image/png") != std::string::npos ?
            "image/png" : (content_type.find("image/jpeg") != std::string::npos ?
                           "image/jpeg" : nullptr);
        if (type != nullptr && !response.body.empty())
            image->set_attribute("src", std::string("data:") + type + ";base64," +
                                        encode_base64(response.body));
    }
}

}  // namespace prowsetk::cli
