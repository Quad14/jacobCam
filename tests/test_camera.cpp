// SPDX-License-Identifier: GPL-2.0-or-later
//
// End-to-end tests through the Camera facade with a mock transport: open,
// probe, init, stream synthetic packets, receive decoded frames.

#include "test_harness.h"

#include <vector>

#include "../src/core/sensor_hdcs.h"

#include "qcam/device.h"
#include "qcam/mock.h"

using namespace qcam;

namespace {

// Returns an already-scripted mock that will pass sensor probing.
std::unique_ptr<MockTransport> MakeQuickCamExpress() {
    std::unique_ptr<MockTransport> t(new MockTransport());
    UsbDeviceInfo info;
    info.vid = kVendorLogitech;
    info.pid = 0x0840;              // QuickCam Express, P/N 861037-0000
    info.device_path = "mock://qcam";
    info.friendly_name = "Logitech QuickCam Express";
    info.bus_speed = 2;
    t->SetInfo(info);
    t->SetSensorRegister(hdcs::kIdent, hdcs::kIdent1x00);
    return t;
}

CameraConfig TestConfig() {
    CameraConfig cfg;
    cfg.settle_ms = 0;              // no sleeping in tests
    cfg.format = PixelFormat::Nv12;
    cfg.color.auto_white_balance = false;
    return cfg;
}

}  // namespace

TEST(CameraOpensAndProbesTheSensor) {
    auto transport = MakeQuickCamExpress();
    Camera camera;

    CHECK_OK(camera.Open(std::move(transport), TestConfig()));
    CHECK(camera.IsOpen());
    CHECK_EQ(std::string(camera.sensor_name()),
             std::string("Agilent HDCS-1000/1100"));
    CHECK_EQ(int{camera.sensor_geometry().width}, 360);
    CHECK_EQ(int{camera.sensor_geometry().height}, 296);
    CHECK_NEAR(camera.nominal_fps(), 7.9, 0.3);
}

TEST(CameraRejectsAnUnknownUsbDevice) {
    std::unique_ptr<MockTransport> t(new MockTransport());
    UsbDeviceInfo info;
    info.vid = 0x1234;
    info.pid = 0x5678;
    t->SetInfo(info);

    Camera camera;
    CHECK_STATUS(camera.Open(std::move(t), TestConfig()), Status::Unsupported);
    CHECK(!camera.IsOpen());
}

TEST(CameraReportsSensorProbeFailure) {
    std::unique_ptr<MockTransport> t(new MockTransport());
    UsbDeviceInfo info;
    info.vid = kVendorLogitech;
    info.pid = 0x0840;
    t->SetInfo(info);
    t->SetSensorRegister(hdcs::kIdent, 0x99);   // nothing we support

    Camera camera;
    CHECK_STATUS(camera.Open(std::move(t), TestConfig()), Status::NoDevice);
    CHECK(!camera.IsOpen());
}

TEST(CameraStartEnablesIsochronousStreaming) {
    auto transport = MakeQuickCamExpress();
    MockTransport* mock = transport.get();
    Camera camera;
    CHECK_OK(camera.Open(std::move(transport), TestConfig()));

    CHECK_OK(camera.Start([](const DecodedFrame&) {}));
    CHECK(camera.IsStreaming());

    // Alt setting 1 reserves the isochronous bandwidth.
    CHECK_EQ(int{mock->alt_setting()}, int{kAltStreaming});

    const auto* iso = mock->LastWriteTo(reg::kIsoEnable);
    CHECK(iso != nullptr);
    if (iso) CHECK_EQ(int{iso->data[0]}, 1);

    const auto* size = mock->LastWriteTo(reg::kIsoSizeL);
    CHECK(size != nullptr);
    if (size) {
        CHECK_EQ(size->data.size(), size_t{2});
        CHECK_EQ(int{size->data[0]} | (int{size->data[1]} << 8), 847);
    }

    CHECK_OK(camera.Stop());
    CHECK(!camera.IsStreaming());
    CHECK_EQ(int{mock->alt_setting()}, int{kAltIdle});
}

TEST(CameraHonoursANegotiatedDownPacketSize) {
    auto transport = MakeQuickCamExpress();
    MockTransport* mock = transport.get();
    // The host controller could not give us the full allocation.
    mock->SetIsoMaxPacketSize(600);

    Camera camera;
    CHECK_OK(camera.Open(std::move(transport), TestConfig()));
    CHECK_OK(camera.Start([](const DecodedFrame&) {}));

    // Telling the ASIC the wrong packet size makes every packet over-run.
    const auto* size = mock->LastWriteTo(reg::kIsoSizeL);
    CHECK(size != nullptr);
    if (size) CHECK_EQ(int{size->data[0]} | (int{size->data[1]} << 8), 600);
    CHECK_OK(camera.Stop());
}

TEST(CameraDeliversDecodedFrames) {
    auto transport = MakeQuickCamExpress();
    MockTransport* mock = transport.get();
    Camera camera;
    CHECK_OK(camera.Open(std::move(transport), TestConfig()));

    int frames = 0;
    uint16_t width = 0, height = 0;
    size_t size = 0;
    PixelFormat format = PixelFormat::Bayer8;
    CHECK_OK(camera.Start([&](const DecodedFrame& f) {
        frames++;
        width = f.width;
        height = f.height;
        size = f.size;
        format = f.format;
    }));

    // Feed one synthetic 360x296 frame through the chunk layer.
    const std::vector<uint8_t> raw(360 * 296, 100);
    for (auto& packet : BuildFramePackets(raw, 843))
        mock->QueueIsoPacket(std::move(packet));
    mock->PumpIso();

    CHECK_EQ(frames, 1);
    CHECK_EQ(int{width}, 360);
    CHECK_EQ(int{height}, 296);
    CHECK(format == PixelFormat::Nv12);
    CHECK_EQ(size, ImageSize(PixelFormat::Nv12, 360, 296));

    const CameraStats stats = camera.stats();
    CHECK_EQ(stats.frames_delivered, uint64_t{1});
    CHECK_EQ(stats.framer.frames_complete, uint64_t{1});

    CHECK_OK(camera.Stop());
}

