// SPDX-License-Identifier: GPL-2.0-or-later
#include "qcamvcam.h"

#include <mferror.h>

#include <algorithm>
#include <cstring>

#include "qcam/log.h"

namespace qcam {
namespace vcam {

QcamMediaStream::QcamMediaStream() {
    ++g_object_count;
    work_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

QcamMediaStream::~QcamMediaStream() {
    Shutdown();
    if (work_event_) ::CloseHandle(work_event_);
    --g_object_count;
}

HRESULT QcamMediaStream::Initialize(QcamMediaSource* source,
                                    IMFStreamDescriptor* descriptor, UINT32 width,
                                    UINT32 height, UINT32 fps_num, UINT32 fps_den) {
    if (!source || !descriptor) return E_INVALIDARG;
    if (!work_event_) return E_FAIL;

    std::lock_guard<std::mutex> lock(mutex_);
    source_     = source;
    descriptor_ = descriptor;
    width_      = width;
    height_     = height;
    fps_num_    = fps_num ? fps_num : kDefaultFpsNum;
    fps_den_    = fps_den ? fps_den : kDefaultFpsDen;

    frame_bytes_ = ImageSize(PixelFormat::Nv12, static_cast<uint16_t>(width_),
                             static_cast<uint16_t>(height_));
    // One second in 100 ns units, divided by the frame rate.
    frame_duration_100ns_ =
        static_cast<LONGLONG>(10'000'000.0 * fps_den_ / fps_num_);

    return MFCreateEventQueue(&event_queue_);
}

HRESULT QcamMediaStream::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return S_OK;
        shutdown_ = true;
        state_ = MF_STREAM_STATE_STOPPED;
    }

    worker_stop_.store(true);
    if (work_event_) ::SetEvent(work_event_);
    if (worker_.joinable()) worker_.join();

    std::lock_guard<std::mutex> lock(mutex_);
    pending_.clear();
    if (event_queue_) {
        event_queue_->Shutdown();
        event_queue_.Reset();
    }
    ring_.Close();
    ring_open_ = false;
    descriptor_.Reset();
    source_ = nullptr;
    return S_OK;
}

// --- IUnknown --------------------------------------------------------------

IFACEMETHODIMP QcamMediaStream::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator ||
        riid == IID_IMFMediaStream || riid == IID_IMFMediaStream2) {
        *ppv = static_cast<IMFMediaStream2*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) QcamMediaStream::AddRef() {
    return ++ref_count_;
}

IFACEMETHODIMP_(ULONG) QcamMediaStream::Release() {
    const ULONG count = --ref_count_;
    if (count == 0) delete this;
    return count;
}

// --- IMFMediaEventGenerator ------------------------------------------------

IFACEMETHODIMP QcamMediaStream::BeginGetEvent(IMFAsyncCallback* callback,
                                              IUnknown* state) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        queue = event_queue_;
    }
    return queue ? queue->BeginGetEvent(callback, state) : MF_E_SHUTDOWN;
}

IFACEMETHODIMP QcamMediaStream::EndGetEvent(IMFAsyncResult* result,
                                            IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        queue = event_queue_;
    }
    return queue ? queue->EndGetEvent(result, event) : MF_E_SHUTDOWN;
}

IFACEMETHODIMP QcamMediaStream::GetEvent(DWORD flags, IMFMediaEvent** event) {
    // GetEvent can block, so the queue reference is taken under the lock and
    // the call itself is made without it held.
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        queue = event_queue_;
    }
    return queue ? queue->GetEvent(flags, event) : MF_E_SHUTDOWN;
}

IFACEMETHODIMP QcamMediaStream::QueueEvent(MediaEventType type,
                                           REFGUID extended_type, HRESULT status,
                                           const PROPVARIANT* value) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        queue = event_queue_;
    }
    return queue ? queue->QueueEventParamVar(type, extended_type, status, value)
                 : MF_E_SHUTDOWN;
}

// --- IMFMediaStream --------------------------------------------------------

