#include "SheikzAmp/Audio/DeviceEnumerator.h"

#include <Windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace SheikzAmp::Audio {
namespace {

using Microsoft::WRL::ComPtr;

void check(HRESULT result, const char* action) {
    if (FAILED(result)) {
        throw std::runtime_error(action);
    }
}

class ComApartment final {
public:
    ComApartment() {
        const HRESULT result =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        if (FAILED(result)) {
            throw std::runtime_error(
                "Could not initialize Windows COM");
        }

        initialized_ = true;
    }

    ~ComApartment() {
        if (initialized_) {
            CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool initialized_{false};
};

std::vector<AudioDevice> devicesFor(
    EDataFlow flow,
    DeviceFlow publicFlow) {

    ComApartment com;

    ComPtr<IMMDeviceEnumerator> enumerator;

    check(
        CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator)),
        "Could not create Windows audio device enumerator");

    ComPtr<IMMDeviceCollection> collection;

    check(
        enumerator->EnumAudioEndpoints(
            flow,
            DEVICE_STATE_ACTIVE,
            &collection),
        "Could not list active Windows audio devices");

    std::wstring defaultId;

    ComPtr<IMMDevice> defaultDevice;

    if (SUCCEEDED(
            enumerator->GetDefaultAudioEndpoint(
                flow,
                eConsole,
                &defaultDevice))) {

        LPWSTR rawDefaultId = nullptr;

        if (SUCCEEDED(defaultDevice->GetId(&rawDefaultId))) {
            defaultId = rawDefaultId;
            CoTaskMemFree(rawDefaultId);
        }
    }

    UINT count = 0;

    check(
        collection->GetCount(&count),
        "Could not count Windows audio devices");

    std::vector<AudioDevice> result;
    result.reserve(count);

    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;

        check(
            collection->Item(index, &device),
            "Could not read Windows audio device");

        LPWSTR rawId = nullptr;

        check(
            device->GetId(&rawId),
            "Could not read Windows audio device id");

        std::wstring id = rawId;
        CoTaskMemFree(rawId);

        std::wstring name = L"Unnamed device";

        ComPtr<IPropertyStore> properties;

        if (SUCCEEDED(
                device->OpenPropertyStore(
                    STGM_READ,
                    &properties))) {

            PROPVARIANT value;
            PropVariantInit(&value);

            if (SUCCEEDED(
                    properties->GetValue(
                        PKEY_Device_FriendlyName,
                        &value))) {

                if (value.vt == VT_LPWSTR &&
                    value.pwszVal != nullptr) {

                    name = value.pwszVal;
                }
            }

            PropVariantClear(&value);
        }

        result.push_back(
            AudioDevice{
                std::move(id),
                std::move(name),
                publicFlow,
                result.empty()
                    ? false
                    : false
            });

        // Determine the default device by endpoint ID.
        result.back().isDefault =
            result.back().id == defaultId;
    }

    return result;
}

} // namespace

std::vector<AudioDevice>
DeviceEnumerator::playbackDevices() const {
    return devicesFor(
        eRender,
        DeviceFlow::Playback);
}

std::vector<AudioDevice>
DeviceEnumerator::recordingDevices() const {
    return devicesFor(
        eCapture,
        DeviceFlow::Recording);
}

} // namespace SheikzAmp::Audio