// MIT License
//
// Copyright(c) 2025 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "ShimDriverManager.h"
#include "DetourUtils.h"
#include "Tracing.h"

namespace {
    using namespace driver_shim;

    struct EyeTrackerNotSupportedException : public std::exception {
        const char* what() const throw() {
            return "Eye tracker is not supported";
        }
    };

    // The HmdShimDriver driver wraps another ITrackedDeviceServerDriver instance with the intent to override
    // properties and behaviors.
    struct HmdShimDriver : public vr::ITrackedDeviceServerDriver {
        HmdShimDriver(vr::ITrackedDeviceServerDriver* shimmedDevice, pvrEnvHandle pvr, pvrSessionHandle pvrSession)
            : m_shimmedDevice(shimmedDevice), m_pvr(pvr), m_pvrSession(pvrSession) {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdShimDriver_Ctor");

            // TODO: Add any early initialization here if needed.

            // TODO: Add capabilities detection (if not already done in Driver::Init() earlier) and throw
            // EyeTrackerNotSupportedException to skip shimming when capabilities are not available.

            TraceLoggingWriteStop(local, "HmdShimDriver_Ctor");
        }

        vr::EVRInitError Activate(uint32_t unObjectId) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdShimDriver_Activate", TLArg(unObjectId, "ObjectId"));

            // Activate the real device driver.
            m_shimmedDevice->Activate(unObjectId);

            m_deviceIndex = unObjectId;

            const vr::PropertyContainerHandle_t container =
                vr::VRProperties()->TrackedDeviceToPropertyContainer(m_deviceIndex);

            // Advertise supportsEyeGazeInteraction.
            vr::VRProperties()->SetBoolProperty(container, vr::Prop_SupportsXrEyeGazeInteraction_Bool, true);

            // Create the input component for the eye gaze. It must have the path /eyetracking and nothing else!
            vr::VRDriverInput()->CreateEyeTrackingComponent(container, "/eyetracking", &m_eyeTrackingComponent);
            TraceLoggingWriteTagged(
                local, "HmdShimDriver_Activate", TLArg(m_eyeTrackingComponent, "EyeTrackingComponent"));
            DriverLog("Eye Gaze Component: %lld", m_eyeTrackingComponent);

            // Schedule updates in a background thread.
            // TODO: Can use a callback instead of a thread here, if available.
            m_active = true;
            m_updateThread = std::thread(&HmdShimDriver::UpdateThread, this);

            TraceLoggingWriteStop(local, "HmdShimDriver_Activate");

            return vr::VRInitError_None;
        }

        void Deactivate() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdShimDriver_Deactivate", TLArg(m_deviceIndex, "ObjectId"));

            if (m_active.exchange(false)) {
                m_updateThread.join();
            }

            m_deviceIndex = vr::k_unTrackedDeviceIndexInvalid;

            m_shimmedDevice->Deactivate();

            DriverLog("Deactivated device shimmed with HmdShimDriver");

