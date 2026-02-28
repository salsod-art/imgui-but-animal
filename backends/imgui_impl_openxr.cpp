// dear imgui: Platform Backend for OpenXR (VR / IL2CPP game hooking)
// See imgui_impl_openxr.h for usage documentation.
//
// Supported render paths:
//   - Vulkan  (define IMGUI_IMPL_OPENXR_VULKAN)
//   - OpenGL3 (define IMGUI_IMPL_OPENXR_OPENGL)
//
// Hook paths:
//   - MinHook (define IMGUI_IMPL_OPENXR_USE_MINHOOK)
//   - Dobby   (define IMGUI_IMPL_OPENXR_USE_DOBBY)
//   - Manual  (define IMGUI_IMPL_OPENXR_MANUAL_HOOK, handle hooking yourself)

#ifndef IMGUI_DISABLE
#include "imgui.h"
#include "imgui_impl_openxr.h"

#ifdef IMGUI_IMPL_OPENXR_VULKAN
#include "imgui_impl_vulkan.h"
#endif
#ifdef IMGUI_IMPL_OPENXR_OPENGL
#include "imgui_impl_opengl3.h"
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>

// ============================================================================
//  Platform includes for hook frameworks
// ============================================================================
#ifdef IMGUI_IMPL_OPENXR_USE_MINHOOK
#   include <MinHook.h>
#   pragma comment(lib, "libMinHook.lib")
#endif
#ifdef IMGUI_IMPL_OPENXR_USE_DOBBY
#   include <dobby.h>
#endif

// ============================================================================
//  Helpers
// ============================================================================
#define OPENXR_CHECK(expr, msg)                                             \
    do {                                                                    \
        XrResult _r = (expr);                                               \
        if (XR_FAILED(_r)) {                                                \
            fprintf(stderr, "[ImGui-OpenXR] %s failed: %d\n", msg, _r);    \
            return false;                                                   \
        }                                                                   \
    } while(0)

// ============================================================================
//  Internal state
// ============================================================================
struct ImGui_ImplOpenXR_SwapchainImage
{
#ifdef IMGUI_IMPL_OPENXR_VULKAN
    VkImage         Image      = VK_NULL_HANDLE;
    VkImageView     ImageView  = VK_NULL_HANDLE;
    VkFramebuffer   Framebuffer = VK_NULL_HANDLE;
#endif
    uint32_t        Width  = 0;
    uint32_t        Height = 0;
};

struct ImGui_ImplOpenXR_Data
{
    // --- init info copy ---
    ImGui_ImplOpenXR_InitInfo   Info;

    // --- OpenXR objects ---
    XrSwapchain                 Swapchain       = XR_NULL_HANDLE;
    uint32_t                    SwapchainW      = 0;
    uint32_t                    SwapchainH      = 0;
    XrSpace                     HeadSpace       = XR_NULL_HANDLE;       // VIEW space
    XrSpace                     LocalSpace      = XR_NULL_HANDLE;       // LOCAL/STAGE space

    // --- Action set for controller input ---
    XrActionSet                 ActionSet       = XR_NULL_HANDLE;
    XrAction                    ThumbstickAction = XR_NULL_HANDLE;
    XrAction                    TriggerAction    = XR_NULL_HANDLE;
    XrAction                    AButtonAction    = XR_NULL_HANDLE;
    XrPath                      HandPath[2]      = {};                  // 0=left,1=right
    bool                        UseRightHand     = true;

    // --- per-image render resources ---
    std::vector<ImGui_ImplOpenXR_SwapchainImage> Images;

#ifdef IMGUI_IMPL_OPENXR_VULKAN
    VkRenderPass                RenderPass      = VK_NULL_HANDLE;
    VkCommandPool               CmdPool         = VK_NULL_HANDLE;
    VkCommandBuffer             CmdBuf          = VK_NULL_HANDLE;
    VkFence                     Fence           = VK_NULL_HANDLE;
    VkDescriptorPool            DescPool        = VK_NULL_HANDLE;
    VkFormat                    FinalFormat     = VK_FORMAT_R8G8B8A8_UNORM;
#endif

