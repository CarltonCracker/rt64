//
// RT64
//

#include "rt64_vr.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/rt64_math.h"
#include "render/rt64_render_worker.h"

#if RT64_VR_OPENXR
#   include "plume_d3d12.h"
#   define XR_USE_PLATFORM_WIN32
#   define XR_USE_GRAPHICS_API_D3D12
#   include <openxr/openxr.h>
#   include <openxr/openxr_platform.h>
#endif

namespace RT64 {
    static const char *getEnvironmentVariable(const char *name) {
        const char *value = std::getenv(name);
        return ((value != nullptr) && (value[0] != '\0')) ? value : nullptr;
    }

    static bool getEnvironmentBool(const char *name, bool defaultValue) {
        const char *value = getEnvironmentVariable(name);
        if (value == nullptr) {
            return defaultValue;
        }

        return (strcmp(value, "0") != 0) && (strcmp(value, "false") != 0) && (strcmp(value, "off") != 0);
    }

    // VREyeOverride

    bool VREyeOverride::matchesProjection(uint32_t matrixId, const hlslpp::float4x4 &viewMatrix) const {
        if ((cameraMatrixIds != nullptr) && !cameraMatrixIds->empty()) {
            return std::find(cameraMatrixIds->begin(), cameraMatrixIds->end(), matrixId) != cameraMatrixIds->end();
        }
        else {
            return !isMatrixIdentity(viewMatrix);
        }
    }

    hlslpp::float4x4 VREyeOverride::applyView(const hlslpp::float4x4 &gameViewMatrix) const {
        hlslpp::float4x4 viewMatrix = gameViewMatrix;
        if (levelHorizon) {
            // Rebuild the camera's orientation using only its yaw. The rows of the inverse view matrix are the camera's axes in world space.
            const hlslpp::float4x4 invViewMatrix = hlslpp::inverse(gameViewMatrix);
            const hlslpp::float3 cameraRight = invViewMatrix[0].xyz;
            const hlslpp::float3 cameraUp = invViewMatrix[1].xyz;
            const hlslpp::float3 cameraBack = invViewMatrix[2].xyz;
            hlslpp::float3 flatBack(cameraBack.x, 0.0f, cameraBack.z);
            float flatLength = hlslpp::length(flatBack);

            // When the camera is looking straight down, its up vector points towards where the camera is facing.
            if (flatLength < 1e-4f) {
                flatBack = hlslpp::float3(-cameraUp.x, 0.0f, -cameraUp.z);
                flatLength = hlslpp::length(flatBack);
            }

            if (flatLength >= 1e-4f) {
                const hlslpp::float3 levelBack = flatBack / flatLength;
                const hlslpp::float3 levelUp(0.0f, 1.0f, 0.0f);
                const hlslpp::float3 levelRight = hlslpp::cross(levelUp, levelBack);

                // Preserve any scaling the game might've applied to its view.
                hlslpp::float4x4 levelInvViewMatrix = invViewMatrix;
                levelInvViewMatrix[0] = hlslpp::float4(levelRight * hlslpp::length(cameraRight), 0.0f);
                levelInvViewMatrix[1] = hlslpp::float4(levelUp * hlslpp::length(cameraUp), 0.0f);
                levelInvViewMatrix[2] = hlslpp::float4(levelBack * hlslpp::length(cameraBack), 0.0f);
                viewMatrix = hlslpp::inverse(levelInvViewMatrix);
            }
        }

        return hlslpp::mul(viewMatrix, cameraToEye);
    }

    hlslpp::float4x4 VREyeOverride::projection(const hlslpp::float4x4 &gameProjMatrix) const {
        // Keep the same clip planes the game used so depth precision and fog behave the same way. The matrix follows the same OpenGL-style convention as guPerspective.
        const float nearPlane = nearPlaneFromProj(gameProjMatrix);
        const float farPlane = std::max(farPlaneFromProj(gameProjMatrix), nearPlane + 1.0f);
        const float tanWidth = tanRight - tanLeft;
        const float tanHeight = tanUp - tanDown;
        hlslpp::float4x4 proj(0.0f);
        proj[0][0] = 2.0f / tanWidth;
        proj[1][1] = 2.0f / tanHeight;
        proj[2][0] = (tanRight + tanLeft) / tanWidth;
        proj[2][1] = (tanUp + tanDown) / tanHeight;
        proj[2][2] = -(farPlane + nearPlane) / (farPlane - nearPlane);
        proj[2][3] = -1.0f;
        proj[3][2] = -(2.0f * farPlane * nearPlane) / (farPlane - nearPlane);
        return proj;
    }

