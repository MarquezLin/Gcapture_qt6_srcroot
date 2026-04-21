#include "dshow_custom_sink.h"
#include "dshow_raw_renderer.h"

#include <dvdmedia.h>
#include <mfapi.h>
#include <cstring>
#include <new>


namespace
{
static void dshow_sink_log(const char *msg)
{
    OutputDebugStringA(msg);
}

static void dshow_sink_log_hr(const char *prefix, HRESULT hr)
{
    char sys[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, static_cast<DWORD>(hr), 0,
                   sys, static_cast<DWORD>(sizeof(sys)), nullptr);
    char buf[512] = {};
    sprintf_s(buf, "[DShowRawSink] %s hr=0x%08X (%s)", prefix, static_cast<unsigned>(hr), sys[0] ? sys : "n/a");
    OutputDebugStringA(buf);
}
}

namespace
{
void free_mt(AM_MEDIA_TYPE &mt)
{
    if (mt.cbFormat && mt.pbFormat)
        CoTaskMemFree(mt.pbFormat);
    mt.pbFormat = nullptr;
    mt.cbFormat = 0;
    if (mt.pUnk)
        mt.pUnk->Release();
    mt.pUnk = nullptr;
}

HRESULT copy_mt(AM_MEDIA_TYPE &dst, const AM_MEDIA_TYPE &src)
{
    dst = src;
    dst.pbFormat = nullptr;
    dst.pUnk = src.pUnk;
    if (dst.pUnk)
        dst.pUnk->AddRef();
    if (src.cbFormat && src.pbFormat)
    {
        dst.pbFormat = reinterpret_cast<BYTE *>(CoTaskMemAlloc(src.cbFormat));
        if (!dst.pbFormat)
            return E_OUTOFMEMORY;
        memcpy(dst.pbFormat, src.pbFormat, src.cbFormat);
        dst.cbFormat = src.cbFormat;
    }
    return S_OK;
}

bool extract_video_info(const AM_MEDIA_TYPE &mt, int &w, int &h, int &fpsNum, int &fpsDen)
{
    w = h = fpsNum = fpsDen = 0;
    if (!mt.pbFormat)
        return false;
    if (mt.formattype == FORMAT_VideoInfo && mt.cbFormat >= sizeof(VIDEOINFOHEADER))
    {
        auto *vih = reinterpret_cast<const VIDEOINFOHEADER *>(mt.pbFormat);
        w = vih->bmiHeader.biWidth;
        h = abs(vih->bmiHeader.biHeight);
        if (vih->AvgTimePerFrame > 0)
        {
            fpsNum = 10000000;
            fpsDen = static_cast<int>(vih->AvgTimePerFrame);
        }
        return true;
    }
    if (mt.formattype == FORMAT_VideoInfo2 && mt.cbFormat >= sizeof(VIDEOINFOHEADER2))
    {
        auto *vih = reinterpret_cast<const VIDEOINFOHEADER2 *>(mt.pbFormat);
        w = vih->bmiHeader.biWidth;
        h = abs(vih->bmiHeader.biHeight);
        if (vih->AvgTimePerFrame > 0)
        {
            fpsNum = 10000000;
            fpsDen = static_cast<int>(vih->AvgTimePerFrame);
        }
        return true;
    }
    return false;
}
}

DShowCustomSinkPin::DShowCustomSinkPin(DShowCustomSinkFilter *owner) : owner_(owner) {}
DShowCustomSinkPin::~DShowCustomSinkPin()
{
    Disconnect();
}

STDMETHODIMP DShowCustomSinkPin::QueryInterface(REFIID riid, void **ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IPin)
        *ppv = static_cast<IPin *>(this);
    else if (riid == IID_IMemInputPin)
        *ppv = static_cast<IMemInputPin *>(this);
    else
        return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) DShowCustomSinkPin::AddRef() { return ++refCount_; }
STDMETHODIMP_(ULONG) DShowCustomSinkPin::Release() { ULONG v = --refCount_; if (!v) delete this; return v; }

HRESULT DShowCustomSinkPin::setMediaType(const AM_MEDIA_TYPE *pmt)
{
    if (!pmt) return E_POINTER;
    freeMediaType();
    HRESULT hr = copy_mt(mediaType_, *pmt);
    if (FAILED(hr)) return hr;
    mediaTypeValid_ = true;
    subtype_ = pmt->subtype;
    extract_video_info(*pmt, width_, height_, fpsNum_, fpsDen_);
    return S_OK;
}

