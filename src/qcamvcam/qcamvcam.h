// SPDX-License-Identifier: GPL-2.0-or-later
//
// Media Foundation virtual camera source.
//
// This DLL is a COM in-process server. The Windows Frame Server instantiates
// CLSID_QcamMediaSource inside its own process when an application opens the
// camera; the source then reads decoded frames out of the shared-memory ring
// that qcamsvc fills. Nothing here touches USB - by the time a frame reaches
// this code it is already NV12.

#ifndef QCAM_VCAM_SOURCE_H_
#define QCAM_VCAM_SOURCE_H_

#include <windows.h>

// The build defines WIN32_LEAN_AND_MEAN, so <windows.h> does not pull COM in.
// The kernel-streaming headers below assume it is already there, so bring it
// in explicitly first. Without this, <cguid.h> is reached in a state where
// __uuidof is not yet usable and the SDK header fails to parse.
#include <objbase.h>

#include <ks.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <wrl/client.h>

// IKsControl is declared in <ksproxy.h>, but that header drags in
// DirectShow-era dependencies and does not survive being included here. The
// interface is three methods behind a fixed IID, so declare it directly --
// the same approach Microsoft's own virtual camera samples take. <ks.h>
// above supplies the KSPROPERTY / KSMETHOD / KSEVENT types it carries.
#ifndef __IKsControl_INTERFACE_DEFINED__
#define __IKsControl_INTERFACE_DEFINED__
MIDL_INTERFACE("28F54685-06FD-11D2-B27A-00A0C9223196")
IKsControl : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE KsProperty(
        PKSPROPERTY Property, ULONG PropertyLength, void* PropertyData,
        ULONG DataLength, ULONG* BytesReturned) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsMethod(
        PKSMETHOD Method, ULONG MethodLength, void* MethodData,
        ULONG DataLength, ULONG* BytesReturned) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsEvent(
        PKSEVENT Event, ULONG EventLength, void* EventData,
        ULONG DataLength, ULONG* BytesReturned) = 0;
};
#endif  // __IKsControl_INTERFACE_DEFINED__

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "qcam/ring.h"
#include "qcam/types.h"

namespace qcam {
namespace vcam {

using Microsoft::WRL::ComPtr;

// Fallback format used when the service is not running yet, so the camera
// still enumerates with a sane media type instead of failing to open.
constexpr UINT32 kDefaultWidth   = 352;
constexpr UINT32 kDefaultHeight  = 288;
constexpr UINT32 kDefaultFpsNum  = 15;
constexpr UINT32 kDefaultFpsDen  = 2;   // 7.5 fps, what the hardware sustains

class QcamMediaSource;

// One video stream. The Frame Server asks for samples one at a time; a worker
// thread pairs each request with the next frame out of the ring.
class QcamMediaStream final : public IMFMediaStream2 {
public:
    QcamMediaStream();
    virtual ~QcamMediaStream();

    HRESULT Initialize(QcamMediaSource* source, IMFStreamDescriptor* descriptor,
                       UINT32 width, UINT32 height, UINT32 fps_num, UINT32 fps_den);
    HRESULT Shutdown();

