#pragma once

#include <cstdint>

class Core;

// Publish a stock libcd command's synchronous response, retaining it for the
// matching CdSync completion. False means the requested TOC cannot be read.
bool stock_cd_begin_command(Core &core, uint8_t command, uint32_t parameter, uint32_t result);
void stock_cd_publish_sync(Core &core, uint32_t result);
void stock_cd_forget_response(Core &core);
void stock_cd_zero_result(Core &core, uint32_t result);