void DShowCustomSinkPin::freeMediaType()
{
    if (mediaTypeValid_)
        free_mt(mediaType_);
    ZeroMemory(&mediaType_, sizeof(mediaType_));
    mediaTypeValid_ = false;
    subtype_ = MEDIASUBTYPE_NULL;
    width_ = height_ = fpsNum_ = fpsDen_ = 0;
}

GUID DShowCustomSinkPin::subtype() const { return subtype_; }
int DShowCustomSinkPin::width() const { return width_; }
int DShowCustomSinkPin::height() const { return height_; }
int DShowCustomSinkPin::fpsNum() const { return fpsNum_; }
int DShowCustomSinkPin::fpsDen() const { return fpsDen_; }

STDMETHODIMP DShowCustomSinkPin::Connect(IPin *, const AM_MEDIA_TYPE *) { return E_NOTIMPL; }
STDMETHODIMP DShowCustomSinkPin::ReceiveConnection(IPin *pConnector, const AM_MEDIA_TYPE *pmt)
{
    if (!pConnector || !pmt) return E_POINTER;
    {
        int w=0,h=0,fn=0,fd=0;
        extract_video_info(*pmt,w,h,fn,fd);
        wchar_t wguid[64] = {};
        StringFromGUID2(pmt->subtype, wguid, static_cast<int>(sizeof(wguid) / sizeof(wguid[0])));
        char guid[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, wguid, -1, guid, static_cast<int>(sizeof(guid)), nullptr, nullptr);
        char msg[384] = {};
        sprintf_s(msg, "[DShowRawSink] ReceiveConnection subtype=%s guid=%s %dx%d fps=%d/%d",
                  DShowRawRenderer::subtypeName(pmt->subtype), guid, w, h, fn, fd);
        dshow_sink_log(msg);
    }
    std::lock_guard<std::mutex> lock(mtx_);
    if (connectedPin_) return VFW_E_ALREADY_CONNECTED;
    if (QueryAccept(pmt) != S_OK) return VFW_E_TYPE_NOT_ACCEPTED;
    HRESULT hr = setMediaType(pmt);
    if (FAILED(hr)) return hr;
    connectedPin_ = pConnector;
    connectedPin_->AddRef();
    return S_OK;
}

STDMETHODIMP DShowCustomSinkPin::Disconnect()
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (connectedPin_) { connectedPin_->Release(); connectedPin_ = nullptr; }
    if (allocator_) { allocator_->Release(); allocator_ = nullptr; }
    freeMediaType();
    return S_OK;
}

STDMETHODIMP DShowCustomSinkPin::ConnectedTo(IPin **ppPin)
{
    if (!ppPin) return E_POINTER;
    *ppPin = nullptr;
    std::lock_guard<std::mutex> lock(mtx_);
    if (!connectedPin_) return VFW_E_NOT_CONNECTED;
    connectedPin_->AddRef();
    *ppPin = connectedPin_;
    return S_OK;
}

STDMETHODIMP DShowCustomSinkPin::ConnectionMediaType(AM_MEDIA_TYPE *pmt)
{
    if (!pmt) return E_POINTER;
    std::lock_guard<std::mutex> lock(mtx_);
    if (!mediaTypeValid_) return VFW_E_NOT_CONNECTED;
    ZeroMemory(pmt, sizeof(*pmt));
    return copy_mt(*pmt, mediaType_);
}

STDMETHODIMP DShowCustomSinkPin::QueryPinInfo(PIN_INFO *pInfo)
{
    if (!pInfo) return E_POINTER;
    ZeroMemory(pInfo, sizeof(*pInfo));
    pInfo->dir = PINDIR_INPUT;
    pInfo->pFilter = reinterpret_cast<IBaseFilter *>(owner_);
    if (pInfo->pFilter) pInfo->pFilter->AddRef();
    wcscpy_s(pInfo->achName, L"In");
    return S_OK;
}

STDMETHODIMP DShowCustomSinkPin::QueryDirection(PIN_DIRECTION *pPinDir)
{
    if (!pPinDir) return E_POINTER;
    *pPinDir = PINDIR_INPUT;
    return S_OK;
}