    // --- hook ---
    PFN_xrEndFrame              Real_xrEndFrame = nullptr;

    // --- frame state ---
    XrTime                      PredictedTime   = 0;
    bool                        Initialised     = false;
};

static ImGui_ImplOpenXR_Data* ImGui_ImplOpenXR_GetBackendData()
{
    return ImGui::GetCurrentContext()
        ? (ImGui_ImplOpenXR_Data*)ImGui::GetIO().BackendPlatformUserData
        : nullptr;
}

// ============================================================================
//  Vulkan helpers
// ============================================================================
#ifdef IMGUI_IMPL_OPENXR_VULKAN

static bool VK_CreateRenderPass(ImGui_ImplOpenXR_Data* bd)
{
    VkAttachmentDescription att = {};
    att.format         = bd->FinalFormat;
    att.samples        = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

    VkSubpassDescription sub = {};
    sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments    = &ref;

    VkSubpassDependency dep = {};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = 1;
    ci.pAttachments    = &att;
    ci.subpassCount    = 1;
    ci.pSubpasses      = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies   = &dep;

    return vkCreateRenderPass(bd->Info.VkDevice, &ci, nullptr, &bd->RenderPass) == VK_SUCCESS;
}

static bool VK_CreateImageViews(ImGui_ImplOpenXR_Data* bd,
    const std::vector<XrSwapchainImageVulkanKHR>& xrImages)
{
    bd->Images.resize(xrImages.size());
    for (uint32_t i = 0; i < xrImages.size(); i++)
    {
        auto& img = bd->Images[i];
        img.Image  = xrImages[i].image;
        img.Width  = bd->SwapchainW;
        img.Height = bd->SwapchainH;

        // Image view
        VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        ivci.image            = img.Image;
        ivci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format           = bd->FinalFormat;
        ivci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(bd->Info.VkDevice, &ivci, nullptr, &img.ImageView) != VK_SUCCESS)
            return false;

        // Framebuffer
        VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass      = bd->RenderPass;
        fci.attachmentCount = 1;
        fci.pAttachments    = &img.ImageView;
        fci.width           = bd->SwapchainW;
        fci.height          = bd->SwapchainH;
        fci.layers          = 1;
        if (vkCreateFramebuffer(bd->Info.VkDevice, &fci, nullptr, &img.Framebuffer) != VK_SUCCESS)
            return false;
    }
    return true;
}

static bool VK_AllocCommandBuffer(ImGui_ImplOpenXR_Data* bd)
{
    // Command pool
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = bd->Info.VkQueueFamily;
    if (vkCreateCommandPool(bd->Info.VkDevice, &pci, nullptr, &bd->CmdPool) != VK_SUCCESS)
        return false;

    // Command buffer
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool        = bd->CmdPool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(bd->Info.VkDevice, &cbai, &bd->CmdBuf) != VK_SUCCESS)
        return false;

    // Fence
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    return vkCreateFence(bd->Info.VkDevice, &fci, nullptr, &bd->Fence) == VK_SUCCESS;
}

static bool VK_CreateDescriptorPool(ImGui_ImplOpenXR_Data* bd)
{
    VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 },
    };
    VkDescriptorPoolCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    ci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    ci.maxSets       = 16;
    ci.poolSizeCount = 1;
    ci.pPoolSizes    = sizes;
    return vkCreateDescriptorPool(bd->Info.VkDevice, &ci, nullptr, &bd->DescPool) == VK_SUCCESS;
}

