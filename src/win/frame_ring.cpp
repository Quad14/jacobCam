// SPDX-License-Identifier: GPL-2.0-or-later
//
// Shared-memory frame ring, Windows implementation.

#include "qcam/ring.h"

#include <windows.h>

#include <sddl.h>

#include <cstring>
#include <iterator>
#include <string>

#include "qcam/log.h"
#include "qcam/win_guids.h"

namespace qcam {
namespace {

size_t SlotStride(uint32_t slot_bytes) {
    // Keep each slot on its own cache-line-aligned boundary so the writer's
    // stores do not falsely share with a reader's seqlock load.
    const size_t raw = sizeof(RingSlotHeader) + slot_bytes;
    return (raw + 63) & ~size_t{63};
}

uint8_t* SlotAt(uint8_t* base, uint32_t slot_bytes, uint32_t index) {
    return base + sizeof(RingHeader) + SlotStride(slot_bytes) * index;
}

// The virtual camera source is loaded by the Windows Frame Server, which runs
// as LOCAL SERVICE. A default DACL would grant access only to SYSTEM and to
// Administrators, so the Frame Server could not open the ring at all and the
// camera would enumerate but never produce a frame. These descriptors give
// the writer full control and every plausible consumer read access.
//
// qcamsvc itself also runs as LOCAL SERVICE, so granting that account write
// access would hand it to the Frame Server too. The writer is identified by
// its per-service SID instead (see WriterAce below).
//
// Sections: GENERIC_READ already implies SECTION_MAP_READ.
constexpr wchar_t kSectionSddl[] =
    L"D:P"
    L"(A;;GA;;;SY)"          // LocalSystem: full
    L"(A;;GA;;;BA)"          // Administrators: full
    L"(A;;GR;;;LS)"          // LOCAL SERVICE (Frame Server): read
    L"(A;;GR;;;NS)"          // NETWORK SERVICE: read
    L"(A;;GR;;;IU)";         // interactive users (qcamctl): read

// Events: GENERIC_READ does not include SYNCHRONIZE, so readers need it
// spelled out (0x00100000) or WaitForSingleObject fails with access denied.
constexpr wchar_t kEventSddl[] =
    L"D:P"
    L"(A;;0x1F0003;;;SY)"    // EVENT_ALL_ACCESS
    L"(A;;0x1F0003;;;BA)"
    L"(A;;0x00100002;;;LS)"  // SYNCHRONIZE | EVENT_MODIFY_STATE
    L"(A;;0x00100002;;;NS)"
    L"(A;;0x00100002;;;IU)";

// An ACE granting `rights` to the qcamsvc service SID, or an empty string when
// the service is not installed (console mode from an elevated prompt, where
// the Administrators ACE already covers the writer).
std::wstring WriterAce(const wchar_t* rights) {
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD sid_size = sizeof(sid);
    wchar_t domain[256];
    DWORD domain_size = static_cast<DWORD>(std::size(domain));
    SID_NAME_USE use;
    if (!::LookupAccountNameW(nullptr, QCAM_SERVICE_ACCOUNT, sid, &sid_size,
                              domain, &domain_size, &use)) {
        return std::wstring();
    }
    wchar_t* text = nullptr;
    if (!::ConvertSidToStringSidW(sid, &text)) return std::wstring();
    std::wstring ace = std::wstring(L"(A;;") + rights + L";;;" + text + L")";
    ::LocalFree(text);
    return ace;
}

// Owns the descriptor allocated by the SDDL converter.
class ScopedSecurityAttributes {
public:
    explicit ScopedSecurityAttributes(const wchar_t* sddl) {
        if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl, SDDL_REVISION_1, &descriptor_, nullptr)) {
            attributes_.nLength = sizeof(attributes_);
            attributes_.lpSecurityDescriptor = descriptor_;
            attributes_.bInheritHandle = FALSE;
            valid_ = true;
        } else {
            QCAM_LOGW("could not build security descriptor: %lu", ::GetLastError());
        }
    }
    ~ScopedSecurityAttributes() {
        if (descriptor_) ::LocalFree(descriptor_);
    }

    ScopedSecurityAttributes(const ScopedSecurityAttributes&) = delete;
    ScopedSecurityAttributes& operator=(const ScopedSecurityAttributes&) = delete;