IFACEMETHODIMP QcamMediaStream::GetMediaSource(IMFMediaSource** source) {
    if (!source) return E_POINTER;

    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_ || !source_) return MF_E_SHUTDOWN;
    return source_->QueryInterface(IID_PPV_ARGS(source));
}

IFACEMETHODIMP QcamMediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;

    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (!descriptor_) return E_UNEXPECTED;

    *descriptor = descriptor_.Get();
    (*descriptor)->AddRef();
    return S_OK;
}

IFACEMETHODIMP QcamMediaStream::RequestSample(IUnknown* token) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;
        pending_.emplace_back(token);
    }
    // Hand the work to the worker thread: RequestSample must return promptly,
    // and pulling a frame can block for up to a frame interval.
    ::SetEvent(work_event_);
    return S_OK;
}

// --- IMFMediaStream2 -------------------------------------------------------

IFACEMETHODIMP QcamMediaStream::SetStreamState(MF_STREAM_STATE state) {
    switch (state) {
        case MF_STREAM_STATE_RUNNING: return Start();
        case MF_STREAM_STATE_PAUSED:  return Pause();
        case MF_STREAM_STATE_STOPPED: return StopStream();
        default:                      return E_INVALIDARG;
    }
}

IFACEMETHODIMP QcamMediaStream::GetStreamState(MF_STREAM_STATE* state) {
    if (!state) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    *state = state_;
    return S_OK;
}

// --- Streaming -------------------------------------------------------------

HRESULT QcamMediaStream::Start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (state_ == MF_STREAM_STATE_RUNNING) return S_OK;
        state_ = MF_STREAM_STATE_RUNNING;
        next_timestamp_ = 0;

        if (!ring_open_) {
            // The service may not be running yet. That is not fatal: the
            // stream produces blank frames until it appears, which keeps
            // conferencing apps from erroring out at open time.
            ring_open_ = Succeeded(ring_.Open());
            if (!ring_open_)
                QCAM_LOGW("frame ring unavailable; emitting blank frames");
        }

        if (!worker_.joinable()) {
            worker_stop_.store(false);
            worker_ = std::thread([this] { WorkerLoop(); });
        }
    }
    ::SetEvent(work_event_);
    return QueueEvent(MEStreamStarted, GUID_NULL, S_OK, nullptr);
}

HRESULT QcamMediaStream::Pause() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        state_ = MF_STREAM_STATE_PAUSED;
    }
    return QueueEvent(MEStreamPaused, GUID_NULL, S_OK, nullptr);
}

HRESULT QcamMediaStream::StopStream() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        state_ = MF_STREAM_STATE_STOPPED;
        pending_.clear();
    }
    return QueueEvent(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

HRESULT QcamMediaStream::SetRate(float) {
    // The camera runs at one fixed rate; there is nothing to vary.
    return S_OK;
}

void QcamMediaStream::WorkerLoop() {
    while (!worker_stop_.load()) {
        ComPtr<IUnknown> token;
        bool have_request = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!shutdown_ && state_ == MF_STREAM_STATE_RUNNING && !pending_.empty()) {
                token = pending_.front();
                pending_.erase(pending_.begin());
                have_request = true;
            }
        }

        if (!have_request) {
            ::WaitForSingleObject(work_event_, 100);
            continue;
        }

        const HRESULT hr = DeliverSample(token.Get());
        if (FAILED(hr) && hr != MF_E_SHUTDOWN) {
            QCAM_LOGW("delivering a sample failed: 0x%08lx",
                      static_cast<unsigned long>(hr));
            QueueEvent(MEError, GUID_NULL, hr, nullptr);
        }
    }
}

HRESULT QcamMediaStream::DeliverSample(IUnknown* token) {
    ComPtr<IMFSample> sample;
    HRESULT hr = CreateSampleFromRing(&sample);
    if (hr == MF_E_SHUTDOWN) return hr;
    if (FAILED(hr) || !sample) {
        // No frame available. Send a blank one so the consumer's pipeline
        // keeps ticking rather than stalling on a missing sample.
        hr = CreateBlankSample(&sample);
        if (FAILED(hr)) return hr;
    }

    if (token) {
        // The Frame Server matches the token it handed to RequestSample
        // against the one that comes back on the sample.
        hr = sample->SetUnknown(MFSampleExtension_Token, token);
        if (FAILED(hr)) return hr;
    }

    // A sample is delivered as an MEMediaSample event carrying the sample as
    // its IUnknown payload; there is no separate delivery call.
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        queue = event_queue_;
    }
    if (!queue) return MF_E_SHUTDOWN;

    return queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
}

