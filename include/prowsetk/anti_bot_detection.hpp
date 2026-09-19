#ifndef PROWSETK_ANTI_BOT_DETECTION_HPP
#define PROWSETK_ANTI_BOT_DETECTION_HPP

#include <string>
#include <string_view>
#include <vector>

#include "prowsetk/document.hpp"
#include "prowsetk/network_client.hpp"

namespace prowsetk {

struct AntiBotSignal {
    std::string source;
    std::string name;
    std::string detail;
    double confidence = 0.0;
};

struct AntiBotDetection {
    bool activated = false;
    std::string category;
    std::string url;
    int status = 0;
    double confidence = 0.0;
    std::vector<AntiBotSignal> signals;
};

class AntiBotDetector {
public:
    AntiBotDetection inspect_response(const HttpRequest& request,
                                      const HttpResponse& response) const;
    AntiBotDetection inspect_document(const Document& document) const;
    AntiBotDetection merge(AntiBotDetection first,
                           const AntiBotDetection& second) const;
};

const char* to_string(const AntiBotDetection& detection) noexcept;

}  // namespace prowsetk

#endif  // PROWSETK_ANTI_BOT_DETECTION_HPP
