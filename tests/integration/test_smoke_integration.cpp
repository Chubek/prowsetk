#include <prowsetk/version.hpp>

#include <cstdlib>
#include <iostream>

int main() {
    std::cout << "prowsetk.integration.smoke: core " << prowsetk::version() << '\n';
    return EXIT_SUCCESS;
}
