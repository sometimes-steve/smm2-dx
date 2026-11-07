// Dear ImGui from C: cimgui (third_party/cimgui: C bindings generated for the vendored ImGui) plus the declarations of
// the SDL3 and Vulkan backends (third_party/imgui/backends, built with IMGUI_IMPL_API=extern "C" so their names are
// plain C symbols). Functions are igXxx(), types and enums keep ImGui's names, ImVec2 / ImVec4 are passed by value.
#pragma once
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include "cimgui/cimgui.h"

#define IM_COL32(R, G, B, A) (((ImU32)(A) << 24) | ((ImU32)(B) << 16) | ((ImU32)(G) << 8) | ((ImU32)(R)))
#define IM_COL32_A_MASK 0xFF000000
#define IM_VEC2(x, y) ((ImVec2){(float)(x), (float)(y)})
#define IM_VEC4(x, y, z, w) ((ImVec4){(float)(x), (float)(y), (float)(z), (float)(w)})

// ---- imgui_impl_sdl3.cpp
bool ImGui_ImplSDL3_InitForVulkan(SDL_Window *window);
bool ImGui_ImplSDL3_ProcessEvent(const SDL_Event *event);
void ImGui_ImplSDL3_NewFrame(void);
void ImGui_ImplSDL3_Shutdown(void);

// ---- imgui_impl_vulkan.cpp (the struct mirrors ImGui 1.91.9's ImGui_ImplVulkan_InitInfo with dynamic rendering)
typedef struct ImGui_ImplVulkan_InitInfo_C {
    uint32_t ApiVersion;
    VkInstance Instance;
    VkPhysicalDevice PhysicalDevice;
    VkDevice Device;
    uint32_t QueueFamily;
    VkQueue Queue;
    VkDescriptorPool DescriptorPool;
    VkRenderPass RenderPass;
    uint32_t MinImageCount;
    uint32_t ImageCount;
    VkSampleCountFlagBits MSAASamples;
    VkPipelineCache PipelineCache;
    uint32_t Subpass;
    uint32_t DescriptorPoolSize;
    bool UseDynamicRendering;
    VkPipelineRenderingCreateInfoKHR PipelineRenderingCreateInfo;
    const VkAllocationCallbacks *Allocator;
    void (*CheckVkResultFn)(VkResult err);
    VkDeviceSize MinAllocationSize;
} ImGui_ImplVulkan_InitInfo_C;

bool ImGui_ImplVulkan_Init(ImGui_ImplVulkan_InitInfo_C *info);
void ImGui_ImplVulkan_Shutdown(void);
void ImGui_ImplVulkan_NewFrame(void);
void ImGui_ImplVulkan_RenderDrawData(ImDrawData *draw_data, VkCommandBuffer command_buffer, VkPipeline pipeline);
VkDescriptorSet ImGui_ImplVulkan_AddTexture(VkSampler sampler, VkImageView image_view, VkImageLayout image_layout);
