#pragma once
// Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
#include <stdexcept>
#include <string>

namespace graph::query::execution::detail {
// A procedure's admission can fail at different owners on different STLs.
// Keep a typed quota failure distinct from semantic/backend runtime errors;
// the message still identifies the actual failing admission stage.
class ProcedureByteBoundExceeded final : public std::runtime_error {
public:
    explicit ProcedureByteBoundExceeded(const std::string &message) : std::runtime_error(message) {}
};
}