STDMETHODIMP DShowCustomSinkPin::QueryId(LPWSTR *Id)
{
    if (!Id) return E_POINTER;
    const wchar_t *name = L"In";
    size_t bytes = (wcslen(name) + 1) * sizeof(wchar_t);
    *Id = reinterpret_cast<LPWSTR>(CoTaskMemAlloc(bytes));
    if (!*Id) return E_OUTOFMEMORY;
    memcpy(*Id, name, bytes);
    return S_OK;
}

STDMETHODIMP DShowCustomSinkPin::QueryAccept(const AM_MEDIA_TYPE *pmt)
{
    if (!pmt) return E_POINTER;
    if (pmt->majortype != MEDIATYPE_Video) return S_FALSE;
    if (pmt->subtype == MEDIASUBTYPE_NV12 || pmt->subtype == MFVideoFormat_P010 || pmt->subtype == MEDIASUBTYPE_YUY2 || pmt->subtype == MEDIASUBTYPE_Y210 ||
        pmt->subtype == MEDIASUBTYPE_RGB24 || pmt->subtype == MEDIASUBTYPE_RGB32 || pmt->subtype == MEDIASUBTYPE_ARGB32) return S_OK;
    return S_FALSE;
}

STDMETHODIMP DShowCustomSinkPin::EnumMediaTypes(IEnumMediaTypes **ppEnum)
{
    if (!ppEnum) return E_POINTER;
    *ppEnum = new (std::nothrow) DShowNoMediaTypeEnum();
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP DShowCustomSinkPin::QueryInternalConnections(IPin **, ULONG *) { return E_NOTIMPL; }
STDMETHODIMP DShowCustomSinkPin::EndOfStream() { return S_OK; }
STDMETHODIMP DShowCustomSinkPin::BeginFlush() { return S_OK; }
STDMETHODIMP DShowCustomSinkPin::EndFlush() { return S_OK; }
STDMETHODIMP DShowCustomSinkPin::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) { return S_OK; }

STDMETHODIMP DShowCustomSinkPin::GetAllocator(IMemAllocator **ppAllocator)
{
    if (!ppAllocator) return E_POINTER;
    *ppAllocator = nullptr;
    std::lock_guard<std::mutex> lock(mtx_);
    if (!allocator_) return VFW_E_NO_ALLOCATOR;
    allocator_->AddRef();
    *ppAllocator = allocator_;
    return S_OK;
}

STDMETHODIMP DShowCustomSinkPin::NotifyAllocator(IMemAllocator *pAllocator, BOOL bReadOnly)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (allocator_) allocator_->Release();
    allocator_ = pAllocator;
    if (allocator_) allocator_->AddRef();
    readOnly_ = bReadOnly;
    return S_OK;
}

STDMETHODIMP DShowCustomSinkPin::GetAllocatorRequirements(ALLOCATOR_PROPERTIES *pProps)
{
    if (!pProps) return E_POINTER;
    ZeroMemory(pProps, sizeof(*pProps));
    return S_OK;
}

STDMETHODIMP DShowCustomSinkPin::Receive(IMediaSample *pSample)
{
    if (!pSample) return E_POINTER;
    std::lock_guard<std::mutex> lock(mtx_);
    if (!connectedPin_ || !mediaTypeValid_)
    {
        dshow_sink_log("[DShowRawSink] Receive skipped: not connected or media type invalid");
        return VFW_E_NOT_CONNECTED;
    }
    static std::atomic<unsigned> s_receiveCalls{0};
    const unsigned idx = ++s_receiveCalls;
    if (idx <= 5)
    {
        long size = pSample->GetActualDataLength();
        char msg[256] = {};
        sprintf_s(msg, "[DShowRawSink] Receive() sample#%u subtype=%s %dx%d fps=%d/%d bytes=%ld",
                  idx, DShowRawRenderer::subtypeName(subtype_), width_, height_, fpsNum_, fpsDen_, size);
        dshow_sink_log(msg);
    }
    return owner_->onReceive(pSample, &mediaType_, subtype_, width_, height_, fpsNum_, fpsDen_);
}

STDMETHODIMP DShowCustomSinkPin::ReceiveMultiple(IMediaSample **pSamples, long nSamples, long *nSamplesProcessed)
{
    if (nSamplesProcessed) *nSamplesProcessed = 0;
    if (!pSamples || nSamples <= 0) return E_INVALIDARG;
    HRESULT hr = S_OK;
    long done = 0;
    for (long i = 0; i < nSamples; ++i)
    {
        hr = Receive(pSamples[i]);
        if (FAILED(hr)) break;
        ++done;
    }
    if (nSamplesProcessed) *nSamplesProcessed = done;
    return hr;
}

