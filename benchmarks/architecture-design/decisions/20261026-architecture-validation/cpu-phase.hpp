#pragma once
#include <cstdint>
namespace cpu_phase {
inline thread_local uint8_t current = 0;
struct Scope {
 bool root;
 explicit Scope(uint8_t id) noexcept : root(current == 0) { if(root) current=id; }
 ~Scope() { if(root) current=0; }
};
}
