#include <prowsetk/plugins/basic_gui.hpp>
#include <prowsetk/error.hpp>

namespace prowsetk::basic_gui {
struct Viewer::Impl {};
Viewer::Viewer(Session&) { throw Error(ErrorCode::Unsupported, "basic-gui was built without FLTK"); }
Viewer::~Viewer() = default;
void Viewer::show() { throw Error(ErrorCode::Unsupported, "basic-gui was built without FLTK"); }
void Viewer::navigate(std::string_view) {}
void Viewer::load_html(std::string_view, std::string_view) {}
void Viewer::refresh() {}
void Viewer::close() {}
int Viewer::exec() { throw Error(ErrorCode::Unsupported, "basic-gui was built without FLTK"); }
int run(Session&) { throw Error(ErrorCode::Unsupported, "basic-gui was built without FLTK"); }
}  // namespace prowsetk::basic_gui
