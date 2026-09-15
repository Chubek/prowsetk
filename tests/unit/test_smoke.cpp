#include <prowsetk/version.hpp>

#include <cstdlib>
#include <cstring>
#include <iostream>

int main() {
    const char* v = prowsetk::version();
    if (v == nullptr || std::strlen(v) == 0) {
        std::cerr << "prowsetk.unit.smoke: empty version string\n";
        return EXIT_FAILURE;
    }
    std::cout << "prowsetk " << v << '\n';
    return EXIT_SUCCESS;
}