    // Returns nullptr when the descriptor could not be built, which falls
    // back to the default DACL rather than failing outright.
    SECURITY_ATTRIBUTES* get() { return valid_ ? &attributes_ : nullptr; }

private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES  attributes_ = {};
    bool                 valid_ = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

struct FrameRingWriter::Impl {
    HANDLE      mapping = nullptr;
    HANDLE      event   = nullptr;
    uint8_t*    view    = nullptr;
    RingHeader* header  = nullptr;
    uint32_t    slot_bytes = 0;

    ~Impl() { Close(); }

    void Close() {
        if (header) header->writer_alive.store(0, std::memory_order_release);
        if (event) {
            ::SetEvent(event);   // wake readers so they notice we are gone
            ::CloseHandle(event);
            event = nullptr;
        }
        if (view) {
            ::UnmapViewOfFile(view);
            view = nullptr;
        }
        if (mapping) {
            ::CloseHandle(mapping);
            mapping = nullptr;
        }
        header = nullptr;
    }
};

FrameRingWriter::FrameRingWriter() : impl_(new Impl()) {}
FrameRingWriter::~FrameRingWriter() = default;

bool FrameRingWriter::IsOpen() const { return impl_ && impl_->header != nullptr; }

Status FrameRingWriter::Create(const RingConfig& config) {
    Close();

    const uint32_t slot_bytes = static_cast<uint32_t>(
        ImageSize(config.format, static_cast<uint16_t>(config.width),
                  static_cast<uint16_t>(config.height)));
    if (slot_bytes == 0) return Status::InvalidArg;

    const size_t total = sizeof(RingHeader) + SlotStride(slot_bytes) * kRingSlots;

    const std::wstring section_sddl = kSectionSddl + WriterAce(L"GA");
    ScopedSecurityAttributes section_sa(section_sddl.c_str());
    impl_->mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, section_sa.get(),
                                          PAGE_READWRITE, 0,
                                          static_cast<DWORD>(total),
                                          QCAM_RING_NAME);
    if (!impl_->mapping) {
        const DWORD err = ::GetLastError();
        QCAM_LOGE("CreateFileMapping failed: %lu", err);
        return (err == ERROR_ACCESS_DENIED) ? Status::Busy : Status::Io;
    }
    const bool already_existed = (::GetLastError() == ERROR_ALREADY_EXISTS);

    impl_->view = static_cast<uint8_t*>(
        ::MapViewOfFile(impl_->mapping, FILE_MAP_ALL_ACCESS, 0, 0, total));
    if (!impl_->view) {
        QCAM_LOGE("MapViewOfFile failed: %lu", ::GetLastError());
        impl_->Close();
        return Status::Io;
    }

    const std::wstring event_sddl = kEventSddl + WriterAce(L"0x1F0003");
    ScopedSecurityAttributes event_sa(event_sddl.c_str());
    // Manual reset, so a frame published while every reader was busy is not
    // missed by the next waiter.
    impl_->event = ::CreateEventW(event_sa.get(), /*manual=*/TRUE, FALSE,
                                  QCAM_RING_EVENT);
    if (!impl_->event) {
        QCAM_LOGE("CreateEvent failed: %lu", ::GetLastError());
        impl_->Close();
        return Status::Io;
    }

    if (already_existed) {
        // Another service instance is already publishing. Two writers would
        // interleave frames into the same slots.
        const auto* existing = reinterpret_cast<const RingHeader*>(impl_->view);
        if (existing->magic == kRingMagic &&
            existing->writer_alive.load(std::memory_order_acquire)) {
            QCAM_LOGE("another qcam writer already owns the frame ring");
            impl_->Close();
            return Status::Busy;
        }
    }

    std::memset(impl_->view, 0, total);
    impl_->header = reinterpret_cast<RingHeader*>(impl_->view);
    impl_->header->magic           = kRingMagic;
    impl_->header->version         = kRingVersion;
    impl_->header->slot_count      = kRingSlots;
    impl_->header->slot_bytes      = slot_bytes;
    impl_->header->width           = config.width;
    impl_->header->height          = config.height;
    impl_->header->format          = static_cast<uint32_t>(config.format);
    impl_->header->fps_numerator   = config.fps_numerator;
    impl_->header->fps_denominator = config.fps_denominator;
    impl_->header->write_sequence.store(0, std::memory_order_relaxed);
    impl_->header->writer_alive.store(1, std::memory_order_release);
    impl_->slot_bytes = slot_bytes;

    QCAM_LOGI("frame ring created: %ux%u %s, %u slots of %u bytes", config.width,
              config.height, PixelFormatName(config.format), kRingSlots,
              slot_bytes);
    return Status::Ok;
}

