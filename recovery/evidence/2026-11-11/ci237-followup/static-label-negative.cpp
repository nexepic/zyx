#include "graph/debug/StaticPerfTimer.hpp"
void invalid(const std::string &runtime) {
 graph::debug::StaticPerfTimer timer(graph::debug::StaticPerfLabel{std::string_view(runtime)});
}
