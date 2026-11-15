// text_capture.h — run a FILE*-writing diagnostic view and keep what it wrote.
#pragma once

#include <cstdio>
#include <functional>
#include <optional>
#include <string>

namespace psx::debug {

// Empty when the host cannot give a stream.
std::optional<std::string> captureText(const std::function<void(std::FILE *)> &writer);

} // namespace psx::debug
