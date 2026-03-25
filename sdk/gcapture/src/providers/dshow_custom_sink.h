#pragma once

#include <windows.h>
#include <dshow.h>
#include <atomic>
#include <mutex>
#include <wrl/client.h>

class DShowRawRenderer;
class DShowCustomSinkFilter;

class DShowCustomSinkPin final : public IPin, public IMemInputPin
{
public:
    explicit DShowCustomSinkPin(DShowCustomSinkFilter *owner);
    ~DShowCustomSinkPin();

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP Connect(IPin *pReceivePin, const AM_MEDIA_TYPE *pmt) override;
    STDMETHODIMP ReceiveConnection(IPin *pConnector, const AM_MEDIA_TYPE *pmt) override;
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin **ppPin) override;
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE *pmt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO *pInfo) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION *pPinDir) override;
    STDMETHODIMP QueryId(LPWSTR *Id) override;
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE *pmt) override;
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes **ppEnum) override;
    STDMETHODIMP QueryInternalConnections(IPin **apPin, ULONG *nPin) override;
    STDMETHODIMP EndOfStream() override;
    STDMETHODIMP BeginFlush() override;
    STDMETHODIMP EndFlush() override;
    STDMETHODIMP NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) override;

    STDMETHODIMP GetAllocator(IMemAllocator **ppAllocator) override;
    STDMETHODIMP NotifyAllocator(IMemAllocator *pAllocator, BOOL bReadOnly) override;
    STDMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES *pProps) override;
    STDMETHODIMP Receive(IMediaSample *pSample) override;
    STDMETHODIMP ReceiveMultiple(IMediaSample **pSamples, long nSamples, long *nSamplesProcessed) override;
    STDMETHODIMP ReceiveCanBlock() override;

    GUID subtype() const;
    int width() const;
    int height() const;
    int fpsNum() const;
    int fpsDen() const;

private:
    HRESULT setMediaType(const AM_MEDIA_TYPE *pmt);
    void freeMediaType();

private:
    std::atomic<ULONG> refCount_{1};
    DShowCustomSinkFilter *owner_ = nullptr;
    IPin *connectedPin_ = nullptr;
    IMemAllocator *allocator_ = nullptr;
    BOOL readOnly_ = FALSE;
    AM_MEDIA_TYPE mediaType_{};
    bool mediaTypeValid_ = false;
    GUID subtype_ = MEDIASUBTYPE_NULL;
    int width_ = 0;
    int height_ = 0;
    int fpsNum_ = 0;
    int fpsDen_ = 0;
    mutable std::mutex mtx_;
};

class DShowSinglePinEnum final : public IEnumPins
{
public:
    explicit DShowSinglePinEnum(IPin *pin);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP Next(ULONG cPins, IPin **ppPins, ULONG *pcFetched) override;
    STDMETHODIMP Skip(ULONG cPins) override;
    STDMETHODIMP Reset() override;
    STDMETHODIMP Clone(IEnumPins **ppEnum) override;

private:
    std::atomic<ULONG> refCount_{1};
    Microsoft::WRL::ComPtr<IPin> pin_;
    bool given_ = false;
};

class DShowNoMediaTypeEnum final : public IEnumMediaTypes
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP Next(ULONG cMediaTypes, AM_MEDIA_TYPE **ppMediaTypes, ULONG *pcFetched) override;
    STDMETHODIMP Skip(ULONG cMediaTypes) override;
    STDMETHODIMP Reset() override;
    STDMETHODIMP Clone(IEnumMediaTypes **ppEnum) override;

private:
    std::atomic<ULONG> refCount_{1};
};

class DShowCustomSinkFilter final : public IBaseFilter
{
public:
    explicit DShowCustomSinkFilter(DShowRawRenderer *renderer);
    ~DShowCustomSinkFilter();

    IPin *inputPin() const;
    bool hasReceivedSample() const { return receivedSample_.load(); }
    HRESULT onReceive(IMediaSample *sample, const AM_MEDIA_TYPE *mt, GUID subtype, int width, int height, int fpsNum, int fpsDen);

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP GetClassID(CLSID *pClassID) override;
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Run(REFERENCE_TIME tStart) override;
    STDMETHODIMP GetState(DWORD dwMilliSecsTimeout, FILTER_STATE *State) override;
    STDMETHODIMP SetSyncSource(IReferenceClock *pClock) override;
    STDMETHODIMP GetSyncSource(IReferenceClock **pClock) override;

    STDMETHODIMP EnumPins(IEnumPins **ppEnum) override;
    STDMETHODIMP FindPin(LPCWSTR Id, IPin **ppPin) override;
    STDMETHODIMP QueryFilterInfo(FILTER_INFO *pInfo) override;
    STDMETHODIMP JoinFilterGraph(IFilterGraph *pGraph, LPCWSTR pName) override;
    STDMETHODIMP QueryVendorInfo(LPWSTR *pVendorInfo) override;

private:
    std::atomic<ULONG> refCount_{1};
    FILTER_STATE state_ = State_Stopped;
    IFilterGraph *graph_ = nullptr;
    IReferenceClock *clock_ = nullptr;
    DShowRawRenderer *renderer_ = nullptr;
    DShowCustomSinkPin *pin_ = nullptr;
    std::atomic<bool> receivedSample_{false};
    mutable std::mutex mtx_;
};
