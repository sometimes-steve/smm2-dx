// Interface between the NVN executor and the Vulkan backend (runtime/nvn/vulkan/).
#pragma once
#include <SDL3/SDL_events.h>

bool renderer_is_gpu_owned(NvnTexture *t);
void renderer_flush(void);  // finish all recorded GPU work
bool renderer_origin_lower_left(void);
const char *renderer_stats(void);
bool renderer_format_is_depth(uint32_t f);
bool renderer_format_has_stencil(uint32_t f);
bool renderer_format_is_soft16(uint32_t f);  // packed 16-bit colour texture backed by RGBA8 (decoded on the CPU)
void renderer_soft16_decode(uint32_t f, const uint8_t *src, uint64_t n, uint8_t *dst);
void renderer_mark_gpu_owned(NvnTexture *t);
void input_handle_event(const SDL_Event *e);
bool input_user_blocked(const SDL_Event *e);
