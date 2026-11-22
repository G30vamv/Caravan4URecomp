// The main of the test binaries that do not define MH4N_TEST_OWN_MAIN.
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#include <stdlib.h>
#else
#define _putenv_s(n, v) setenv(n, v, 1)
#endif
#include <exception>
#include <iostream>

#include <doctest/doctest.h>

int main(int argc, char** argv) {
    // MODIFIED for the Windows port: the program installs the independent service implementations (own/) by default; the older
    // services' tests exercise the older ones. The independent ones have their own tests (wp*_test targets).
    if (std::getenv("MH4N_NEW_SERVICES") == nullptr) {
        _putenv_s("MH4N_NEW_SERVICES", "none");
    }
    // An assertion that fails where it cannot unwind (a worker thread, a destructor) ends the test in
    // std::terminate. Flush what doctest printed first, so that the failing assertion reaches a CI log
    // (a piped stdout is buffered and is otherwise lost on abort).
    std::set_terminate([] {
        std::cout.flush();
        std::fflush(nullptr);
        std::abort();
    });
    return doctest::Context(argc, argv).run();
}
