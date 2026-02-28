// ============================================================================
//  imgui_vr_example.cpp
//  Example: Rendering Dear ImGui in a Vulkan VR game via IL2CPP hooking.
//
//  Works with: Beat Saber, Gorilla Tag, VRChat (PC), Bonelab, etc.
//  Hook framework: MinHook (Windows) — swap define for Dobby on Android/Quest.
//
//  Build as a DLL and inject / load as a BepInEx/MelonLoader native plugin,
//  or use any other DLL injection method.
//
//  Steps:
//    1. Add imgui core files + imgui_impl_vulkan + imgui_impl_openxr to project.
//    2. Include openxr headers (from Khronos SDK or game's own headers).
//    3. Include vulkan headers.
//    4. Link: openxr_loader.lib (or resolve dynamically), vulkan-1.lib, MinHook.
//    5. Compile as x64 Release.
//    6. Drop DLL into BepInEx/plugins or inject via loader.
// ============================================================================

#define IMGUI_IMPL_OPENXR_VULKAN
#define IMGUI_IMPL_OPENXR_USE_MINHOOK   // swap to IMGUI_IMPL_OPENXR_USE_DOBBY for Quest

#include "imgui.h"
#include "backends/imgui_impl_openxr.h"
#include "backends/imgui_impl_vulkan.h"   // linked renderer

#include <windows.h>
#include <openxr/openxr.h>
#include <vulkan/vulkan.h>

// ============================================================================
//  State grabbed from the game's Vulkan / XR initialisation.
//  Populate these by hooking vkCreateDevice / xrCreateSession (see below).
// ============================================================================
static struct
{
    // Vulkan
    VkInstance          vkInstance      = VK_NULL_HANDLE;
    VkPhysicalDevice    vkPhysDevice    = VK_NULL_HANDLE;
    VkDevice            vkDevice        = VK_NULL_HANDLE;
    uint32_t            vkQueueFamily   = 0;
    VkQueue             vkQueue         = VK_NULL_HANDLE;

    // OpenXR
    XrInstance          xrInstance      = XR_NULL_HANDLE;
    XrSession           xrSession       = XR_NULL_HANDLE;
    XrSystemId          xrSystemId      = XR_NULL_SYSTEM_ID;
} g_State;

static bool g_ImGuiInited = false;

