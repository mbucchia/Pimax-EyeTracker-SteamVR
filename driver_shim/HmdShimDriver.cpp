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

namespace vr {
    struct VREyeTrackingData_t {
        uint16_t flag1;
        uint8_t flag2;
        DirectX::XMVECTOR vector;
    };
    struct IVRDriverInputInternal_XXX {
        virtual void dummy01() = 0;
        virtual void dummy02() = 0;
        virtual vr::EVRInputError CreateEyeTrackingComponent(vr::PropertyContainerHandle_t ulContainer,
                                                             const char* pchName,
                                                             vr::VRInputComponentHandle_t* pHandle) = 0;
        virtual vr::EVRInputError UpdateEyeTrackingComponent(vr::VRInputComponentHandle_t ulComponent,
                                                             VREyeTrackingData_t* data) = 0;
    };
} // namespace vr

namespace {
    using namespace driver_shim;

    // The HmdShimDriver driver wraps another ITrackedDeviceServerDriver instance with the intent to override
    // properties and behaviors.
    struct HmdShimDriver : public vr::ITrackedDeviceServerDriver {
        HmdShimDriver(vr::ITrackedDeviceServerDriver* shimmedDevice, pvrEnvHandle pvr, pvrSessionHandle pvrSession)
            : m_shimmedDevice(shimmedDevice), m_pvr(pvr), m_pvrSession(pvrSession) {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdShimDriver_Ctor");
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

            // Advertise that we will pass eye tracking data. This is an undocumented property.
            vr::VRProperties()->SetBoolProperty(container, (vr::ETrackedDeviceProperty)6009, true);

            // Get the internal interface.
            vr::EVRInitError eError;
            IVRDriverInputInternal_XXX = (vr::IVRDriverInputInternal_XXX*)vr::VRDriverContext()->GetGenericInterface(
                "IVRDriverInputInternal_XXX", &eError);

            // Create the eye tracking component.
            IVRDriverInputInternal_XXX->CreateEyeTrackingComponent(container, "/eyetracking", &m_eyeTrackingComponent);

            // Schedule updates in a background thread.
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
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));

                    TraceLoggingWriteStop(sleep, "HmdShimDriver_UpdateThread_Sleep", TLArg(m_active.load(), "Active"));

                    if (!m_active) {
                        break;
                    }
                }

                pvrEyeTrackingInfo state{};
                pvrResult result = pvr_getEyeTrackingInfo(m_pvrSession, pvr_getTimeSeconds(m_pvr), &state);
                TraceLoggingWriteTagged(local,
                                        "HmdShimDriver_PvrEyeTrackingInfo",
                                        TLArg((int)result, "Result"),
                                        TLArg(state.TimeInSeconds, "TimeInSeconds"));

                const bool isEyeTrackingDataAvailable = result == pvr_success && state.TimeInSeconds > 0;
                if (isEyeTrackingDataAvailable) {
                    // Not entirely sure what each bit means, but overall this means there is data to pass.
                    data.flag1 = 0x101;
                    data.flag2 = 0x1;

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
                    data.vector =
                        DirectX::XMVector3Normalize(DirectX::XMVectorSet(sinf(angleHorizontal) * cosf(angleVertical),
                                                                         sinf(angleVertical),
                                                                         -cosf(angleHorizontal) * cosf(angleVertical),
                                                                         1));
                } else {
                    data.flag1 = 0;
                    data.flag2 = 0;
                    data.vector = DirectX::XMVectorSet(0, 0, -1, 1);
                }
                IVRDriverInputInternal_XXX->UpdateEyeTrackingComponent(m_eyeTrackingComponent, &data);
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
        vr::IVRDriverInputInternal_XXX* IVRDriverInputInternal_XXX = nullptr;
    };
} // namespace

namespace driver_shim {

    vr::ITrackedDeviceServerDriver* CreateHmdShimDriver(vr::ITrackedDeviceServerDriver* shimmedDriver,
                                                        pvrEnvHandle pvr,
                                                        pvrSessionHandle pvrSession) {
        return new HmdShimDriver(shimmedDriver, pvr, pvrSession);
    }

} // namespace driver_shim
