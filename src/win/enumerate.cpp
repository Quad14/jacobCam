// SPDX-License-Identifier: GPL-2.0-or-later
//
// Device discovery on Windows.
//
// Devices show up here only once qcamusb.inf has bound them to WinUSB and
// they have published the project's device interface GUID. A camera that is
// plugged in but still owned by some other driver (or by nothing at all) will
// not be listed; qcamctl reports that case separately so the difference
// between "not plugged in" and "not bound" is visible.

#include <windows.h>

#include <cfgmgr32.h>
#include <setupapi.h>

#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <cstdlib>
#include <string>
#include <vector>

#include "qcam/log.h"
#include "qcam/usb.h"
#include "qcam/win_guids.h"

namespace qcam {

namespace {

std::string Narrow(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                             static_cast<int>(wide.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string out(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                          &out[0], needed, nullptr, nullptr);
    return out;
}

std::wstring ToUpper(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(::towupper(c));
    return s;
}

// Device interface paths look like
//   \\?\usb#vid_046d&pid_0840#5&1b2c3d4e&0&2#{b17cb711-...}
// so the identifiers can be read straight out of the path without opening
// the device first.
bool ParseVidPid(const std::wstring& path, uint16_t* vid, uint16_t* pid) {
    const std::wstring upper = ToUpper(path);
    const size_t vpos = upper.find(L"VID_");
    const size_t ppos = upper.find(L"PID_");
    if (vpos == std::wstring::npos || ppos == std::wstring::npos) return false;
    if (vpos + 8 > upper.size() || ppos + 8 > upper.size()) return false;

    *vid = static_cast<uint16_t>(std::wcstoul(upper.substr(vpos + 4, 4).c_str(),
                                              nullptr, 16));
    *pid = static_cast<uint16_t>(std::wcstoul(upper.substr(ppos + 4, 4).c_str(),
                                              nullptr, 16));
    return true;
}

std::string ReadFriendlyName(HDEVINFO set, SP_DEVINFO_DATA* info) {
    const DWORD props[] = {SPDRP_FRIENDLYNAME, SPDRP_DEVICEDESC};
    for (DWORD prop : props) {
        wchar_t buffer[512] = {};
        if (::SetupDiGetDeviceRegistryPropertyW(set, info, prop, nullptr,
                                                reinterpret_cast<PBYTE>(buffer),
                                                sizeof(buffer), nullptr)) {
            return Narrow(buffer);
        }
    }
    return std::string();
}

}  // namespace

Status EnumerateDevices(std::vector<UsbDeviceInfo>* out) {
    if (!out) return Status::InvalidArg;
    out->clear();

    HDEVINFO set = ::SetupDiGetClassDevsW(&GUID_DEVINTERFACE_QCAM, nullptr, nullptr,
                                          DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        QCAM_LOGE("SetupDiGetClassDevs failed: %lu", ::GetLastError());
        return Status::Io;
    }

    SP_DEVICE_INTERFACE_DATA iface = {};
    iface.cbSize = sizeof(iface);

    for (DWORD index = 0;
         ::SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_QCAM,
                                       index, &iface);
         ++index) {
        DWORD needed = 0;
        ::SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &needed, nullptr);
        if (needed == 0) continue;

        std::vector<uint8_t> buffer(needed);
        auto* detail =
            reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        SP_DEVINFO_DATA devinfo = {};
        devinfo.cbSize = sizeof(devinfo);

        if (!::SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, needed, nullptr,
                                                &devinfo)) {
            QCAM_LOGW("SetupDiGetDeviceInterfaceDetail failed: %lu", ::GetLastError());
            continue;
        }

        UsbDeviceInfo info;
        info.device_path = Narrow(detail->DevicePath);
        if (!ParseVidPid(detail->DevicePath, &info.vid, &info.pid)) {
            QCAM_LOGW("could not parse VID/PID out of %s", info.device_path.c_str());
            continue;
        }

        const DeviceId* known = LookupDevice(info.vid, info.pid);
        if (!known) {
            // Something else published our interface GUID. Not ours to drive.
            QCAM_LOGD("ignoring unrecognised device %04x:%04x", info.vid, info.pid);
            continue;
        }

        info.friendly_name = ReadFriendlyName(set, &devinfo);
        if (info.friendly_name.empty()) info.friendly_name = known->marketing_name;

        QCAM_LOGD("found %s at %s", info.friendly_name.c_str(),
                  info.device_path.c_str());
        out->push_back(std::move(info));
    }

    ::SetupDiDestroyDeviceInfoList(set);
    return Status::Ok;
}

}  // namespace qcam