HRESULT QcamMediaStream::CreateSampleFromRing(IMFSample** out) {
    if (!out) return E_POINTER;
    *out = nullptr;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (!ring_open_) {
            // Retry cheaply: the service may have started since we last looked.
            ring_open_ = Succeeded(ring_.Open());
            if (!ring_open_) return E_PENDING;
        }
    }

    std::vector<uint8_t> frame;
    FrameMeta meta;
    // Wait about two frame intervals: long enough to actually catch the next
    // frame at 7.5 fps, short enough that a stalled service does not hang the
    // Frame Server's request.
    const uint32_t timeout_ms =
        static_cast<uint32_t>(frame_duration_100ns_ / 10'000 * 2 + 50);
    const Status st = ring_.Read(&frame, &meta, timeout_ms);

    if (st == Status::NoDevice) {
        std::lock_guard<std::mutex> lock(mutex_);
        ring_.Close();
        ring_open_ = false;
        return E_PENDING;
    }
    if (Failed(st)) return E_PENDING;

    if (frame.size() != frame_bytes_) {
        // The service is publishing a different size than we advertised.
        // Sending it anyway would corrupt the consumer's buffer.
        QCAM_LOGW("ring frame is %zu bytes, expected %zu; check that qcamsvc "
                  "and the virtual camera agree on --size",
                  frame.size(), frame_bytes_);
        return E_PENDING;
    }

    return WrapBuffer(frame.data(), frame.size(), next_timestamp_, out);
}

HRESULT QcamMediaStream::CreateBlankSample(IMFSample** out) {
    // Mid-grey NV12: Y at 16 (studio black would be harsher), chroma neutral.
    std::vector<uint8_t> blank(frame_bytes_);
    const size_t luma = static_cast<size_t>(width_) * height_;
    std::memset(blank.data(), 32, luma);
    std::memset(blank.data() + luma, 128, blank.size() - luma);

    ++blanks_;
    if (blanks_ == 1 || blanks_ % 150 == 0)
        QCAM_LOGD("emitting blank frames (%llu so far)",
                  static_cast<unsigned long long>(blanks_));

    return WrapBuffer(blank.data(), blank.size(), next_timestamp_, out);
}

HRESULT QcamMediaStream::WrapBuffer(const uint8_t* data, size_t size,
                                    LONGLONG timestamp, IMFSample** out) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer);
    if (FAILED(hr)) return hr;

    BYTE* dst = nullptr;
    DWORD max_len = 0;
    hr = buffer->Lock(&dst, &max_len, nullptr);
    if (FAILED(hr)) return hr;
    if (max_len < size) {
        buffer->Unlock();
        return E_UNEXPECTED;
    }
    std::memcpy(dst, data, size);
    buffer->Unlock();

    hr = buffer->SetCurrentLength(static_cast<DWORD>(size));
    if (FAILED(hr)) return hr;

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) return hr;

    hr = sample->AddBuffer(buffer.Get());
    if (FAILED(hr)) return hr;

    // Timestamps are synthesised from the advertised frame rate rather than
    // taken from the capture clock: the ring's timestamps come from a
    // different process and a monotonic, evenly spaced series is what
    // downstream encoders want.
    hr = sample->SetSampleTime(timestamp);
    if (FAILED(hr)) return hr;
    hr = sample->SetSampleDuration(frame_duration_100ns_);
    if (FAILED(hr)) return hr;

    next_timestamp_ = timestamp + frame_duration_100ns_;
    ++delivered_;

    *out = sample.Detach();
    return S_OK;
}

}  // namespace vcam
}  // namespace qcam
