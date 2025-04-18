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

    std::unique_ptr<vr::IServerTrackedDeviceProvider> thisDriver;

    struct Driver : public vr::IServerTrackedDeviceProvider {
      public:
        Driver() {
        }

        virtual ~Driver() {
            Cleanup();
        };

        vr::EVRInitError Init(vr::IVRDriverContext* pDriverContext) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "Driver_Init");

            VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);

            // Detect whether we should attempt to shim the target driver.
            if (!m_isLoaded) {
                bool loadDriver = false;
                try {
                    pvrResult result = pvr_initialise(&m_pvr);
                    if (result != pvr_success) {
                        TraceLoggingWriteTagged(local, "Driver_Init_PvrInitError", TLArg((int)result, "Error"));
                        throw EyeTrackerNotSupportedException();
                    }

                    result = pvr_createSession(m_pvr, &m_pvrSession);
                    if (result != pvr_success) {
                        TraceLoggingWriteTagged(local, "Driver_Init_PvrCreateError", TLArg((int)result, "Error"));
                        throw EyeTrackerNotSupportedException();
                    }

                    pvrHmdInfo info{};
                    result = pvr_getHmdInfo(m_pvrSession, &info);
                    if (result != pvr_success) {
                        TraceLoggingWriteTagged(local, "Driver_Init_HmdInfoError", TLArg((int)result, "Error"));
                        throw EyeTrackerNotSupportedException();
                    }

                    // Look for a Pimax Crystal or Pimax Crystal Super.
                    if (!(info.VendorId == 0x34A4 && (info.ProductId == 0x0012 || info.ProductId == 0x0040))) {
                        TraceLoggingWriteTagged(local,
                                                "Driver_Init_HmdNotSupported",
                                                TLArg(info.VendorId, "VendorId"),
                                                TLArg(info.ProductId, "ProductId"));
                        DriverLog("Pimax Headset Product 0x%04x is not compatible", info.ProductId);
                        throw EyeTrackerNotSupportedException();
                    }

                    loadDriver = true;
                } catch (EyeTrackerNotSupportedException&) {
                }

                if (loadDriver) {
                    DriverLog("Installing IVRServerDriverHost::TrackedDeviceAdded hook");
                    InstallShimDriverHook(m_pvr, m_pvrSession);
                    m_isLoaded = true;
                }
            }

            TraceLoggingWriteStop(local, "Driver_Init");

            return m_isLoaded ? vr::VRInitError_None : vr::VRInitError_Init_HmdNotFound;
        }

        void Cleanup() override {
            VR_CLEANUP_SERVER_DRIVER_CONTEXT();

            pvr_destroySession(m_pvrSession);
            pvr_shutdown(m_pvr);
        }

        const char* const* GetInterfaceVersions() override {
            return vr::k_InterfaceVersions;
        }

        void RunFrame() override {};

        bool ShouldBlockStandbyMode() override {
            return false;
        }

        void EnterStandby() override {};

        void LeaveStandby() override {};

        bool m_isLoaded = false;
        pvrEnvHandle m_pvr = nullptr;
        pvrSessionHandle m_pvrSession = nullptr;
    };
} // namespace

// Entry point for vrserver.
extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* pInterfaceName, int* pReturnCode) {
    if (strcmp(vr::IServerTrackedDeviceProvider_Version, pInterfaceName) == 0) {
        if (!thisDriver) {
            thisDriver = std::make_unique<Driver>();
        }
        return thisDriver.get();
    }
    if (pReturnCode) {
        *pReturnCode = vr::VRInitError_Init_InterfaceNotFound;
    }
    return nullptr;
}