void FrameRingWriter::Close() {
    if (impl_) impl_->Close();
}

Status FrameRingWriter::Publish(const uint8_t* data, size_t size,
                                uint64_t sequence, uint64_t timestamp_100ns) {
    if (!IsOpen()) return Status::NoDevice;
    if (!data) return Status::InvalidArg;
    if (size > impl_->slot_bytes) return Status::InvalidArg;

    const uint64_t published = impl_->header->write_sequence.load(std::memory_order_relaxed);
    const uint32_t index = static_cast<uint32_t>(published % kRingSlots);

    auto* slot = reinterpret_cast<RingSlotHeader*>(
        SlotAt(impl_->view, impl_->slot_bytes, index));
    uint8_t* payload = reinterpret_cast<uint8_t*>(slot) + sizeof(RingSlotHeader);

    // Seqlock write: odd version means "in flux, do not trust the payload".
    const uint32_t v = slot->version.load(std::memory_order_relaxed);
    slot->version.store(v + 1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);

    slot->size            = static_cast<uint32_t>(size);
    slot->sequence        = sequence;
    slot->timestamp_100ns = timestamp_100ns;
    std::memcpy(payload, data, size);

    std::atomic_thread_fence(std::memory_order_release);
    slot->version.store(v + 2, std::memory_order_release);

    impl_->header->write_sequence.store(published + 1, std::memory_order_release);
    // Manual-reset: every waiting reader wakes, and a reader that arrives
    // slightly late still sees the signal. Readers reset it themselves once
    // they have observed a newer write_sequence.
    ::SetEvent(impl_->event);
    return Status::Ok;
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

struct FrameRingReader::Impl {
    HANDLE            mapping = nullptr;
    HANDLE            event   = nullptr;
    uint8_t*          view    = nullptr;
    const RingHeader* header  = nullptr;
    uint32_t          slot_bytes = 0;
    uint64_t          last_sequence = 0;

    ~Impl() { Close(); }

    void Close() {
        if (event)   { ::CloseHandle(event);   event = nullptr; }
        if (view)    { ::UnmapViewOfFile(view); view = nullptr; }
        if (mapping) { ::CloseHandle(mapping); mapping = nullptr; }
        header = nullptr;
    }
};

FrameRingReader::FrameRingReader() : impl_(new Impl()) {}
FrameRingReader::~FrameRingReader() = default;

bool FrameRingReader::IsOpen() const { return impl_ && impl_->header != nullptr; }

Status FrameRingReader::Open() {
    Close();

    impl_->mapping = ::OpenFileMappingW(FILE_MAP_READ, FALSE, QCAM_RING_NAME);
    if (!impl_->mapping) {
        // The service is not running, which is the common case rather than an
        // error: the virtual camera just has nothing to show yet.
        QCAM_LOGD("frame ring not present (is qcamsvc running?)");
        return Status::NoDevice;
    }

    // Map the header first so the real size can be worked out. RingHeader
    // holds std::atomic members and so is not copyable; read the plain fields
    // out individually rather than taking a struct copy.
    auto* probe = static_cast<uint8_t*>(
        ::MapViewOfFile(impl_->mapping, FILE_MAP_READ, 0, 0, sizeof(RingHeader)));
    if (!probe) {
        impl_->Close();
        return Status::Io;
    }
    const auto* probe_header = reinterpret_cast<const RingHeader*>(probe);
    const uint32_t magic      = probe_header->magic;
    const uint32_t version    = probe_header->version;
    const uint32_t slot_bytes = probe_header->slot_bytes;
    const uint32_t slot_count = probe_header->slot_count;
    ::UnmapViewOfFile(probe);

    if (magic != kRingMagic) {
        QCAM_LOGE("frame ring has bad magic 0x%08x", magic);
        impl_->Close();
        return Status::Protocol;
    }
    if (version != kRingVersion) {
        QCAM_LOGE("frame ring version %u, expected %u", version, kRingVersion);
        impl_->Close();
        return Status::Unsupported;
    }
    if (slot_bytes == 0 || slot_count == 0 || slot_count > 64) {
        QCAM_LOGE("frame ring geometry is implausible (%u slots of %u bytes)",
                  slot_count, slot_bytes);
        impl_->Close();
        return Status::Protocol;
    }

    const size_t total = sizeof(RingHeader) + SlotStride(slot_bytes) * slot_count;
    impl_->view = static_cast<uint8_t*>(
        ::MapViewOfFile(impl_->mapping, FILE_MAP_READ, 0, 0, total));
    if (!impl_->view) {
        impl_->Close();
        return Status::Io;
    }

    // SYNCHRONIZE to wait, EVENT_MODIFY_STATE to reset before waiting.
    impl_->event = ::OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE,
                                QCAM_RING_EVENT);
    if (!impl_->event) {
        impl_->Close();
        return Status::Io;
    }

    impl_->header     = reinterpret_cast<const RingHeader*>(impl_->view);
    impl_->slot_bytes = slot_bytes;
    // Start from whatever is current rather than replaying stale frames.
    impl_->last_sequence = impl_->header->write_sequence.load(std::memory_order_acquire);
    return Status::Ok;
}