TEST(CameraAppliesConfiguredOutputSize) {
    auto transport = MakeQuickCamExpress();
    MockTransport* mock = transport.get();

    CameraConfig cfg = TestConfig();
    cfg.out_width = 352;      // CIF, centre-cropped from the native 360x296
    cfg.out_height = 288;

    Camera camera;
    CHECK_OK(camera.Open(std::move(transport), cfg));
    CHECK_EQ(int{camera.out_width()}, 352);
    CHECK_EQ(int{camera.out_height()}, 288);

    size_t size = 0;
    CHECK_OK(camera.Start([&](const DecodedFrame& f) { size = f.size; }));
    const std::vector<uint8_t> raw(360 * 296, 64);
    for (auto& p : BuildFramePackets(raw, 843)) mock->QueueIsoPacket(std::move(p));
    mock->PumpIso();

    CHECK_EQ(size, ImageSize(PixelFormat::Nv12, 352, 288));
    CHECK_OK(camera.Stop());
}

TEST(CameraAutoExposureDrivesTheSensor) {
    auto transport = MakeQuickCamExpress();
    MockTransport* mock = transport.get();

    CameraConfig cfg = TestConfig();
    cfg.auto_exposure.enabled = true;
    cfg.auto_exposure.settle_frames = 0;

    Camera camera;
    CHECK_OK(camera.Open(std::move(transport), cfg));
    CHECK_OK(camera.Start([](const DecodedFrame&) {}));
    mock->ClearLog();

    // A very dark frame must make the controller raise the exposure.
    const std::vector<uint8_t> dark(360 * 296, 4);
    for (auto& p : BuildFramePackets(dark, 843)) mock->QueueIsoPacket(std::move(p));
    mock->PumpIso();

    CHECK(camera.stats().exposure > kDefExposure);
    // ... and that has to reach the hardware, not just the controller state.
    CHECK(mock->CountWritesTo(reg::kI2cWriteWindow) > 0);
    CHECK_OK(camera.Stop());
}

TEST(CameraManualControlsDisableAutoExposure) {
    auto transport = MakeQuickCamExpress();
    Camera camera;
    CHECK_OK(camera.Open(std::move(transport), TestConfig()));

    CHECK(camera.auto_exposure());
    CHECK_OK(camera.SetExposure(123));
    CHECK_EQ(camera.auto_exposure(), false);
    // A manual write should show in the diagnostics readout straight away,
    // not only once the next frame arrives.
    CHECK_EQ(camera.stats().exposure, 123);

    CHECK_OK(camera.SetAutoExposure(true));
    CHECK(camera.auto_exposure());
}

TEST(CameraColourSettingsReachTheDecoder) {
    auto transport = MakeQuickCamExpress();
    MockTransport* mock = transport.get();
    Camera camera;
    CHECK_OK(camera.Open(std::move(transport), TestConfig()));

    ColorSettings c = camera.color_settings();
    c.flip_vertical = true;
    c.gamma = 1.0f;
    camera.SetColorSettings(c);
    CHECK_EQ(camera.color_settings().flip_vertical, true);

    // The staged settings are picked up at the next frame boundary.
    CHECK_OK(camera.Start([](const DecodedFrame&) {}));
    const std::vector<uint8_t> raw(360 * 296, 100);
    for (auto& p : BuildFramePackets(raw, 843)) mock->QueueIsoPacket(std::move(p));
    mock->PumpIso();
    CHECK_OK(camera.Stop());
}

TEST(CameraStopIsIdempotentAndCloseIsSafe) {
    auto transport = MakeQuickCamExpress();
    Camera camera;
    CHECK_OK(camera.Open(std::move(transport), TestConfig()));
    CHECK_OK(camera.Start([](const DecodedFrame&) {}));

    CHECK_OK(camera.Stop());
    CHECK_OK(camera.Stop());
    camera.Close();
    camera.Close();
    CHECK(!camera.IsOpen());
}

TEST(CameraRefusesASecondOpen) {
    auto first = MakeQuickCamExpress();
    auto second = MakeQuickCamExpress();
    Camera camera;

    CHECK_OK(camera.Open(std::move(first), TestConfig()));
    CHECK_STATUS(camera.Open(std::move(second), TestConfig()), Status::Busy);
}

TEST(CameraStartRequiresAnOpenDevice) {
    Camera camera;
    CHECK_STATUS(camera.Start([](const DecodedFrame&) {}), Status::NoDevice);
}

TEST(KnownDeviceTableCoversTheTargetCamera) {
    // P/N 861037-0000 / M/N V-UB2 enumerates as 046d:0840.
    const DeviceId* id = LookupDevice(0x046d, 0x0840);
    CHECK(id != nullptr);
    if (!id) return;
    CHECK(id->bridge == Bridge::Stv0600);
    CHECK(LookupDevice(0x046d, 0x9999) == nullptr);
}