            TraceLoggingWriteStop(local, "HmdShimDriver_Deactivate");
        }

        void EnterStandby() override {
            m_shimmedDevice->EnterStandby();
        }

        void* GetComponent(const char* pchComponentNameAndVersion) override {
            return m_shimmedDevice->GetComponent(pchComponentNameAndVersion);
        }

        vr::DriverPose_t GetPose() override {
            return m_shimmedDevice->GetPose();
        }

        void DebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize) override {
            m_shimmedDevice->DebugRequest(pchRequest, pchResponseBuffer, unResponseBufferSize);
        }

        void UpdateThread() {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdShimDriver_UpdateThread");

            DriverLog("Hello from HmdShimDriver::UpdateThread");
            SetThreadDescription(GetCurrentThread(), L"HmdShimDriver_UpdateThread");

            const vr::PropertyContainerHandle_t container =
                vr::VRProperties()->TrackedDeviceToPropertyContainer(m_deviceIndex);

            vr::VREyeTrackingData_t data{};
            while (true) {
                // Wait for the next time to update.
                {
                    TraceLocalActivity(sleep);
                    TraceLoggingWriteStart(sleep, "HmdShimDriver_UpdateThread_Sleep");

                    // We refresh the data at this frequency.
                    // TODO: Use event-based sleep/wake up if appropriate.
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));

                    TraceLoggingWriteStop(sleep, "HmdShimDriver_UpdateThread_Sleep", TLArg(m_active.load(), "Active"));

                    if (!m_active) {
                        break;
                    }
                }

                // Retrieve the data from the eye tracker and push it to the input component.
                pvrEyeTrackingInfo state{};
                pvrResult result = pvr_getEyeTrackingInfo(m_pvrSession, pvr_getTimeSeconds(m_pvr), &state);
                TraceLoggingWriteTagged(local,
                                        "HmdShimDriver_PvrEyeTrackingInfo",
                                        TLArg((int)result, "Result"),
                                        TLArg(state.TimeInSeconds, "TimeInSeconds"));

                const bool isEyeTrackingDataAvailable = result == pvr_success && state.TimeInSeconds > 0;
                if (isEyeTrackingDataAvailable) {
                    TraceLoggingWriteTagged(local,
                                            "HmdShimDriver_PvrEyeTrackingInfo",
                                            TLArg(state.GazeTan[0].x, "LeftGazeTanX"),
                                            TLArg(state.GazeTan[0].y, "LeftGazeTanY"),
                                            TLArg(state.GazeTan[1].x, "RightGazeTanX"),
                                            TLArg(state.GazeTan[1].y, "RightGazeTanY"));

                    // Compute the gaze pitch/yaw angles by averaging both eyes.
                    const float angleHorizontal = atanf((state.GazeTan[0].x + state.GazeTan[1].x) / 2.f);
                    const float angleVertical = atanf((state.GazeTan[0].y + state.GazeTan[1].y) / 2.f);

                    // Use polar coordinates to create a unit vector.
                    DirectX::XMStoreFloat3(
                        (DirectX::XMFLOAT3*)&data.vGazeTarget,
                        DirectX::XMVector3Normalize(DirectX::XMVectorSet(sinf(angleHorizontal) * cosf(angleVertical),
                                                                         sinf(angleVertical),
                                                                         -cosf(angleHorizontal) * cosf(angleVertical),
                                                                         1)));
                    data.bValid = data.bTracked = data.bActive = true;
                } else {
                    // Fallback to identity.
                    DirectX::XMStoreFloat3((DirectX::XMFLOAT3*)&data.vGazeTarget, DirectX::XMVectorSet(0, 0, -1, 1));
                    data.bValid = data.bTracked = data.bActive = false;
                }
                vr::VRDriverInput()->UpdateEyeTrackingComponent(m_eyeTrackingComponent, &data, 0.f);
            }

            DriverLog("Bye from HmdShimDriver::UpdateThread");

            TraceLoggingWriteStop(local, "HmdShimDriver_UpdateThread");
        }

        vr::ITrackedDeviceServerDriver* const m_shimmedDevice;
        const pvrEnvHandle m_pvr;
        const pvrSessionHandle m_pvrSession;

        vr::TrackedDeviceIndex_t m_deviceIndex = vr::k_unTrackedDeviceIndexInvalid;

        std::atomic<bool> m_active = false;
        std::thread m_updateThread;

        vr::VRInputComponentHandle_t m_eyeTrackingComponent = 0;
    };
} // namespace

namespace driver_shim {

    vr::ITrackedDeviceServerDriver* CreateHmdShimDriver(vr::ITrackedDeviceServerDriver* shimmedDriver,
                                                        pvrEnvHandle pvr,
                                                        pvrSessionHandle pvrSession) {
        try {
            return new HmdShimDriver(shimmedDriver, pvr, pvrSession);
        } catch (EyeTrackerNotSupportedException&) {
            return shimmedDriver;
        }
    }

} // namespace driver_shim
