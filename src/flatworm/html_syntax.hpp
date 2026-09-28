#ifndef PROWSETK_FLATWORM_HTML_SYNTAX_HPP
#define PROWSETK_FLATWORM_HTML_SYNTAX_HPP
#include <string>
#include <string_view>
namespace prowsetk::flatworm {
std::string to_lower(std::string_view value);
bool is_space(char c);
bool is_void_element(std::string_view name);
bool is_raw_text_element(std::string_view name);
bool is_rcdata_element(std::string_view name);
std::string decode_entities(std::string_view input);
}
#endif
