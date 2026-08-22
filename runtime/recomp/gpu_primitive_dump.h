// gpu_primitive_dump.h — CSV diagnostics for primitives observed by the native GPU command path.
#pragma once

#include <cstdint>

struct Core;

void gpu_primitive_dump_polygon(Core *core,
                                int frame,
                                unsigned id,
                                uint8_t op,
                                int vertexCount,
                                int is3d,
                                int background,
                                const int *screenX,
                                const int *screenY,
                                const int *textureU,
                                const int *textureV,
                                uint8_t red,
                                uint8_t green,
                                uint8_t blue,
                                int textured,
                                int semitransparent,
                                int rawTexture);

void gpu_primitive_dump_sprite(Core *core,
                               int frame,
                               unsigned id,
                               uint8_t op,
                               int x,
                               int y,
                               int width,
                               int height,
                               int background,
                               uint8_t red,
                               uint8_t green,
                               uint8_t blue,
                               int textured,
                               int semitransparent,
                               int textureU,
                               int textureV,
                               int rawTexture);

void gpu_primitive_dump_finish_frame(Core *core, int frame);
