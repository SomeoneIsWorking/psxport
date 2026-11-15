// text_capture.cpp — see text_capture.h.
#include "text_capture.h"

#include <cstdlib>

namespace psx::debug {

#if defined(_WIN32)
// The Windows C runtime has no open_memstream.
std::optional<std::string> captureText(const std::function<void(std::FILE *)> &writer) {
  std::FILE *stream = std::tmpfile();
  if (stream == nullptr) {
    return std::nullopt;
  }
  writer(stream);
  std::string text;
  if (std::fflush(stream) == 0 && std::fseek(stream, 0, SEEK_SET) == 0) {
    char block[4096];
    std::size_t count = 0;
    while ((count = std::fread(block, 1, sizeof block, stream)) > 0) {
      text.append(block, count);
    }
  }
  std::fclose(stream);
  return text;
}
#else
std::optional<std::string> captureText(const std::function<void(std::FILE *)> &writer) {
  char *buffer = nullptr;
  std::size_t length = 0;
  std::FILE *stream = open_memstream(&buffer, &length);
  if (stream == nullptr) {
    return std::nullopt;
  }
  writer(stream);
  std::fclose(stream);
  std::string text(buffer != nullptr ? buffer : "", length);
  std::free(buffer);
  return text;
}
#endif

} // namespace psx::debug