STDMETHODIMP DShowCustomSinkPin::ReceiveCanBlock() { return S_FALSE; }

DShowSinglePinEnum::DShowSinglePinEnum(IPin *pin) : pin_(pin) {}
STDMETHODIMP DShowSinglePinEnum::QueryInterface(REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IEnumPins) *ppv = static_cast<IEnumPins *>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
}
STDMETHODIMP_(ULONG) DShowSinglePinEnum::AddRef() { return ++refCount_; }
STDMETHODIMP_(ULONG) DShowSinglePinEnum::Release() { ULONG v = --refCount_; if (!v) delete this; return v; }
STDMETHODIMP DShowSinglePinEnum::Next(ULONG cPins, IPin **ppPins, ULONG *pcFetched)
{
    if (pcFetched) *pcFetched = 0;
    if (!ppPins) return E_POINTER;
    if (cPins == 0) return S_OK;
    if (given_ || !pin_) return S_FALSE;
    ppPins[0] = pin_.Get();
    ppPins[0]->AddRef();
    given_ = true;
    if (pcFetched) *pcFetched = 1;
    return S_OK;
}
STDMETHODIMP DShowSinglePinEnum::Skip(ULONG cPins) { if (cPins && !given_) { given_ = true; return S_OK; } return S_FALSE; }
STDMETHODIMP DShowSinglePinEnum::Reset() { given_ = false; return S_OK; }
STDMETHODIMP DShowSinglePinEnum::Clone(IEnumPins **ppEnum)
{
    if (!ppEnum) return E_POINTER;
    auto *e = new (std::nothrow) DShowSinglePinEnum(pin_.Get());
    if (!e) return E_OUTOFMEMORY;
    e->given_ = given_;
    *ppEnum = e;
    return S_OK;
}