// ============================================================================
//  Vulkan state capture hooks
//  (You need these so you have valid Vk handles before ImGui init)
// ============================================================================
typedef VkResult (VKAPI_PTR *PFN_vkCreateDevice_Real)(
    VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
static PFN_vkCreateDevice_Real Real_vkCreateDevice = nullptr;

static VkResult VKAPI_CALL Hook_vkCreateDevice(
    VkPhysicalDevice physDevice,
    const VkDeviceCreateInfo* pCI,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice)
{
    VkResult r = Real_vkCreateDevice(physDevice, pCI, pAllocator, pDevice);
    if (r == VK_SUCCESS)
    {
        g_State.vkPhysDevice  = physDevice;
        g_State.vkDevice      = *pDevice;
        g_State.vkQueueFamily = pCI->pQueueCreateInfos[0].queueFamilyIndex;
        vkGetDeviceQueue(*pDevice, g_State.vkQueueFamily, 0, &g_State.vkQueue);
    }
    return r;
}

typedef VkResult (VKAPI_PTR *PFN_vkCreateInstance_Real)(
    const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
static PFN_vkCreateInstance_Real Real_vkCreateInstance = nullptr;

static VkResult VKAPI_CALL Hook_vkCreateInstance(
    const VkInstanceCreateInfo* pCI,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance)
{
    VkResult r = Real_vkCreateInstance(pCI, pAllocator, pInstance);
    if (r == VK_SUCCESS)
        g_State.vkInstance = *pInstance;
    return r;
}

// ============================================================================
//  OpenXR state capture hooks
// ============================================================================
typedef XrResult (XRAPI_PTR *PFN_xrCreateSession_Real)(
    XrInstance, const XrSessionCreateInfo*, XrSession*);
static PFN_xrCreateSession_Real Real_xrCreateSession = nullptr;

static XrResult XRAPI_CALL Hook_xrCreateSession(
    XrInstance instance,
    const XrSessionCreateInfo* pCI,
    XrSession* session)
{
    XrResult r = Real_xrCreateSession(instance, pCI, session);
    if (XR_SUCCEEDED(r))
    {
        g_State.xrInstance = instance;
        g_State.xrSession  = *session;
        g_State.xrSystemId = pCI->systemId;
    }
    return r;
}

// ============================================================================
//  ImGui initialisation — called once we have all handles.
// ============================================================================
static bool InitImGui()
{
    if (g_ImGuiInited) return true;
    if (g_State.vkDevice == VK_NULL_HANDLE || g_State.xrSession == XR_NULL_HANDLE)
        return false;  // not ready yet

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;   // controller navigation

    // Style
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 8.0f;
    ImGui::GetStyle().Alpha          = 0.92f;

    // Fill init info
    ImGui_ImplOpenXR_InitInfo initInfo;
    initInfo.Instance        = g_State.xrInstance;
    initInfo.Session         = g_State.xrSession;
    initInfo.SystemId        = g_State.xrSystemId;
    initInfo.VkInstance      = g_State.vkInstance;
    initInfo.VkPhysicalDevice = g_State.vkPhysDevice;
    initInfo.VkDevice        = g_State.vkDevice;
    initInfo.VkQueueFamily   = g_State.vkQueueFamily;
    initInfo.VkQueue         = g_State.vkQueue;
    initInfo.QuadWidth       = 1.2f;    // 1.2 metres wide
    initInfo.QuadHeight      = 0.9f;    // 0.9 metres tall
    initInfo.QuadDistance    = 1.4f;    // 1.4 m in front of face
    initInfo.Billboard       = true;    // always face the player

    if (!ImGui_ImplOpenXR_Init(&initInfo))
    {
        fprintf(stderr, "[Example] ImGui_ImplOpenXR_Init failed\n");
        return false;
    }

    // Install xrEndFrame hook via MinHook
    if (!ImGui_ImplOpenXR_InstallHook_MinHook())
    {
        fprintf(stderr, "[Example] Hook install failed\n");
        return false;
    }

    g_ImGuiInited = true;
    fprintf(stdout, "[Example] ImGui VR initialised!\n");
    return true;
}

// ============================================================================
//  Your UI — called every frame inside the hooked xrEndFrame.
//  Edit this to build whatever overlay you want.
// ============================================================================
static void DrawMyMenu()
{
    ImGui::SetNextWindowSize(ImVec2(420, 300), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(50, 50),   ImGuiCond_FirstUseEver);

    ImGui::Begin("VR Overlay  ###MainMenu");

    ImGui::Text("Hello from Dear ImGui in VR!");
    ImGui::Separator();

    static float speed = 1.0f;
    static bool  godMode = false;
    ImGui::SliderFloat("Speed multiplier", &speed, 0.1f, 10.0f);
    ImGui::Checkbox("God mode", &godMode);

    if (ImGui::Button("Do something cool"))
    {
        // Call into IL2CPP via il2cpp_class_get_method / invoke here
    }

    ImGui::Separator();
    ImGui::TextDisabled("Right stick = move cursor | Trigger/A = click");

    ImGui::End();
}

// ============================================================================
//  Hooked xrEndFrame — this is where the frame gets injected each tick.
// ============================================================================
// NOTE: ImGui_ImplOpenXR_xrEndFrame_Detour already calls ImGui_ImplOpenXR_EndFrame
//       which calls ImGui::Render() internally.  We only need to ensure NewFrame
//       + our UI runs BEFORE that.  We accomplish this by wrapping the detour:

static PFN_xrEndFrame OriginalDetour = nullptr;

static XrResult XRAPI_CALL MyEndFrameWrapper(
    XrSession session, const XrFrameEndInfo* frameEndInfo)
{
    // Lazy-init (needs valid session which we now have)
    InitImGui();

    if (g_ImGuiInited)
    {
        // Pump controller input
        XrSpace space;
        XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        rsci.referenceSpaceType     = XR_REFERENCE_SPACE_TYPE_LOCAL;
        rsci.poseInReferenceSpace   = { {0,0,0,1},{0,0,0} };
        xrCreateReferenceSpace(session, &rsci, &space);

        ImGui_ImplOpenXR_ProcessControllerInput(space, frameEndInfo->displayTime);
        xrDestroySpace(space);

        // Build frame
        ImGui_ImplOpenXR_NewFrame();
        ImGui::NewFrame();

        DrawMyMenu();

        // EndFrame injects the layer and calls real xrEndFrame
        return ImGui_ImplOpenXR_EndFrame(session, frameEndInfo);
    }

    // Not ready — pass through unchanged
    return ((PFN_xrEndFrame)OriginalDetour)(session, frameEndInfo);
}

// ============================================================================
//  Hook installation for Vulkan functions + setup
// ============================================================================
static bool InstallVulkanHooks()
{
#ifdef IMGUI_IMPL_OPENXR_USE_MINHOOK
    MH_Initialize();

    HMODULE hVk = GetModuleHandleA("vulkan-1.dll");
    if (!hVk) hVk = LoadLibraryA("vulkan-1.dll");
    if (!hVk) return false;

    void* pCI = GetProcAddress(hVk, "vkCreateInstance");
    void* pCD = GetProcAddress(hVk, "vkCreateDevice");

    MH_CreateHook(pCI, (void*)Hook_vkCreateInstance, (void**)&Real_vkCreateInstance);
    MH_CreateHook(pCD, (void*)Hook_vkCreateDevice,   (void**)&Real_vkCreateDevice);
    MH_EnableHook(pCI);
    MH_EnableHook(pCD);

    // xrCreateSession hook
    HMODULE hXr = LoadLibraryA("openxr_loader.dll");
    if (hXr)
    {
        void* pCS = GetProcAddress(hXr, "xrCreateSession");
        MH_CreateHook(pCS, (void*)Hook_xrCreateSession, (void**)&Real_xrCreateSession);
        MH_EnableHook(pCS);
    }

    return true;
#else
    return false;
#endif
}

// ============================================================================
//  DLL Entry Point
// ============================================================================
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hInst);

        // Spawn setup on a background thread so we don't block loader lock.
        CreateThread(nullptr, 0, [](void*) -> DWORD {
            Sleep(2000); // wait for game to init Vulkan / XR
            InstallVulkanHooks();
            fprintf(stdout, "[Example] Hooks installed, waiting for VR session...\n");
            return 0;
        }, nullptr, 0, nullptr);
    }
    return TRUE;
}

// ============================================================================
//  Quest / Android variant notes (Dobby):
//
//  Replace DllMain with a __attribute__((constructor)) function.
//  Use IMGUI_IMPL_OPENXR_USE_DOBBY instead of MINHOOK.
//  Resolve vulkan symbols via dlopen("libvulkan.so", RTLD_NOW).
//  The rest of the logic is identical.
// ============================================================================