    // VRSystem

#if RT64_VR_OPENXR
    struct VRSystem::Internal {
        struct EyeSwapchain {
            XrSwapchain handle = XR_NULL_HANDLE;
            uint32_t width = 0;
            uint32_t height = 0;
            std::vector<std::unique_ptr<D3D12Texture>> textures;
        };

        D3D12Device *device = nullptr;
        XrInstance instance = XR_NULL_HANDLE;
        XrSystemId systemId = XR_NULL_SYSTEM_ID;
        XrSession session = XR_NULL_HANDLE;
        XrSpace space = XR_NULL_HANDLE;
        XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
        bool sessionRunning = false;
        bool sessionLost = false;
        int64_t swapchainFormat = 0;
        std::array<EyeSwapchain, 2> swapchains;
        std::array<XrView, 2> views = {};
        std::array<XrCompositionLayerProjectionView, 2> layerViews = {};
        std::array<VREyeOverride, 2> eyes;
        uint32_t eyesSubmitted = 0;
        XrTime predictedDisplayTime = 0;
        uint32_t refreshRate = 90;
        uint32_t recommendedHeight = 0;

        ~Internal() {
            for (EyeSwapchain &swapchain : swapchains) {
                destroySwapchain(swapchain);
            }

            if (space != XR_NULL_HANDLE) {
                xrDestroySpace(space);
            }

            if (session != XR_NULL_HANDLE) {
                xrDestroySession(session);
            }

            if (instance != XR_NULL_HANDLE) {
                xrDestroyInstance(instance);
            }
        }

        static void destroySwapchain(EyeSwapchain &swapchain) {
            swapchain.textures.clear();
            if (swapchain.handle != XR_NULL_HANDLE) {
                xrDestroySwapchain(swapchain.handle);
                swapchain.handle = XR_NULL_HANDLE;
            }

            swapchain.width = 0;
            swapchain.height = 0;
        }

        bool checkResult(XrResult result, const char *operation) {
            if (XR_SUCCEEDED(result)) {
                return true;
            }

            char resultString[XR_MAX_RESULT_STRING_SIZE] = {};
            if ((instance == XR_NULL_HANDLE) || XR_FAILED(xrResultToString(instance, result, resultString))) {
                snprintf(resultString, sizeof(resultString), "%d", int(result));
            }

            fprintf(stderr, "[RT64 VR] %s failed: %s\n", operation, resultString);
            return false;
        }

        void pollEvents() {
            XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
            while (xrPollEvent(instance, &event) == XR_SUCCESS) {
                switch (event.type) {
                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                    const XrEventDataSessionStateChanged *stateChanged = reinterpret_cast<const XrEventDataSessionStateChanged *>(&event);
                    sessionState = stateChanged->state;
                    if (sessionState == XR_SESSION_STATE_READY) {
                        XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
                        beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        sessionRunning = checkResult(xrBeginSession(session, &beginInfo), "xrBeginSession");
                    }
                    else if (sessionState == XR_SESSION_STATE_STOPPING) {
                        checkResult(xrEndSession(session), "xrEndSession");
                        sessionRunning = false;
                    }
                    else if ((sessionState == XR_SESSION_STATE_EXITING) || (sessionState == XR_SESSION_STATE_LOSS_PENDING)) {
                        sessionRunning = false;
                        sessionLost = true;
                    }

                    break;
                }
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                    sessionRunning = false;
                    sessionLost = true;
                    break;
                default:
                    break;
                }

                event = { XR_TYPE_EVENT_DATA_BUFFER };
            }
        }

