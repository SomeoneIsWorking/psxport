// image_writer.h — one host-file boundary for RGB24 captures.
#pragma once

// Writes RGB24 pixels as PPM only for an explicit .ppm suffix; every other suffix uses PNG.
// Returns false when no complete image was written and logs the failing operation at its owner.
bool image_write_rgb24(const char *path, const unsigned char *rgb, int width, int height);
