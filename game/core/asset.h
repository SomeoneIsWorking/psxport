// engine/asset.h — PC-native asset loading subsystem (texture-group load + LZ decompress + VRAM upload).
// Extracted from game_tomba2.cpp into its own module (PC-game code structure). Registered in
// game_tomba2.cpp's init block by these names.
#ifndef ENGINE_ASSET_H
#define ENGINE_ASSET_H
struct Core;
void ov_lz_decompress(Core* c);   // FUN_80044D8C — LZ image decompressor
void ov_unpack_group(Core* c);    // FUN_80044E84 — texture-group unpacker (decompress+upload each image)
void ov_load_texgroup(Core* c);   // FUN_80044F58 — texture-group LOADER orchestration (header+archive+unpack)
void ov_upload_image(Core* c);    // FUN_80081218 — PC-native CPU->VRAM upload
void preload_texgroup(Core* c, uint32_t mode, uint32_t set);   // FUN_80044F58 (stage-0/stage-1 texgroup preload, synchronous)
void preload_stage1(Core* c);     // FUN_8004514C — SWDATA+DAT load, shared texgroup sub-load, relocation, cel/sprite VRAM build
#endif