void FrameRingReader::Close() {
    if (impl_) impl_->Close();
}

Status FrameRingReader::GetConfig(RingConfig* config) const {
    if (!config) return Status::InvalidArg;
    if (!IsOpen()) return Status::NoDevice;

    config->width           = impl_->header->width;
    config->height          = impl_->header->height;
    config->format          = static_cast<PixelFormat>(impl_->header->format);
    config->fps_numerator   = impl_->header->fps_numerator;
    config->fps_denominator = impl_->header->fps_denominator;
    return Status::Ok;
}

Status FrameRingReader::Read(std::vector<uint8_t>* out, FrameMeta* meta,
                             uint32_t timeout_ms) {
    if (!out || !meta) return Status::InvalidArg;
    if (!IsOpen()) return Status::NoDevice;

    const DWORD deadline = ::GetTickCount() + timeout_ms;
    for (;;) {
        if (!impl_->header->writer_alive.load(std::memory_order_acquire))
            return Status::NoDevice;

        const uint64_t published =
            impl_->header->write_sequence.load(std::memory_order_acquire);

        if (published > impl_->last_sequence) {
            // Always take the newest frame. Falling behind on a live capture
            // is better served by skipping than by draining a backlog.
            const uint64_t take = published - 1;
            const uint32_t index =
                static_cast<uint32_t>(take % impl_->header->slot_count);
            const auto* slot = reinterpret_cast<const RingSlotHeader*>(
                SlotAt(impl_->view, impl_->slot_bytes, index));
            const uint8_t* payload =
                reinterpret_cast<const uint8_t*>(slot) + sizeof(RingSlotHeader);

            // Seqlock read: even version before and the same version after
            // means nothing was rewritten underneath us.
            const uint32_t before = slot->version.load(std::memory_order_acquire);
            if (before & 1u) {
                // The writer has lapped us and is rewriting this slot. Its
                // write_sequence will have moved on, so the retry picks a
                // newer frame rather than spinning here.
                ::YieldProcessor();
                continue;
            }

            const uint32_t size = slot->size;
            if (size > impl_->slot_bytes) continue;

            FrameMeta candidate;
            candidate.sequence        = slot->sequence;
            candidate.timestamp_100ns = slot->timestamp_100ns;
            candidate.width           = impl_->header->width;
            candidate.height          = impl_->header->height;
            candidate.format          = static_cast<PixelFormat>(impl_->header->format);

            out->resize(size);
            std::memcpy(out->data(), payload, size);

            std::atomic_thread_fence(std::memory_order_acquire);
            if (slot->version.load(std::memory_order_acquire) != before)
                continue;   // torn; the writer lapped us, retry

            *meta = candidate;
            impl_->last_sequence = published;
            return Status::Ok;
        }

        const DWORD now = ::GetTickCount();
        // GetTickCount wraps every 49 days; comparing the difference as a
        // signed value handles the wrap correctly.
        const LONG remaining = static_cast<LONG>(deadline - now);
        if (remaining <= 0) return Status::Timeout;

        // Reset before re-checking the sequence: if the writer publishes
        // between the check and the wait, it re-signals and we do not sleep.
        ::ResetEvent(impl_->event);
        if (impl_->header->write_sequence.load(std::memory_order_acquire) >
            impl_->last_sequence) {
            continue;
        }

        const DWORD wait = ::WaitForSingleObject(impl_->event,
                                                 static_cast<DWORD>(remaining));
        if (wait == WAIT_TIMEOUT) return Status::Timeout;
        if (wait != WAIT_OBJECT_0) return Status::Io;
    }
}

}  // namespace qcam