    HRESULT SetRate(float rate);
    HRESULT Start();
    HRESULT Pause();
    HRESULT StopStream();

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    IFACEMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    IFACEMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    IFACEMETHODIMP QueueEvent(MediaEventType type, REFGUID extended_type,
                              HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaStream
    IFACEMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    IFACEMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
    IFACEMETHODIMP RequestSample(IUnknown* token) override;

    // IMFMediaStream2
    IFACEMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    IFACEMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;

private:
    void    WorkerLoop();
    HRESULT DeliverSample(IUnknown* token);
    HRESULT CreateSampleFromRing(IMFSample** out);
    HRESULT CreateBlankSample(IMFSample** out);
    HRESULT WrapBuffer(const uint8_t* data, size_t size, LONGLONG timestamp,
                       IMFSample** out);

    std::atomic<ULONG>        ref_count_{1};
    mutable std::mutex        mutex_;

    QcamMediaSource*          source_ = nullptr;   // weak; the source owns us
    ComPtr<IMFMediaEventQueue> event_queue_;
    ComPtr<IMFStreamDescriptor> descriptor_;

    UINT32   width_   = kDefaultWidth;
    UINT32   height_  = kDefaultHeight;
    UINT32   fps_num_ = kDefaultFpsNum;
    UINT32   fps_den_ = kDefaultFpsDen;
    size_t   frame_bytes_ = 0;
    LONGLONG frame_duration_100ns_ = 0;

    MF_STREAM_STATE  state_ = MF_STREAM_STATE_STOPPED;
    bool             shutdown_ = false;

    // Pending sample requests, oldest first. Tokens may be null.
    std::vector<ComPtr<IUnknown>> pending_;
    HANDLE            work_event_ = nullptr;
    std::thread       worker_;
    std::atomic<bool> worker_stop_{false};

    FrameRingReader   ring_;
    bool              ring_open_ = false;
    LONGLONG          next_timestamp_ = 0;
    uint64_t          delivered_ = 0;
    uint64_t          blanks_ = 0;
};

// The media source. One stream, one media type: whatever the hardware can
// actually deliver.
class QcamMediaSource final : public IMFMediaSourceEx,
                              public IMFGetService,
                              public IKsControl {
public:
    QcamMediaSource();
    virtual ~QcamMediaSource();

    HRESULT Initialize();

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    IFACEMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    IFACEMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    IFACEMETHODIMP QueueEvent(MediaEventType type, REFGUID extended_type,
                              HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaSource
    IFACEMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** pd) override;
    IFACEMETHODIMP GetCharacteristics(DWORD* characteristics) override;
    IFACEMETHODIMP Pause() override;
    IFACEMETHODIMP Shutdown() override;
    IFACEMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* time_format,
                         const PROPVARIANT* start_position) override;
    IFACEMETHODIMP Stop() override;

    // IMFMediaSourceEx
    IFACEMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
    IFACEMETHODIMP GetStreamAttributes(DWORD stream_id,
                                       IMFAttributes** attributes) override;
    IFACEMETHODIMP SetD3DManager(IUnknown* manager) override;

    // IMFGetService
    IFACEMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* ppv) override;

    // IKsControl - carries the standard camera controls (brightness, contrast,
    // and so on) that apps expose in their settings panels.
    IFACEMETHODIMP KsProperty(PKSPROPERTY property, ULONG property_length,
                              void* data, ULONG data_length,
                              ULONG* bytes_returned) override;
    IFACEMETHODIMP KsMethod(PKSMETHOD method, ULONG method_length, void* data,
                            ULONG data_length, ULONG* bytes_returned) override;
    IFACEMETHODIMP KsEvent(PKSEVENT event, ULONG event_length, void* data,
                           ULONG data_length, ULONG* bytes_returned) override;

private:
    HRESULT CheckShutdown() const;
    HRESULT CreateMediaType(IMFMediaType** out) const;
    HRESULT HandleVideoProcAmp(PKSPROPERTY property, void* data, ULONG data_length,
                               ULONG* bytes_returned);

    std::atomic<ULONG>  ref_count_{1};
    mutable std::mutex  mutex_;

    ComPtr<IMFMediaEventQueue>       event_queue_;
    ComPtr<IMFPresentationDescriptor> descriptor_;
    ComPtr<IMFAttributes>            source_attributes_;
    ComPtr<IMFAttributes>            stream_attributes_;
    QcamMediaStream*                 stream_ = nullptr;

    UINT32 width_   = kDefaultWidth;
    UINT32 height_  = kDefaultHeight;
    UINT32 fps_num_ = kDefaultFpsNum;
    UINT32 fps_den_ = kDefaultFpsDen;
    bool   shutdown_ = false;
    // The first Start() announces the stream with MENewStream; later ones
    // use MEUpdatedStream.
    bool   stream_announced_ = false;
};

// Module-wide COM object count, so DllCanUnloadNow can answer correctly.
extern std::atomic<long> g_object_count;

}  // namespace vcam
}  // namespace qcam

#endif  // QCAM_VCAM_SOURCE_H_