STDMETHODIMP DShowNoMediaTypeEnum::QueryInterface(REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) *ppv = static_cast<IEnumMediaTypes *>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
}
STDMETHODIMP_(ULONG) DShowNoMediaTypeEnum::AddRef() { return ++refCount_; }
STDMETHODIMP_(ULONG) DShowNoMediaTypeEnum::Release() { ULONG v = --refCount_; if (!v) delete this; return v; }
STDMETHODIMP DShowNoMediaTypeEnum::Next(ULONG, AM_MEDIA_TYPE **, ULONG *pcFetched) { if (pcFetched) *pcFetched = 0; return S_FALSE; }
STDMETHODIMP DShowNoMediaTypeEnum::Skip(ULONG) { return S_FALSE; }
STDMETHODIMP DShowNoMediaTypeEnum::Reset() { return S_OK; }
STDMETHODIMP DShowNoMediaTypeEnum::Clone(IEnumMediaTypes **ppEnum)
{
    if (!ppEnum) return E_POINTER;
    *ppEnum = new (std::nothrow) DShowNoMediaTypeEnum();
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

DShowCustomSinkFilter::DShowCustomSinkFilter(DShowRawRenderer *renderer) : renderer_(renderer)
{
    pin_ = new DShowCustomSinkPin(this);
}

DShowCustomSinkFilter::~DShowCustomSinkFilter()
{
    if (graph_) graph_->Release();
    if (clock_) clock_->Release();
    if (pin_) pin_->Release();
}

IPin *DShowCustomSinkFilter::inputPin() const { return pin_; }

HRESULT DShowCustomSinkFilter::onReceive(IMediaSample *sample, const AM_MEDIA_TYPE *mt, GUID subtype, int width, int height, int fpsNum, int fpsDen)
{
    if (!renderer_ || !sample || !mt) return E_POINTER;
    BYTE *ptr = nullptr;
    HRESULT hr = sample->GetPointer(&ptr);
    if (FAILED(hr) || !ptr)
    {
        dshow_sink_log_hr("IMediaSample::GetPointer", FAILED(hr) ? hr : E_FAIL);
        return FAILED(hr) ? hr : E_FAIL;
    }
    long size = sample->GetActualDataLength();
    if (size <= 0)
    {
        dshow_sink_log("[DShowRawSink] onReceive got empty sample");
        return S_OK;
    }
    renderer_->setNegotiated(subtype, width, height, fpsNum, fpsDen);
    const bool pushed = renderer_->pushSample(ptr, static_cast<size_t>(size), 0);
    static std::atomic<unsigned> s_pushLogs{0};
    const unsigned pushLogIdx = ++s_pushLogs;
    if (pushLogIdx <= 8 || !pushed)
    {
        char msg[256] = {};
        sprintf_s(msg, "[DShowRawSink] pushSample %s subtype=%s %dx%d bytes=%ld total=%llu",
                  pushed ? "OK" : "FAIL", DShowRawRenderer::subtypeName(subtype), width, height, size,
                  static_cast<unsigned long long>(renderer_->sampleCount()));
        dshow_sink_log(msg);
    }
    receivedSample_ = pushed;
    return pushed ? S_OK : E_FAIL;
}

STDMETHODIMP DShowCustomSinkFilter::QueryInterface(REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_IBaseFilter || riid == IID_IMediaFilter || riid == IID_IPersist)
        *ppv = static_cast<IBaseFilter *>(this);
    else
        return E_NOINTERFACE;
    AddRef();
    return S_OK;
}
STDMETHODIMP_(ULONG) DShowCustomSinkFilter::AddRef() { return ++refCount_; }
STDMETHODIMP_(ULONG) DShowCustomSinkFilter::Release() { ULONG v = --refCount_; if (!v) delete this; return v; }
STDMETHODIMP DShowCustomSinkFilter::GetClassID(CLSID *pClassID) { if (!pClassID) return E_POINTER; *pClassID = CLSID_NULL; return S_OK; }
STDMETHODIMP DShowCustomSinkFilter::Stop() { state_ = State_Stopped; return S_OK; }
STDMETHODIMP DShowCustomSinkFilter::Pause() { state_ = State_Paused; return S_OK; }
STDMETHODIMP DShowCustomSinkFilter::Run(REFERENCE_TIME) { state_ = State_Running; return S_OK; }
STDMETHODIMP DShowCustomSinkFilter::GetState(DWORD, FILTER_STATE *State) { if (!State) return E_POINTER; *State = state_; return S_OK; }
STDMETHODIMP DShowCustomSinkFilter::SetSyncSource(IReferenceClock *pClock)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (clock_) clock_->Release();
    clock_ = pClock;
    if (clock_) clock_->AddRef();
    return S_OK;
}
STDMETHODIMP DShowCustomSinkFilter::GetSyncSource(IReferenceClock **pClock)
{
    if (!pClock) return E_POINTER;
    std::lock_guard<std::mutex> lock(mtx_);
    *pClock = clock_;
    if (*pClock) (*pClock)->AddRef();
    return S_OK;
}
STDMETHODIMP DShowCustomSinkFilter::EnumPins(IEnumPins **ppEnum)
{
    if (!ppEnum) return E_POINTER;
    *ppEnum = new (std::nothrow) DShowSinglePinEnum(pin_);
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}
STDMETHODIMP DShowCustomSinkFilter::FindPin(LPCWSTR Id, IPin **ppPin)
{
    if (!ppPin) return E_POINTER;
    *ppPin = nullptr;
    if (!Id) return E_POINTER;
    if (_wcsicmp(Id, L"In") != 0) return VFW_E_NOT_FOUND;
    pin_->AddRef();
    *ppPin = pin_;
    return S_OK;
}
STDMETHODIMP DShowCustomSinkFilter::QueryFilterInfo(FILTER_INFO *pInfo)
{
    if (!pInfo) return E_POINTER;
    ZeroMemory(pInfo, sizeof(*pInfo));
    wcscpy_s(pInfo->achName, L"GCapCustomRawSink");
    pInfo->pGraph = graph_;
    if (pInfo->pGraph) pInfo->pGraph->AddRef();
    return S_OK;
}
STDMETHODIMP DShowCustomSinkFilter::JoinFilterGraph(IFilterGraph *pGraph, LPCWSTR)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (graph_) graph_->Release();
    graph_ = pGraph;
    if (graph_) graph_->AddRef();
    return S_OK;
}
STDMETHODIMP DShowCustomSinkFilter::QueryVendorInfo(LPWSTR *pVendorInfo)
{
    if (!pVendorInfo) return E_POINTER;
    const wchar_t *text = L"OpenAI";
    size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    *pVendorInfo = reinterpret_cast<LPWSTR>(CoTaskMemAlloc(bytes));
    if (!*pVendorInfo) return E_OUTOFMEMORY;
    memcpy(*pVendorInfo, text, bytes);
    return S_OK;
}
