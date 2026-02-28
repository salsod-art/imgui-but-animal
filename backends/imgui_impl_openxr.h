// dear imgui: Platform Backend for OpenXR (VR / IL2CPP game hooking)
// Hooks xrEndFrame to inject ImGui as an OpenXR composition layer.
// Works with any hooking framework: MinHook, Dobby, minhook-rs, etc.
//
// SETUP (IL2CPP mod / game hook):
//   1. Grab openxr_loader.dll / libopenxr_loader.so from the game's install dir.
//   2. Hook xrEndFrame (see ImGui_ImplOpenXR_HookEndFrame).
//   3. Inside your hook call ImGui_ImplOpenXR_NewFrame() then build your UI,
//      then call ImGui_ImplOpenXR_EndFrame(session, frameEndInfo) which injects
//      a quad-layer carrying the ImGui render into the submitted frame.
//
// RENDERER BACKEND:
//   Pair with imgui_impl_vulkan (most Unity/IL2CPP VR titles use Vulkan).
//   For OpenGL-based titles use imgui_impl_opengl3 instead.
//
// CONTROLLER INPUT:
//   Call ImGui_ImplOpenXR_ProcessControllerInput() each frame. It maps
//   right-thumbstick to ImGui mouse and A/trigger to left-click.
//
// COMPILATION:
//   Define IMGUI_IMPL_OPENXR_VULKAN to compile the Vulkan swapchain path.
//   Define IMGUI_IMPL_OPENXR_OPENGL to compile the OpenGL path.
//
// HOOKS (choose one framework, define before including):
//   #define IMGUI_IMPL_OPENXR_USE_MINHOOK   - uses MinHook (Windows)
//   #define IMGUI_IMPL_OPENXR_USE_DOBBY     - uses Dobby  (Android/Linux)
//   #define IMGUI_IMPL_OPENXR_MANUAL_HOOK   - you install the hook yourself,
//                                             just call ImGui_ImplOpenXR_InstallHooks()
//                                             with function pointers.

#pragma once
#ifndef IMGUI_DISABLE

#include "imgui.h"
#include <openxr/openxr.h>

#ifdef IMGUI_IMPL_OPENXR_VULKAN
#include <vulkan/vulkan.h>
#endif

//-----------------------------------------------------------------------------
// [SECTION] Init / Shutdown
//-----------------------------------------------------------------------------

// InitInfo: fill before calling ImGui_ImplOpenXR_Init().
struct ImGui_ImplOpenXR_InitInfo
{
    XrInstance          Instance;
    XrSession           Session;
    XrSystemId          SystemId;

    // Overlay quad size in meters, centred in front of the user.
    float               QuadWidth;          // default 1.2 m
    float               QuadHeight;         // default 0.9 m
    float               QuadDistance;       // default 1.5 m in front of head

    // How many swapchain images to allocate (2 or 3).
    uint32_t            SwapchainLength;    // default 3

    // If true the quad will always face the HMD (billboard mode).
    bool                Billboard;          // default true

    // Renderer-specific fields — fill only the one you use.
#ifdef IMGUI_IMPL_OPENXR_VULKAN
    VkInstance          VkInstance;
    VkPhysicalDevice    VkPhysicalDevice;
    VkDevice            VkDevice;
    uint32_t            VkQueueFamily;
    VkQueue             VkQueue;
    VkFormat            SwapchainFormat;    // leave 0 for VK_FORMAT_R8G8B8A8_UNORM
#endif

#ifdef IMGUI_IMPL_OPENXR_OPENGL
    // Provide current GL context (Windows: HGLRC, Linux: GLXContext).
    void*               GLContext;
#endif

    ImGui_ImplOpenXR_InitInfo()
    {
        memset(this, 0, sizeof(*this));
        QuadWidth      = 1.2f;
        QuadHeight     = 0.9f;
        QuadDistance   = 1.5f;
        SwapchainLength = 3;
        Billboard      = true;
    }
};

// Core lifecycle
IMGUI_IMPL_API bool  ImGui_ImplOpenXR_Init(ImGui_ImplOpenXR_InitInfo* info);
IMGUI_IMPL_API void  ImGui_ImplOpenXR_Shutdown();
IMGUI_IMPL_API void  ImGui_ImplOpenXR_NewFrame();

// Call this INSTEAD of the real xrEndFrame when you have a manual hook.
// It injects the ImGui quad layer then forwards to the real xrEndFrame.
IMGUI_IMPL_API XrResult ImGui_ImplOpenXR_EndFrame(
    XrSession                       session,
    const XrFrameEndInfo*           frameEndInfo);

//-----------------------------------------------------------------------------
// [SECTION] Hook installation helpers
//-----------------------------------------------------------------------------

// Signature matching OpenXR loader export.
typedef XrResult (XRAPI_PTR *PFN_xrEndFrame)(XrSession, const XrFrameEndInfo*);

struct ImGui_ImplOpenXR_HookFunctions
{
    // [in]  Real xrEndFrame fetched from the loader. Fill this.
    PFN_xrEndFrame  Real_xrEndFrame;
    // [out] Your detour (= ImGui_ImplOpenXR_xrEndFrame_Detour). Set by Init.
    PFN_xrEndFrame  Detour_xrEndFrame;
};

// If IMGUI_IMPL_OPENXR_MANUAL_HOOK: call after ImGui_ImplOpenXR_Init().
// Fills out->Detour_xrEndFrame — install that as your hook target.
IMGUI_IMPL_API void ImGui_ImplOpenXR_InstallHooks(ImGui_ImplOpenXR_HookFunctions* out);

// MinHook / Dobby helpers — call once at mod startup, before Init().
#ifdef IMGUI_IMPL_OPENXR_USE_MINHOOK
IMGUI_IMPL_API bool ImGui_ImplOpenXR_InstallHook_MinHook();   // calls MH_Initialize internally
#endif
#ifdef IMGUI_IMPL_OPENXR_USE_DOBBY
IMGUI_IMPL_API bool ImGui_ImplOpenXR_InstallHook_Dobby();
#endif

//-----------------------------------------------------------------------------
// [SECTION] Controller / input
//-----------------------------------------------------------------------------

// Call once per frame (before NewFrame) to pump XrAction input into ImGui.
// Thumbstick X/Y → mouse delta, trigger/A → left mouse button.
IMGUI_IMPL_API void ImGui_ImplOpenXR_ProcessControllerInput(XrSpace referenceSpace, XrTime predictedTime);

// If you want to map a different hand, set this (default: right hand).
IMGUI_IMPL_API void ImGui_ImplOpenXR_SetActiveHand(bool rightHand);

//-----------------------------------------------------------------------------
// [SECTION] Utility / debug
//-----------------------------------------------------------------------------

// Returns the XrSwapchain allocated for the ImGui layer (for debugging).
IMGUI_IMPL_API XrSwapchain ImGui_ImplOpenXR_GetSwapchain();

// Detour function — exposed so you can reference it in a trampoline hook.
IMGUI_IMPL_API XrResult XRAPI_CALL ImGui_ImplOpenXR_xrEndFrame_Detour(
    XrSession session, const XrFrameEndInfo* frameEndInfo);

#endif // IMGUI_DISABLE
