//
// RT64
//

#pragma once

#include <array>
#include <memory>
#include <vector>

#include "common/rt64_common.h"
#include "common/rt64_plume.h"

namespace RT64 {
    struct RenderWorker;

    // Per-eye camera override applied by the projection processor to the perspective projections that belong to the game's camera.
    // The game's camera is used as the origin of the VR tracking space, so the head pose is applied on top of it (third person "diorama" view).
    struct VREyeOverride {
        // Transforms from the game's camera space to the eye's space. Uses row vectors like the rest of RT64. Translation is in game units.
        hlslpp::float4x4 cameraToEye;
        float tanLeft = -1.0f;
        float tanRight = 1.0f;
        float tanUp = 1.0f;
        float tanDown = -1.0f;
        bool levelHorizon = true;
        const std::vector<uint32_t> *cameraMatrixIds = nullptr;

        bool matchesProjection(uint32_t matrixId, const hlslpp::float4x4 &viewMatrix) const;
        hlslpp::float4x4 applyView(const hlslpp::float4x4 &gameViewMatrix) const;
        hlslpp::float4x4 projection(const hlslpp::float4x4 &gameProjMatrix) const;
    };

    // Optional OpenXR output. Renders the game twice per displayed frame, once for each eye, and submits the result to the headset.
    // Only D3D12 is supported at the moment.
    struct VRSystem {
        struct Configuration {
            // Game units per real world meter. Bigger values make the world look smaller (more like a diorama).
            float worldScale = 100.0f;

            // Removes the pitch and roll from the game's camera so the horizon stays level and the player can look around freely.
            bool levelHorizon = true;

            // Matrix group IDs of the projections that belong to the game's camera. Every perspective projection with a non-identity view is used if empty.
            std::vector<uint32_t> cameraMatrixIds;
        };

        struct Internal;
        std::unique_ptr<Internal> internal;
        Configuration config;

        VRSystem();
        ~VRSystem();

        // Whether VR was requested by the user through the RT64_VR environment variable.
        static bool requested();

        bool setup(RenderDevice *device, RenderCommandQueue *commandQueue);
        bool isSessionRunning() const;
        uint32_t refreshRate() const;
        uint32_t recommendedHeight() const;

        // Waits for the next headset frame. Returns true if the frame must be rendered, in which case submitEye() must be called for both eyes.
        // endFrame() must always be called after a successful call to beginFrame(), regardless of what it returned.
        bool beginFrame(bool &frameBegun);
        const VREyeOverride &eyeOverride(uint32_t eyeIndex) const;
        void submitEye(uint32_t eyeIndex, RenderWorker *worker, RenderTexture *srcTexture, uint32_t width, uint32_t height);
        void endFrame();
    };
};