// Renders ImGui draw data into swapchain image[imageIndex].
static void VK_RenderImGui(ImGui_ImplOpenXR_Data* bd, uint32_t imageIndex)
{
    VkDevice dev = bd->Info.VkDevice;

    // Wait for previous use of fence
    vkWaitForFences(dev, 1, &bd->Fence, VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &bd->Fence);

    vkResetCommandBuffer(bd->CmdBuf, 0);

    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(bd->CmdBuf, &bi);

    VkClearValue clear = { {{0.0f, 0.0f, 0.0f, 0.0f}} };   // transparent black

    VkRenderPassBeginInfo rpbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpbi.renderPass        = bd->RenderPass;
    rpbi.framebuffer       = bd->Images[imageIndex].Framebuffer;
    rpbi.renderArea.extent = { bd->SwapchainW, bd->SwapchainH };
    rpbi.clearValueCount   = 1;
    rpbi.pClearValues      = &clear;

    vkCmdBeginRenderPass(bd->CmdBuf, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), bd->CmdBuf);

    vkCmdEndRenderPass(bd->CmdBuf);
    vkEndCommandBuffer(bd->CmdBuf);

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &bd->CmdBuf;
    vkQueueSubmit(bd->Info.VkQueue, 1, &si, bd->Fence);
}

static bool VK_InitImGuiRenderer(ImGui_ImplOpenXR_Data* bd)
{
    ImGui_ImplVulkan_InitInfo vi = {};
    vi.Instance      = bd->Info.VkInstance;
    vi.PhysicalDevice = bd->Info.VkPhysicalDevice;
    vi.Device        = bd->Info.VkDevice;
    vi.QueueFamily   = bd->Info.VkQueueFamily;
    vi.Queue         = bd->Info.VkQueue;
    vi.DescriptorPool = bd->DescPool;
    vi.RenderPass    = bd->RenderPass;
    vi.MinImageCount = bd->Info.SwapchainLength;
    vi.ImageCount    = bd->Info.SwapchainLength;
    vi.MSAASamples   = VK_SAMPLE_COUNT_1_BIT;
    return ImGui_ImplVulkan_Init(&vi);
}

#endif // IMGUI_IMPL_OPENXR_VULKAN

