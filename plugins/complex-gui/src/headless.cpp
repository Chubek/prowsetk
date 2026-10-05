#include <prowsetk/plugins/complex_gui.hpp>
#include <prowsetk/error.hpp>
namespace prowsetk::complex_gui {
struct Viewer::Impl {};
bool available() noexcept { return false; }
Viewer::Viewer(Session&) { throw Error(ErrorCode::Unsupported, "complex-gui built without FLTK"); }
Viewer::~Viewer() = default;
Controller& Viewer::controller() { throw Error(ErrorCode::Unsupported, "complex-gui unavailable"); }
void Viewer::show() {}
void Viewer::hide() {}
int Viewer::exec() { return 1; }
void Viewer::refresh() {}
}