        bool ensureSwapchain(uint32_t eyeIndex, uint32_t width, uint32_t height) {
            EyeSwapchain &swapchain = swapchains[eyeIndex];
            if ((swapchain.handle != XR_NULL_HANDLE) && (swapchain.width == width) && (swapchain.height == height)) {
                return true;
            }

            destroySwapchain(swapchain);

            XrSwapchainCreateInfo createInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
            createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            createInfo.format = swapchainFormat;
            createInfo.sampleCount = 1;
            createInfo.width = width;
            createInfo.height = height;
            createInfo.faceCount = 1;
            createInfo.arraySize = 1;
            createInfo.mipCount = 1;
            if (!checkResult(xrCreateSwapchain(session, &createInfo, &swapchain.handle), "xrCreateSwapchain")) {
                swapchain.handle = XR_NULL_HANDLE;
                return false;
            }

            uint32_t imageCount = 0;
            if (!checkResult(xrEnumerateSwapchainImages(swapchain.handle, 0, &imageCount, nullptr), "xrEnumerateSwapchainImages")) {
                destroySwapchain(swapchain);
                return false;
            }

            std::vector<XrSwapchainImageD3D12KHR> images(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
            if (!checkResult(xrEnumerateSwapchainImages(swapchain.handle, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data())), "xrEnumerateSwapchainImages")) {
                destroySwapchain(swapchain);
                return false;
            }

            // Wrap the runtime's images so they can be used with the render interface. The runtime owns the resources and hands them over in the render target state.
            for (const XrSwapchainImageD3D12KHR &image : images) {
                std::unique_ptr<D3D12Texture> texture = std::make_unique<D3D12Texture>();
                texture->d3d = image.texture;
                texture->device = device;
                // The render interface has no sRGB formats, but the format is only used for describing the copy and both formats are copy compatible.
                texture->desc = RenderTextureDesc::ColorTarget(width, height, RenderFormat::R8G8B8A8_UNORM);
                texture->resourceStates = D3D12_RESOURCE_STATE_RENDER_TARGET;
                texture->layout = RenderTextureLayout::COLOR_WRITE;
                swapchain.textures.emplace_back(std::move(texture));
            }

            swapchain.width = width;
            swapchain.height = height;
            return true;
        }
    };

    VRSystem::VRSystem() { }

    VRSystem::~VRSystem() { }

    bool VRSystem::requested() {
        return getEnvironmentBool("RT64_VR", false);
    }

    bool VRSystem::setup(RenderDevice *device, RenderCommandQueue *commandQueue) {
        const char *worldScale = getEnvironmentVariable("RT64_VR_WORLD_SCALE");
        if (worldScale != nullptr) {
            config.worldScale = std::max(float(std::atof(worldScale)), 0.001f);
        }

        config.levelHorizon = getEnvironmentBool("RT64_VR_LEVEL_HORIZON", config.levelHorizon);

        const char *cameraIds = getEnvironmentVariable("RT64_VR_CAMERA_IDS");
        if (cameraIds != nullptr) {
            config.cameraMatrixIds.clear();
            const char *cursor = cameraIds;
            while (*cursor != '\0') {
                char *end = nullptr;
                const unsigned long id = std::strtoul(cursor, &end, 0);
                if (end == cursor) {
                    cursor++;
                    continue;
                }

                config.cameraMatrixIds.emplace_back(uint32_t(id));
                cursor = end;
            }
        }

        D3D12Device *d3d12Device = dynamic_cast<D3D12Device *>(device);
        D3D12CommandQueue *d3d12Queue = dynamic_cast<D3D12CommandQueue *>(commandQueue);
        if ((d3d12Device == nullptr) || (d3d12Queue == nullptr)) {
            fprintf(stderr, "[RT64 VR] VR output requires the D3D12 graphics API.\n");
            return false;
        }

        internal = std::make_unique<Internal>();
        internal->device = d3d12Device;

        uint32_t extensionCount = 0;
        if (!internal->checkResult(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr), "xrEnumerateInstanceExtensionProperties")) {
            internal.reset();
            return false;
        }

        std::vector<XrExtensionProperties> extensions(extensionCount, { XR_TYPE_EXTENSION_PROPERTIES });
        xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data());
        const bool d3d12Supported = std::any_of(extensions.begin(), extensions.end(), [](const XrExtensionProperties &properties) {
            return strcmp(properties.extensionName, XR_KHR_D3D12_ENABLE_EXTENSION_NAME) == 0;
        });

        if (!d3d12Supported) {
            fprintf(stderr, "[RT64 VR] The active OpenXR runtime doesn't support D3D12.\n");
            internal.reset();
            return false;
        }

        const char *enabledExtensions[] = { XR_KHR_D3D12_ENABLE_EXTENSION_NAME };
        XrInstanceCreateInfo instanceInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
        strncpy(instanceInfo.applicationInfo.applicationName, "RT64", XR_MAX_APPLICATION_NAME_SIZE - 1);
        strncpy(instanceInfo.applicationInfo.engineName, "RT64", XR_MAX_ENGINE_NAME_SIZE - 1);
        instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        instanceInfo.enabledExtensionCount = uint32_t(std::size(enabledExtensions));
        instanceInfo.enabledExtensionNames = enabledExtensions;
        if (!internal->checkResult(xrCreateInstance(&instanceInfo, &internal->instance), "xrCreateInstance")) {
            internal.reset();
            return false;
        }

        XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        if (!internal->checkResult(xrGetSystem(internal->instance, &systemInfo, &internal->systemId), "xrGetSystem")) {
            internal.reset();
            return false;
        }

        // The runtime requires the graphics requirements to be queried before creating the session.
        PFN_xrGetD3D12GraphicsRequirementsKHR getD3D12GraphicsRequirements = nullptr;
        xrGetInstanceProcAddr(internal->instance, "xrGetD3D12GraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction *>(&getD3D12GraphicsRequirements));
        if (getD3D12GraphicsRequirements == nullptr) {
            fprintf(stderr, "[RT64 VR] Unable to retrieve xrGetD3D12GraphicsRequirementsKHR.\n");
            internal.reset();
            return false;
        }

        XrGraphicsRequirementsD3D12KHR graphicsRequirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
        if (!internal->checkResult(getD3D12GraphicsRequirements(internal->instance, internal->systemId, &graphicsRequirements), "xrGetD3D12GraphicsRequirementsKHR")) {
            internal.reset();
            return false;
        }

        DXGI_ADAPTER_DESC1 adapterDesc = {};
        if ((d3d12Device->adapter != nullptr) && SUCCEEDED(d3d12Device->adapter->GetDesc1(&adapterDesc))) {
            const LUID &requiredLuid = graphicsRequirements.adapterLuid;
            if ((adapterDesc.AdapterLuid.LowPart != requiredLuid.LowPart) || (adapterDesc.AdapterLuid.HighPart != requiredLuid.HighPart)) {
                fprintf(stderr, "[RT64 VR] The headset is connected to a different GPU than the one selected for rendering.\n");
                internal.reset();
                return false;
            }
        }

        XrGraphicsBindingD3D12KHR graphicsBinding = { XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
        graphicsBinding.device = d3d12Device->d3d;
        graphicsBinding.queue = d3d12Queue->d3d;

        XrSessionCreateInfo sessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
        sessionInfo.next = &graphicsBinding;
        sessionInfo.systemId = internal->systemId;
        if (!internal->checkResult(xrCreateSession(internal->instance, &sessionInfo, &internal->session), "xrCreateSession")) {
            internal.reset();
            return false;
        }

        // The local space is centered on the player's head when the session starts or when the user recenters the view through the runtime.
        XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
        if (!internal->checkResult(xrCreateReferenceSpace(internal->session, &spaceInfo, &internal->space), "xrCreateReferenceSpace")) {
            internal.reset();
            return false;
        }

        uint32_t viewCount = 0;
        xrEnumerateViewConfigurationViews(internal->instance, internal->systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
        std::vector<XrViewConfigurationView> configurationViews(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
        if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(internal->instance, internal->systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, configurationViews.data()))) {
            for (const XrViewConfigurationView &view : configurationViews) {
                internal->recommendedHeight = std::max(internal->recommendedHeight, view.recommendedImageRectHeight);
            }
        }

        // RT64 renders gamma encoded colors, so an sRGB swapchain is preferred to get the colors to show up correctly on the headset.
        uint32_t formatCount = 0;
        xrEnumerateSwapchainFormats(internal->session, 0, &formatCount, nullptr);
        std::vector<int64_t> formats(formatCount);
        xrEnumerateSwapchainFormats(internal->session, formatCount, &formatCount, formats.data());
        const int64_t preferredFormats[] = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM };
        for (int64_t preferredFormat : preferredFormats) {
            if (std::find(formats.begin(), formats.end(), preferredFormat) != formats.end()) {
                internal->swapchainFormat = preferredFormat;
                break;
            }
        }

        if (internal->swapchainFormat == 0) {
            fprintf(stderr, "[RT64 VR] The OpenXR runtime doesn't support any of the required swapchain formats.\n");
            internal.reset();
            return false;
        }

        for (VREyeOverride &eye : internal->eyes) {
            eye.levelHorizon = config.levelHorizon;
            eye.cameraMatrixIds = &config.cameraMatrixIds;
        }

        fprintf(stdout, "[RT64 VR] OpenXR session created. World scale: %.2f units per meter.\n", config.worldScale);
        return true;
    }

    bool VRSystem::isSessionRunning() const {
        return (internal != nullptr) && internal->sessionRunning;
    }

    uint32_t VRSystem::refreshRate() const {
        return (internal != nullptr) ? internal->refreshRate : 0;
    }

    uint32_t VRSystem::recommendedHeight() const {
        return (internal != nullptr) ? internal->recommendedHeight : 0;
    }

    bool VRSystem::beginFrame(bool &frameBegun) {
        frameBegun = false;
        if ((internal == nullptr) || internal->sessionLost) {
            return false;
        }

        internal->pollEvents();
        if (!internal->sessionRunning) {
            return false;
        }

        XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
        XrFrameState frameState = { XR_TYPE_FRAME_STATE };
        if (!internal->checkResult(xrWaitFrame(internal->session, &waitInfo, &frameState), "xrWaitFrame")) {
            return false;
        }

        XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
        if (!internal->checkResult(xrBeginFrame(internal->session, &beginInfo), "xrBeginFrame")) {
            return false;
        }

        frameBegun = true;
        internal->eyesSubmitted = 0;
        internal->predictedDisplayTime = frameState.predictedDisplayTime;
        if (frameState.predictedDisplayPeriod > 0) {
            internal->refreshRate = uint32_t(std::lround(1e9 / double(frameState.predictedDisplayPeriod)));
        }

        if (!frameState.shouldRender) {
            return false;
        }

        XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
        locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = internal->space;

        XrViewState viewState = { XR_TYPE_VIEW_STATE };
        uint32_t viewCount = 0;
        internal->views.fill({ XR_TYPE_VIEW });
        if (!internal->checkResult(xrLocateViews(internal->session, &locateInfo, &viewState, uint32_t(internal->views.size()), &viewCount, internal->views.data()), "xrLocateViews")) {
            return false;
        }

        const XrViewStateFlags requiredFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        if ((viewCount != 2) || ((viewState.viewStateFlags & requiredFlags) != requiredFlags)) {
            return false;
        }

        for (uint32_t i = 0; i < 2; i++) {
            const XrView &view = internal->views[i];
            const XrQuaternionf &q = view.pose.orientation;
            const XrVector3f &p = view.pose.position;

            // Build the eye's pose in tracking space with the translation converted to game units. Each row is one of the eye's axes.
            hlslpp::float4x4 eyePose;
            eyePose[0] = hlslpp::float4(1.0f - 2.0f * (q.y * q.y + q.z * q.z), 2.0f * (q.x * q.y + q.z * q.w), 2.0f * (q.x * q.z - q.y * q.w), 0.0f);
            eyePose[1] = hlslpp::float4(2.0f * (q.x * q.y - q.z * q.w), 1.0f - 2.0f * (q.x * q.x + q.z * q.z), 2.0f * (q.y * q.z + q.x * q.w), 0.0f);
            eyePose[2] = hlslpp::float4(2.0f * (q.x * q.z + q.y * q.w), 2.0f * (q.y * q.z - q.x * q.w), 1.0f - 2.0f * (q.x * q.x + q.y * q.y), 0.0f);
            eyePose[3] = hlslpp::float4(p.x * config.worldScale, p.y * config.worldScale, p.z * config.worldScale, 1.0f);

            VREyeOverride &eye = internal->eyes[i];
            eye.cameraToEye = hlslpp::inverse(eyePose);
            eye.tanLeft = tanf(view.fov.angleLeft);
            eye.tanRight = tanf(view.fov.angleRight);
            eye.tanUp = tanf(view.fov.angleUp);
            eye.tanDown = tanf(view.fov.angleDown);
        }

        return true;
    }

    const VREyeOverride &VRSystem::eyeOverride(uint32_t eyeIndex) const {
        assert(internal != nullptr);
        return internal->eyes[eyeIndex];
    }

    void VRSystem::submitEye(uint32_t eyeIndex, RenderWorker *worker, RenderTexture *srcTexture, uint32_t width, uint32_t height) {
        assert(internal != nullptr);
        assert(eyeIndex < 2);

        if ((srcTexture == nullptr) || (width == 0) || (height == 0) || !internal->ensureSwapchain(eyeIndex, width, height)) {
            return;
        }

        Internal::EyeSwapchain &swapchain = internal->swapchains[eyeIndex];
        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        if (!internal->checkResult(xrAcquireSwapchainImage(swapchain.handle, &acquireInfo, &imageIndex), "xrAcquireSwapchainImage")) {
            return;
        }

        XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        waitInfo.timeout = XR_INFINITE_DURATION;
        if (internal->checkResult(xrWaitSwapchainImage(swapchain.handle, &waitInfo), "xrWaitSwapchainImage")) {
            RenderTexture *dstTexture = swapchain.textures[imageIndex].get();
            const RenderBox srcBox(0, 0, int32_t(width), int32_t(height));
            worker->commandList->begin();

            RenderTextureBarrier copyBarriers[] = {
                RenderTextureBarrier(srcTexture, RenderTextureLayout::COPY_SOURCE),
                RenderTextureBarrier(dstTexture, RenderTextureLayout::COPY_DEST)
            };

            worker->commandList->barriers(RenderBarrierStage::COPY, copyBarriers, uint32_t(std::size(copyBarriers)));
            worker->commandList->copyTextureRegion(RenderTextureCopyLocation::Subresource(dstTexture), RenderTextureCopyLocation::Subresource(srcTexture), 0, 0, 0, &srcBox);

            // The runtime expects the image back in the render target state. The source goes back to the state the present queue expects.
            RenderTextureBarrier restoreBarriers[] = {
                RenderTextureBarrier(srcTexture, RenderTextureLayout::SHADER_READ),
                RenderTextureBarrier(dstTexture, RenderTextureLayout::COLOR_WRITE)
            };

            worker->commandList->barriers(RenderBarrierStage::GRAPHICS, restoreBarriers, uint32_t(std::size(restoreBarriers)));
            worker->commandList->end();
            worker->execute();
            worker->wait();

            XrCompositionLayerProjectionView &layerView = internal->layerViews[eyeIndex];
            layerView = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
            layerView.pose = internal->views[eyeIndex].pose;
            layerView.fov = internal->views[eyeIndex].fov;
            layerView.subImage.swapchain = swapchain.handle;
            layerView.subImage.imageRect.offset = { 0, 0 };
            layerView.subImage.imageRect.extent = { int32_t(width), int32_t(height) };
            layerView.subImage.imageArrayIndex = 0;
            internal->eyesSubmitted |= (1U << eyeIndex);
        }

        XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        internal->checkResult(xrReleaseSwapchainImage(swapchain.handle, &releaseInfo), "xrReleaseSwapchainImage");
    }

    void VRSystem::endFrame() {
        assert(internal != nullptr);

        XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        layer.space = internal->space;
        layer.viewCount = uint32_t(internal->layerViews.size());
        layer.views = internal->layerViews.data();

        const XrCompositionLayerBaseHeader *layers[] = { reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer) };
        XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
        endInfo.displayTime = internal->predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = (internal->eyesSubmitted == 0x3) ? 1 : 0;
        endInfo.layers = layers;
        internal->checkResult(xrEndFrame(internal->session, &endInfo), "xrEndFrame");
        internal->eyesSubmitted = 0;
    }
#else
    struct VRSystem::Internal {
        std::array<VREyeOverride, 2> eyes;
    };

    VRSystem::VRSystem() { }

    VRSystem::~VRSystem() { }

    bool VRSystem::requested() {
        return getEnvironmentBool("RT64_VR", false);
    }

    bool VRSystem::setup(RenderDevice *device, RenderCommandQueue *commandQueue) {
        fprintf(stderr, "[RT64 VR] This build of RT64 was compiled without VR support.\n");
        return false;
    }

    bool VRSystem::isSessionRunning() const {
        return false;
    }

    uint32_t VRSystem::refreshRate() const {
        return 0;
    }

    uint32_t VRSystem::recommendedHeight() const {
        return 0;
    }

    bool VRSystem::beginFrame(bool &frameBegun) {
        frameBegun = false;
        return false;
    }

    const VREyeOverride &VRSystem::eyeOverride(uint32_t eyeIndex) const {
        static const VREyeOverride defaultEye;
        return defaultEye;
    }

    void VRSystem::submitEye(uint32_t eyeIndex, RenderWorker *worker, RenderTexture *srcTexture, uint32_t width, uint32_t height) { }

    void VRSystem::endFrame() { }
#endif
};