// ============================================================================
//  Swapchain creation
// ============================================================================
static bool CreateSwapchain(ImGui_ImplOpenXR_Data* bd)
{
    const auto& info = bd->Info;

    // Pick resolution: 1024x768 is a sensible quad UI resolution.
    bd->SwapchainW = 1024;
    bd->SwapchainH = 768;

    XrSwapchainCreateInfo sci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    sci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
                    | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sci.arraySize   = 1;
    sci.mipCount    = 1;
    sci.faceCount   = 1;
    sci.sampleCount = 1;
    sci.width       = bd->SwapchainW;
    sci.height      = bd->SwapchainH;

#ifdef IMGUI_IMPL_OPENXR_VULKAN
    sci.format = (int64_t)(bd->Info.SwapchainFormat != 0
        ? bd->Info.SwapchainFormat
        : (VkFormat)VK_FORMAT_R8G8B8A8_UNORM);
    bd->FinalFormat = (VkFormat)sci.format;
#endif
#ifdef IMGUI_IMPL_OPENXR_OPENGL
    // GL_RGBA8
    sci.format = 0x8058;
#endif

    if (XR_FAILED(xrCreateSwapchain(info.Session, &sci, &bd->Swapchain)))
    {
        fprintf(stderr, "[ImGui-OpenXR] xrCreateSwapchain failed\n");
        return false;
    }

    // Enumerate swapchain images
    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(bd->Swapchain, 0, &imgCount, nullptr);

#ifdef IMGUI_IMPL_OPENXR_VULKAN
    std::vector<XrSwapchainImageVulkanKHR> xrImgs(imgCount,
        { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
    xrEnumerateSwapchainImages(bd->Swapchain, imgCount, &imgCount,
        (XrSwapchainImageBaseHeader*)xrImgs.data());
    if (!VK_CreateImageViews(bd, xrImgs)) return false;
#endif

#ifdef IMGUI_IMPL_OPENXR_OPENGL
    std::vector<XrSwapchainImageOpenGLKHR> xrImgs(imgCount,
        { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR });
    xrEnumerateSwapchainImages(bd->Swapchain, imgCount, &imgCount,
        (XrSwapchainImageBaseHeader*)xrImgs.data());
    bd->Images.resize(imgCount);
    for (uint32_t i = 0; i < imgCount; i++)
        bd->Images[i].Width = bd->SwapchainW,
        bd->Images[i].Height = bd->SwapchainH;
    (void)xrImgs; // OpenGL textures accessed by name
#endif

    return true;
}

// ============================================================================
//  Reference spaces
// ============================================================================
static bool CreateSpaces(ImGui_ImplOpenXR_Data* bd)
{
    XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsci.poseInReferenceSpace = { {0,0,0,1}, {0,0,0} };

    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (XR_FAILED(xrCreateReferenceSpace(bd->Info.Session, &rsci, &bd->HeadSpace)))
        return false;

    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (XR_FAILED(xrCreateReferenceSpace(bd->Info.Session, &rsci, &bd->LocalSpace)))
        return false;

    return true;
}

// ============================================================================
//  Action set / controller input
// ============================================================================
static bool CreateActions(ImGui_ImplOpenXR_Data* bd)
{
    XrActionSetCreateInfo asci = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy(asci.actionSetName,          "imgui_input");
    strcpy(asci.localizedActionSetName, "ImGui Input");
    asci.priority = 0;
    if (XR_FAILED(xrCreateActionSet(bd->Info.Instance, &asci, &bd->ActionSet)))
        return false;

    xrStringToPath(bd->Info.Instance,
        "/user/hand/left",  &bd->HandPath[0]);
    xrStringToPath(bd->Info.Instance,
        "/user/hand/right", &bd->HandPath[1]);

    auto createAction = [&](const char* name, XrActionType type, XrAction* out) {
        XrActionCreateInfo aci = { XR_TYPE_ACTION_CREATE_INFO };
        strcpy(aci.actionName,          name);
        strcpy(aci.localizedActionName, name);
        aci.actionType          = type;
        aci.countSubactionPaths = 2;
        aci.subactionPaths      = bd->HandPath;
        return XR_SUCCEEDED(xrCreateAction(bd->ActionSet, &aci, out));
    };

    if (!createAction("thumbstick",  XR_ACTION_TYPE_VECTOR2F_INPUT, &bd->ThumbstickAction)) return false;
    if (!createAction("trigger",     XR_ACTION_TYPE_FLOAT_INPUT,    &bd->TriggerAction))    return false;
    if (!createAction("a_button",    XR_ACTION_TYPE_BOOLEAN_INPUT,  &bd->AButtonAction))    return false;

    // Suggest bindings for both Oculus Touch and generic controllers
    auto bindPath = [&](const char* path, XrAction action, std::vector<XrActionSuggestedBinding>& out) {
        XrPath p;
        xrStringToPath(bd->Info.Instance, path, &p);
        out.push_back({ action, p });
    };

    // --- Oculus Touch ---
    {
        std::vector<XrActionSuggestedBinding> bindings;
        bindPath("/user/hand/left/input/thumbstick",            bd->ThumbstickAction, bindings);
        bindPath("/user/hand/right/input/thumbstick",           bd->ThumbstickAction, bindings);
        bindPath("/user/hand/left/input/trigger/value",         bd->TriggerAction,    bindings);
        bindPath("/user/hand/right/input/trigger/value",        bd->TriggerAction,    bindings);
        bindPath("/user/hand/right/input/a/click",              bd->AButtonAction,    bindings);

        XrPath profile;
        xrStringToPath(bd->Info.Instance,
            "/interaction_profiles/oculus/touch_controller", &profile);

        XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        sb.interactionProfile   = profile;
        sb.suggestedBindings    = bindings.data();
        sb.countSuggestedBindings = (uint32_t)bindings.size();
        xrSuggestInteractionProfileBindings(bd->Info.Instance, &sb);
    }

    // --- Simple controller fallback ---
    {
        std::vector<XrActionSuggestedBinding> bindings;
        bindPath("/user/hand/left/input/select/click",   bd->TriggerAction, bindings);
        bindPath("/user/hand/right/input/select/click",  bd->TriggerAction, bindings);

        XrPath profile;
        xrStringToPath(bd->Info.Instance,
            "/interaction_profiles/khr/simple_controller", &profile);

        XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        sb.interactionProfile     = profile;
        sb.suggestedBindings      = bindings.data();
        sb.countSuggestedBindings = (uint32_t)bindings.size();
        xrSuggestInteractionProfileBindings(bd->Info.Instance, &sb);
    }

    // Attach action set to session
    XrSessionActionSetsAttachInfo ai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    ai.countActionSets = 1;
    ai.actionSets      = &bd->ActionSet;
    xrAttachSessionActionSets(bd->Info.Session, &ai);

    return true;
}

// ============================================================================
//  Public API
// ============================================================================

IMGUI_IMPL_API bool ImGui_ImplOpenXR_Init(ImGui_ImplOpenXR_InitInfo* info)
{
    ImGuiIO& io = ImGui::GetIO();
    IM_ASSERT(io.BackendPlatformUserData == nullptr &&
        "Already initialised! Call ImGui_ImplOpenXR_Shutdown() first.");

    auto* bd = IM_NEW(ImGui_ImplOpenXR_Data)();
    bd->Info = *info;
    io.BackendPlatformUserData = bd;
    io.BackendPlatformName     = "imgui_impl_openxr";

    // No keyboard in VR — users navigate with controller.
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;

    if (!CreateSpaces(bd))    { fprintf(stderr, "[ImGui-OpenXR] CreateSpaces failed\n");  return false; }
    if (!CreateActions(bd))   { fprintf(stderr, "[ImGui-OpenXR] CreateActions failed\n"); return false; }
    if (!CreateSwapchain(bd)) return false;

#ifdef IMGUI_IMPL_OPENXR_VULKAN
    if (!VK_CreateRenderPass(bd))    return false;
    if (!VK_AllocCommandBuffer(bd))  return false;
    if (!VK_CreateDescriptorPool(bd)) return false;
    if (!VK_InitImGuiRenderer(bd))   return false;

    // Upload fonts
    {
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(bd->CmdBuf, &bi);
        ImGui_ImplVulkan_CreateFontsTexture();
        vkEndCommandBuffer(bd->CmdBuf);
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &bd->CmdBuf;
        vkQueueSubmit(bd->Info.VkQueue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(bd->Info.VkQueue);
    }
#endif

#ifdef IMGUI_IMPL_OPENXR_OPENGL
    ImGui_ImplOpenGL3_Init("#version 330");
#endif

    bd->Initialised = true;
    return true;
}

IMGUI_IMPL_API void ImGui_ImplOpenXR_Shutdown()
{
    auto* bd = ImGui_ImplOpenXR_GetBackendData();
    if (!bd) return;

#ifdef IMGUI_IMPL_OPENXR_VULKAN
    vkDeviceWaitIdle(bd->Info.VkDevice);
    ImGui_ImplVulkan_Shutdown();
    for (auto& img : bd->Images)
    {
        if (img.Framebuffer) vkDestroyFramebuffer(bd->Info.VkDevice, img.Framebuffer, nullptr);
        if (img.ImageView)   vkDestroyImageView(bd->Info.VkDevice, img.ImageView, nullptr);
    }
    if (bd->RenderPass) vkDestroyRenderPass(bd->Info.VkDevice, bd->RenderPass, nullptr);
    if (bd->DescPool)   vkDestroyDescriptorPool(bd->Info.VkDevice, bd->DescPool, nullptr);
    if (bd->CmdPool)    vkDestroyCommandPool(bd->Info.VkDevice, bd->CmdPool, nullptr);
    if (bd->Fence)      vkDestroyFence(bd->Info.VkDevice, bd->Fence, nullptr);
#endif
#ifdef IMGUI_IMPL_OPENXR_OPENGL
    ImGui_ImplOpenGL3_Shutdown();
#endif

    if (bd->Swapchain)   xrDestroySwapchain(bd->Swapchain);
    if (bd->HeadSpace)   xrDestroySpace(bd->HeadSpace);
    if (bd->LocalSpace)  xrDestroySpace(bd->LocalSpace);
    if (bd->ActionSet)   xrDestroyActionSet(bd->ActionSet);

    ImGui::GetIO().BackendPlatformUserData = nullptr;
    ImGui::GetIO().BackendPlatformName     = nullptr;
    IM_DELETE(bd);
}

IMGUI_IMPL_API void ImGui_ImplOpenXR_NewFrame()
{
    auto* bd = ImGui_ImplOpenXR_GetBackendData();
    IM_ASSERT(bd != nullptr && "Did you call ImGui_ImplOpenXR_Init()?");

    ImGuiIO& io = ImGui::GetIO();

    // Set display size to our swapchain quad (so ImGui lays out correctly)
    io.DisplaySize             = ImVec2((float)bd->SwapchainW, (float)bd->SwapchainH);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    // Static delta time (60fps target; replace with real timer if desired)
    io.DeltaTime = 1.0f / 90.0f;   // typical VR refresh

#ifdef IMGUI_IMPL_OPENXR_VULKAN
    ImGui_ImplVulkan_NewFrame();
#endif
#ifdef IMGUI_IMPL_OPENXR_OPENGL
    ImGui_ImplOpenGL3_NewFrame();
#endif
}

IMGUI_IMPL_API void ImGui_ImplOpenXR_ProcessControllerInput(
    XrSpace referenceSpace, XrTime predictedTime)
{
    auto* bd = ImGui_ImplOpenXR_GetBackendData();
    if (!bd) return;

    // Sync actions
    XrActiveActionSet aas = { bd->ActionSet, XR_NULL_PATH };
    XrActionsSyncInfo si  = { XR_TYPE_ACTIONS_SYNC_INFO };
    si.countActiveActionSets = 1;
    si.activeActionSets      = &aas;
    xrSyncActions(bd->Info.Session, &si);

    XrPath hand = bd->HandPath[bd->UseRightHand ? 1 : 0];

    // Thumbstick → mouse delta
    {
        XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
        XrActionStateGetInfo gi     = { XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action         = bd->ThumbstickAction;
        gi.subactionPath  = hand;
        if (XR_SUCCEEDED(xrGetActionStateVector2f(bd->Info.Session, &gi, &state)) && state.isActive)
        {
            ImGuiIO& io = ImGui::GetIO();
            // Scale thumbstick to pixel/frame delta (tune multiplier to taste)
            const float speed = 8.0f;
            io.MousePos.x = ImClamp(io.MousePos.x + state.currentState.x * speed,
                                    0.0f, (float)bd->SwapchainW);
            io.MousePos.y = ImClamp(io.MousePos.y - state.currentState.y * speed,
                                    0.0f, (float)bd->SwapchainH);
        }
    }

    // Trigger / A button → left mouse
    {
        bool pressed = false;

        XrActionStateFloat trigState = { XR_TYPE_ACTION_STATE_FLOAT };
        XrActionStateGetInfo gi      = { XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action        = bd->TriggerAction;
        gi.subactionPath = hand;
        if (XR_SUCCEEDED(xrGetActionStateFloat(bd->Info.Session, &gi, &trigState)) && trigState.isActive)
            pressed |= (trigState.currentState > 0.6f);

        XrActionStateBoolean btnState = { XR_TYPE_ACTION_STATE_BOOLEAN };
        gi.action = bd->AButtonAction;
        if (XR_SUCCEEDED(xrGetActionStateBoolean(bd->Info.Session, &gi, &btnState)) && btnState.isActive)
            pressed |= (bool)btnState.currentState;

        ImGui::GetIO().MouseDown[0] = pressed;
    }

    bd->PredictedTime = predictedTime;
}

IMGUI_IMPL_API void ImGui_ImplOpenXR_SetActiveHand(bool rightHand)
{
    auto* bd = ImGui_ImplOpenXR_GetBackendData();
    if (bd) bd->UseRightHand = rightHand;
}

IMGUI_IMPL_API XrSwapchain ImGui_ImplOpenXR_GetSwapchain()
{
    auto* bd = ImGui_ImplOpenXR_GetBackendData();
    return bd ? bd->Swapchain : XR_NULL_HANDLE;
}

// ============================================================================
//  EndFrame — injects ImGui as a quad composition layer
// ============================================================================
IMGUI_IMPL_API XrResult ImGui_ImplOpenXR_EndFrame(
    XrSession session, const XrFrameEndInfo* frameEndInfo)
{
    auto* bd = ImGui_ImplOpenXR_GetBackendData();
    if (!bd || !bd->Initialised) return XR_ERROR_RUNTIME_FAILURE;

    // Render ImGui
    ImGui::Render();

    // Acquire swapchain image
    XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    uint32_t imageIndex = 0;
    XrResult res = xrAcquireSwapchainImage(bd->Swapchain, &acquireInfo, &imageIndex);
    if (XR_FAILED(res)) return bd->Real_xrEndFrame(session, frameEndInfo);

    XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(bd->Swapchain, &waitInfo);

    // Render into that image
#ifdef IMGUI_IMPL_OPENXR_VULKAN
    VK_RenderImGui(bd, imageIndex);
#endif
#ifdef IMGUI_IMPL_OPENXR_OPENGL
    // Bind the FBO / texture for this image and call ImGui_ImplOpenGL3_RenderDrawData
    // (left as an exercise — requires FBO setup similar to Vulkan above)
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#endif

    XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(bd->Swapchain, &releaseInfo);

    // --- Build the quad layer ---
    // Position: 1.5 m in front of the VIEW space origin (head).
    XrCompositionLayerQuad quadLayer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    quadLayer.layerFlags  = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
                          | XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT;
    quadLayer.space       = bd->LocalSpace;
    quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;

    // Sub-image (whole swapchain image)
    quadLayer.subImage.swapchain                  = bd->Swapchain;
    quadLayer.subImage.imageRect.offset           = { 0, 0 };
    quadLayer.subImage.imageRect.extent           = { (int32_t)bd->SwapchainW,
                                                      (int32_t)bd->SwapchainH };
    quadLayer.subImage.imageArrayIndex            = 0;

    // Pose: place in front of player.
    // In LOCAL space: estimate head is at ~1.6 m above floor, facing -Z.
    // The billboard flag will make it face the camera via runtime or we do it in VIEW space.
    if (bd->Info.Billboard)
    {
        // Switch to VIEW space — always faces the user.
        quadLayer.space = bd->HeadSpace;
        quadLayer.pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
        quadLayer.pose.position    = { 0.0f, 0.0f, -bd->Info.QuadDistance };
    }
    else
    {
        quadLayer.space = bd->LocalSpace;
        quadLayer.pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
        quadLayer.pose.position    = { 0.0f, 1.4f, -bd->Info.QuadDistance };
    }

    quadLayer.size = { bd->Info.QuadWidth, bd->Info.QuadHeight };

    // Append our layer to whatever the game submitted.
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    for (uint32_t i = 0; i < frameEndInfo->layerCount; i++)
        layers.push_back(frameEndInfo->layers[i]);
    layers.push_back((const XrCompositionLayerBaseHeader*)&quadLayer);

    XrFrameEndInfo newInfo = *frameEndInfo;
    newInfo.layerCount = (uint32_t)layers.size();
    newInfo.layers     = layers.data();

    return bd->Real_xrEndFrame(session, &newInfo);
}

// ============================================================================
//  Hook installation
// ============================================================================

IMGUI_IMPL_API void ImGui_ImplOpenXR_InstallHooks(ImGui_ImplOpenXR_HookFunctions* out)
{
    auto* bd = ImGui_ImplOpenXR_GetBackendData();
    IM_ASSERT(bd != nullptr && "Call ImGui_ImplOpenXR_Init first.");
    bd->Real_xrEndFrame   = out->Real_xrEndFrame;
    out->Detour_xrEndFrame = ImGui_ImplOpenXR_xrEndFrame_Detour;
}

IMGUI_IMPL_API XrResult XRAPI_CALL ImGui_ImplOpenXR_xrEndFrame_Detour(
    XrSession session, const XrFrameEndInfo* frameEndInfo)
{
    return ImGui_ImplOpenXR_EndFrame(session, frameEndInfo);
}

// ============================================================================
//  MinHook helper
// ============================================================================
#ifdef IMGUI_IMPL_OPENXR_USE_MINHOOK

#include <windows.h>

IMGUI_IMPL_API bool ImGui_ImplOpenXR_InstallHook_MinHook()
{
    auto* bd = ImGui_ImplOpenXR_GetBackendData();
    IM_ASSERT(bd != nullptr && "Call ImGui_ImplOpenXR_Init first.");

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED)
    {
        fprintf(stderr, "[ImGui-OpenXR] MH_Initialize failed\n");
        return false;
    }

    // Resolve xrEndFrame from the loader DLL.
    HMODULE hLoader = GetModuleHandleA("openxr_loader.dll");
    if (!hLoader) hLoader = LoadLibraryA("openxr_loader.dll");
    if (!hLoader)
    {
        fprintf(stderr, "[ImGui-OpenXR] openxr_loader.dll not found\n");
        return false;
    }

    void* pTarget = (void*)GetProcAddress(hLoader, "xrEndFrame");
    if (!pTarget)
    {
        fprintf(stderr, "[ImGui-OpenXR] xrEndFrame not found in loader\n");
        return false;
    }

    void* pOriginal = nullptr;
    MH_STATUS st = MH_CreateHook(pTarget,
        (void*)ImGui_ImplOpenXR_xrEndFrame_Detour, &pOriginal);
    if (st != MH_OK)
    {
        fprintf(stderr, "[ImGui-OpenXR] MH_CreateHook failed: %d\n", st);
        return false;
    }

    bd->Real_xrEndFrame = (PFN_xrEndFrame)pOriginal;

    if (MH_EnableHook(pTarget) != MH_OK)
    {
        fprintf(stderr, "[ImGui-OpenXR] MH_EnableHook failed\n");
        return false;
    }

    fprintf(stdout, "[ImGui-OpenXR] xrEndFrame hooked via MinHook\n");
    return true;
}

#endif // IMGUI_IMPL_OPENXR_USE_MINHOOK

// ============================================================================
//  Dobby helper (Android / Linux)
// ============================================================================
#ifdef IMGUI_IMPL_OPENXR_USE_DOBBY

#include <dlfcn.h>

IMGUI_IMPL_API bool ImGui_ImplOpenXR_InstallHook_Dobby()
{
    auto* bd = ImGui_ImplOpenXR_GetBackendData();
    IM_ASSERT(bd != nullptr && "Call ImGui_ImplOpenXR_Init first.");

    // On Android the loader may be libopenxr_loader.so or embedded in the APK.
    const char* candidates[] = {
        "libopenxr_loader.so",
        "libopenxr_loader_arm64.so",
        nullptr
    };

    void* pTarget = nullptr;
    for (const char** name = candidates; *name && !pTarget; name++)
    {
        void* lib = dlopen(*name, RTLD_NOLOAD | RTLD_NOW);
        if (lib) pTarget = dlsym(lib, "xrEndFrame");
    }

    if (!pTarget)
    {
        // Try global symbol table (loader already linked in)
        pTarget = dlsym(RTLD_DEFAULT, "xrEndFrame");
    }

    if (!pTarget)
    {
        fprintf(stderr, "[ImGui-OpenXR] Cannot find xrEndFrame symbol\n");
        return false;
    }

    void* original = nullptr;
    DobbyHook(pTarget, (void*)ImGui_ImplOpenXR_xrEndFrame_Detour, &original);
    bd->Real_xrEndFrame = (PFN_xrEndFrame)original;

    fprintf(stdout, "[ImGui-OpenXR] xrEndFrame hooked via Dobby\n");
    return true;
}

#endif // IMGUI_IMPL_OPENXR_USE_DOBBY

#endif // IMGUI_DISABLE
