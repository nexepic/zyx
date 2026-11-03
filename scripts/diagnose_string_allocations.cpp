// Standalone diagnostic: measure fresh std::string requests independently of
// the production admission estimate. Run as a separate process.
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>

namespace {
bool observing = false;
std::size_t requested = 0;
std::size_t allocations = 0;
}

void *operator new(std::size_t bytes) {
    if (observing) {
        requested += bytes;
        ++allocations;
    }
    if (void *value = std::malloc(bytes)) return value;
    throw std::bad_alloc();
}
void operator delete(void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }

int main() {
    static char input[8192]{};
    std::printf("sizeof_string=%zu", sizeof(std::string));
#ifdef _LIBCPP_VERSION
    std::printf(" libcxx=%d", _LIBCPP_VERSION);
#endif
    std::printf("\n");
    for (const std::size_t length : {std::size_t{22}, std::size_t{23}, std::size_t{512}, std::size_t{8192}}) {
        requested = allocations = 0;
        observing = true;
        std::string value{std::string_view(input, length)};
        observing = false;
        std::printf("length=%zu allocations=%zu requested=%zu capacity=%zu\n",
                    length, allocations, requested, value.capacity());
        if (value.size() != length) return 1;
    }
}
