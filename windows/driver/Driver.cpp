/*
 * Tsunagu-Pad Display: monitor virtual untuk layar kedua iPad di Windows.
 *
 * Berdasarkan IddSampleDriver dari Microsoft (Windows-driver-samples,
 * video/IndirectDisplay), Copyright (c) Microsoft Corporation, lisensi
 * Microsoft Public License (MS-PL); lihat LICENSE-MS-PL.txt.
 *
 * Perubahan untuk Tsunagu-Pad:
 *  - satu monitor dengan EDID sendiri ("TSU0001", nama "Tsunagu-Pad") dan
 *    nomor seri serta container ID tetap, supaya Windows mengingat letak dan
 *    skalanya antarsesi;
 *  - daftar ukuran dibaca dari HKLM\SOFTWARE\TsunaguPad\Display, nilai
 *    "Modes" (REG_MULTI_SZ, mis. "1280x800"), 60 Hz; ukuran baru ditambahkan
 *    Tsunagu-Pad lalu perangkatnya dimuat ulang;
 *  - tanpa WPP tracing.
 *
 * Driver ini tidak mengolah frame sama sekali: Tsunagu-Pad menangkap gambar
 * monitor ini lewat DXGI Desktop Duplication, sama seperti monitor lain.
 */

#include "Driver.h"

#include <cwchar>

using namespace std;
using namespace TsunaguPad::Display;
using namespace Microsoft::WRL;

#pragma region Monitor

static constexpr DWORD REFRESH_RATE = 60;
static constexpr size_t MAX_MODES = 64;

// Ukuran bila registri belum berisi daftar: tiga ukuran panel Tsunagu-Pad,
// resolusi asli iPad yang umum, dan beberapa ukuran monitor biasa.
static const Mode s_DefaultModes[] =
{
    { 1280,  800 }, { 1640, 1024 }, { 2360, 1640 },
    { 1024,  768 }, { 1180,  820 }, { 1366, 1024 }, { 1194,  834 }, { 1080,  810 },
    { 2048, 1536 }, { 2160, 1620 }, { 2266, 1488 }, { 2388, 1668 }, { 2420, 1668 },
    { 2732, 2048 }, { 2752, 2064 }, { 2064, 2752 },
    { 1280,  720 }, { 1600,  900 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 }, { 2560, 1600 },
};

// Container ID tetap: monitor yang sama di setiap sesi.
static const GUID s_ContainerId = { 0x5f1c2a77, 0x3e0b, 0x4c6d, { 0x9a, 0x41, 0x7e, 0x2d, 0x10, 0x8b, 0xc3, 0x55 } };

static vector<Mode> LoadModes()
{
    vector<Mode> Modes;
    WCHAR Buffer[4096] = {};
    DWORD Size = sizeof(Buffer) - 2 * sizeof(WCHAR);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TsunaguPad\\Display", L"Modes", RRF_RT_REG_MULTI_SZ, nullptr,
                     Buffer, &Size) == ERROR_SUCCESS)
    {
        for (const WCHAR* p = Buffer; *p && Modes.size() < MAX_MODES; p += wcslen(p) + 1)
        {
            unsigned long Width = 0, Height = 0;
            if (swscanf_s(p, L"%lux%lu", &Width, &Height) != 2 || Width < 320 || Height < 240 || Width > 7680 ||
                Height > 4320)
            {
                continue;
            }
            Mode m = { Width & ~1ul, Height & ~1ul };
            bool Known = false;
            for (const Mode& k : Modes)
            {
                Known = Known || (k.Width == m.Width && k.Height == m.Height);
            }
            if (!Known)
            {
                Modes.push_back(m);
            }
        }
    }
    if (Modes.empty())
    {
        Modes.assign(begin(s_DefaultModes), end(s_DefaultModes));
    }
    return Modes;
}

// EDID 1.4 milik monitor ini. Pabrikan "TSU" dan kode produk 0x0001 menjadi
// "TSU0001" di jalur perangkat Windows, yang dipakai Tsunagu-Pad untuk
// mengenali monitornya sendiri.
static void BuildEdid(BYTE (&Edid)[128])
{
    static const BYTE Header[] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    // Deskriptor waktu 1920x1080 60 Hz (CEA-861), ukuran gambar 250x170 mm.
    static const BYTE Timing[] = { 0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40, 0x58,
                                   0x2C, 0x45, 0x00, 0xFA, 0xAA, 0x00, 0x00, 0x00, 0x1E };
    static const char Name[] = "Tsunagu-Pad";
    memset(Edid, 0, sizeof(Edid));
    memcpy(Edid, Header, sizeof(Header));
    WORD Vendor = (WORD) ((('T' - '@') << 10) | (('S' - '@') << 5) | ('U' - '@'));
    Edid[8] = (BYTE) (Vendor >> 8);
    Edid[9] = (BYTE) (Vendor & 0xFF);
    Edid[10] = 0x01;                // kode produk 0x0001
    Edid[11] = 0x00;
    Edid[12] = 0x01;                // nomor seri tetap
    Edid[16] = 1;                   // minggu pembuatan
    Edid[17] = 2026 - 1990;         // tahun pembuatan
    Edid[18] = 1;                   // EDID 1.4
    Edid[19] = 4;
    Edid[20] = 0xA5;                // digital, 8 bit per warna
    Edid[21] = 25;                  // kira-kira ukuran layar iPad, cm
    Edid[22] = 17;
    Edid[23] = 120;                 // gamma 2.2
    Edid[24] = 0x06;                // sRGB, mode pilihan di deskriptor pertama
    static const BYTE Chroma[] = { 0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54 };
    memcpy(Edid + 25, Chroma, sizeof(Chroma));
    for (int i = 38; i < 54; i++)
    {
        Edid[i] = 0x01;             // tanpa standard timing
    }
    memcpy(Edid + 54, Timing, sizeof(Timing));
    BYTE* d = Edid + 72;            // nama monitor
    d[3] = 0xFC;
    memset(d + 5, 0x20, 13);
    memcpy(d + 5, Name, sizeof(Name) - 1);
    d[5 + sizeof(Name) - 1] = 0x0A;
    Edid[90 + 3] = 0x10;            // deskriptor kosong
    Edid[108 + 3] = 0x10;
    BYTE Sum = 0;
    for (int i = 0; i < 127; i++)
    {
        Sum = (BYTE) (Sum + Edid[i]);
    }
    Edid[127] = (BYTE) (0x100 - Sum);
}

static BYTE s_Edid[128];

#pragma endregion

#pragma region helpers

static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, DWORD Width, DWORD Height, DWORD VSync, bool bMonitorMode)
{
    Mode.totalSize.cx = Mode.activeSize.cx = Width;
    Mode.totalSize.cy = Mode.activeSize.cy = Height;

    // See https://docs.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_video_signal_info
    Mode.AdditionalSignalInfo.vSyncFreqDivider = bMonitorMode ? 0 : 1;
    Mode.AdditionalSignalInfo.videoStandard = 255;

    Mode.vSyncFreq.Numerator = VSync;
    Mode.vSyncFreq.Denominator = 1;
    Mode.hSyncFreq.Numerator = VSync * Height;
    Mode.hSyncFreq.Denominator = 1;

    Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

    Mode.pixelRate = ((UINT64) VSync) * ((UINT64) Width) * ((UINT64) Height);
}

static IDDCX_MONITOR_MODE CreateIddCxMonitorMode(DWORD Width, DWORD Height, DWORD VSync, IDDCX_MONITOR_MODE_ORIGIN Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER)
{
    IDDCX_MONITOR_MODE Mode = {};

    Mode.Size = sizeof(Mode);
    Mode.Origin = Origin;
    FillSignalInfo(Mode.MonitorVideoSignalInfo, Width, Height, VSync, true);

    return Mode;
}

static IDDCX_TARGET_MODE CreateIddCxTargetMode(DWORD Width, DWORD Height, DWORD VSync)
{
    IDDCX_TARGET_MODE Mode = {};

    Mode.Size = sizeof(Mode);
    FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);

    return Mode;
}

#pragma endregion

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD TsunaguPadDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY TsunaguPadDeviceD0Entry;

EVT_IDD_CX_ADAPTER_INIT_FINISHED TsunaguPadAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES TsunaguPadAdapterCommitModes;

EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION TsunaguPadParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES TsunaguPadMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES TsunaguPadMonitorQueryModes;

EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN TsunaguPadMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN TsunaguPadMonitorUnassignSwapChain;

struct IndirectDeviceContextWrapper
{
    IndirectDeviceContext* pContext;

    void Cleanup()
    {
        delete pContext;
        pContext = nullptr;
    }
};

struct IndirectMonitorContextWrapper
{
    IndirectMonitorContext* pContext;

    void Cleanup()
    {
        delete pContext;
        pContext = nullptr;
    }
};

// This macro creates the methods for accessing an IndirectDeviceContextWrapper as a context for a WDF object
WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);

WDF_DECLARE_CONTEXT_TYPE(IndirectMonitorContextWrapper);

extern "C" BOOL WINAPI DllMain(
    _In_ HINSTANCE hInstance,
    _In_ UINT dwReason,
    _In_opt_ LPVOID lpReserved)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(lpReserved);
    UNREFERENCED_PARAMETER(dwReason);

    return TRUE;
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
    PDRIVER_OBJECT  pDriverObject,
    PUNICODE_STRING pRegistryPath
)
{
    WDF_DRIVER_CONFIG Config;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);

    WDF_DRIVER_CONFIG_INIT(&Config,
        TsunaguPadDeviceAdd
    );

    Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    return Status;
}

_Use_decl_annotations_
NTSTATUS TsunaguPadDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
    NTSTATUS Status = STATUS_SUCCESS;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;

    UNREFERENCED_PARAMETER(Driver);

    // Register for power callbacks - in this sample only power-on is needed
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDeviceD0Entry = TsunaguPadDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

    IDD_CX_CLIENT_CONFIG IddConfig;
    IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);

    // If the driver wishes to handle custom IoDeviceControl requests, it's necessary to use this callback since IddCx
    // redirects IoDeviceControl requests to an internal queue. This sample does not need this.
    // IddConfig.EvtIddCxDeviceIoControl = TsunaguPadIoDeviceControl;

    IddConfig.EvtIddCxAdapterInitFinished = TsunaguPadAdapterInitFinished;

    IddConfig.EvtIddCxParseMonitorDescription = TsunaguPadParseMonitorDescription;
    IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = TsunaguPadMonitorGetDefaultModes;
    IddConfig.EvtIddCxMonitorQueryTargetModes = TsunaguPadMonitorQueryModes;
    IddConfig.EvtIddCxAdapterCommitModes = TsunaguPadAdapterCommitModes;
    IddConfig.EvtIddCxMonitorAssignSwapChain = TsunaguPadMonitorAssignSwapChain;
    IddConfig.EvtIddCxMonitorUnassignSwapChain = TsunaguPadMonitorUnassignSwapChain;

    Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object)
    {
        // Automatically cleanup the context when the WDF object is about to be deleted
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
        if (pContext)
        {
            pContext->Cleanup();
        }
    };

    WDFDEVICE Device = nullptr;
    Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = IddCxDeviceInitialize(Device);

    // Create a new device context object and attach it to the WDF device object
    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext = new IndirectDeviceContext(Device);

    return Status;
}

_Use_decl_annotations_
NTSTATUS TsunaguPadDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);

    // This function is called by WDF to start the device in the fully-on power state.

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext->InitAdapter();

    return STATUS_SUCCESS;
}

#pragma region Direct3DDevice

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{

}

HRESULT Direct3DDevice::Init()
{
    // The DXGI factory could be cached, but if a new render adapter appears on the system, a new factory needs to be
    // created. If caching is desired, check DxgiFactory->IsCurrent() each time and recreate the factory if !IsCurrent.
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr))
    {
        return hr;
    }

    // Find the specified render adapter
    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr))
    {
        return hr;
    }

    // Create a D3D device using the render adapter. BGRA support is required by the WHQL test suite.
    hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &Device, nullptr, &DeviceContext);
    if (FAILED(hr))
    {
        // If creating the D3D device failed, it's possible the render GPU was lost (e.g. detachable GPU) or else the
        // system is in a transient state.
        return hr;
    }

    return S_OK;
}

#pragma endregion

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent)
    : m_hSwapChain(hSwapChain), m_Device(Device), m_hAvailableBufferEvent(NewFrameEvent)
{
    m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));

    // Immediately create and run the swap-chain processing thread, passing 'this' as the thread parameter
    m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor()
{
    // Alert the swap-chain processing thread to terminate
    SetEvent(m_hTerminateEvent.Get());

    if (m_hThread.Get())
    {
        // Wait for the thread to terminate
        WaitForSingleObject(m_hThread.Get(), INFINITE);
    }
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
    reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
    return 0;
}

void SwapChainProcessor::Run()
{
    // For improved performance, make use of the Multimedia Class Scheduler Service, which will intelligently
    // prioritize this thread for improved throughput in high CPU-load scenarios.
    DWORD AvTask = 0;
    HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &AvTask);

    RunCore();

    // Always delete the swap-chain object when swap-chain processing loop terminates in order to kick the system to
    // provide a new swap-chain if necessary.
    WdfObjectDelete((WDFOBJECT)m_hSwapChain);
    m_hSwapChain = nullptr;

    AvRevertMmThreadCharacteristics(AvTaskHandle);
}

void SwapChainProcessor::RunCore()
{
    // Get the DXGI device interface
    ComPtr<IDXGIDevice> DxgiDevice;
    HRESULT hr = m_Device->Device.As(&DxgiDevice);
    if (FAILED(hr))
    {
        return;
    }

    IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
    SetDevice.pDevice = DxgiDevice.Get();

    hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
    if (FAILED(hr))
    {
        return;
    }

    // Acquire and release buffers in a loop
    for (;;)
    {
        ComPtr<IDXGIResource> AcquiredBuffer;

        // Ask for the next buffer from the producer
        IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
        hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);

        // AcquireBuffer immediately returns STATUS_PENDING if no buffer is yet available
        if (hr == E_PENDING)
        {
            // We must wait for a new buffer
            HANDLE WaitHandles [] =
            {
                m_hAvailableBufferEvent,
                m_hTerminateEvent.Get()
            };
            DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, 16);
            if (WaitResult == WAIT_OBJECT_0 || WaitResult == WAIT_TIMEOUT)
            {
                // We have a new buffer, so try the AcquireBuffer again
                continue;
            }
            else if (WaitResult == WAIT_OBJECT_0 + 1)
            {
                // We need to terminate
                break;
            }
            else
            {
                // The wait was cancelled or something unexpected happened
                hr = HRESULT_FROM_WIN32(WaitResult);
                break;
            }
        }
        else if (SUCCEEDED(hr))
        {
            // We have new frame to process, the surface has a reference on it that the driver has to release
            AcquiredBuffer.Attach(Buffer.MetaData.pSurface);

            // ==============================
            // TODO: Process the frame here
            //
            // This is the most performance-critical section of code in an IddCx driver. It's important that whatever
            // is done with the acquired surface be finished as quickly as possible. This operation could be:
            //  * a GPU copy to another buffer surface for later processing (such as a staging surface for mapping to CPU memory)
            //  * a GPU encode operation
            //  * a GPU VPBlt to another surface
            //  * a GPU custom compute shader encode operation
            // ==============================

            // We have finished processing this frame hence we release the reference on it.
            // If the driver forgets to release the reference to the surface, it will be leaked which results in the
            // surfaces being left around after swapchain is destroyed.
            // NOTE: Although in this sample we release reference to the surface here; the driver still
            // owns the Buffer.MetaData.pSurface surface until IddCxSwapChainReleaseAndAcquireBuffer returns
            // S_OK and gives us a new frame, a driver may want to use the surface in future to re-encode the desktop 
            // for better quality if there is no new frame for a while
            AcquiredBuffer.Reset();
            
            // Indicate to OS that we have finished inital processing of the frame, it is a hint that
            // OS could start preparing another frame
            hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
            if (FAILED(hr))
            {
                break;
            }

            // ==============================
            // TODO: Report frame statistics once the asynchronous encode/send work is completed
            //
            // Drivers should report information about sub-frame timings, like encode time, send time, etc.
            // ==============================
            // IddCxSwapChainReportFrameStatistics(m_hSwapChain, ...);
        }
        else
        {
            // The swap-chain was likely abandoned (e.g. DXGI_ERROR_ACCESS_LOST), so exit the processing loop
            break;
        }
    }
}

#pragma endregion

#pragma region IndirectDeviceContext

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) :
    m_WdfDevice(WdfDevice)
{
    m_Adapter = {};
}

IndirectDeviceContext::~IndirectDeviceContext()
{
}

void IndirectDeviceContext::InitAdapter()
{
    // ==============================
    // TODO: Update the below diagnostic information in accordance with the target hardware. The strings and version
    // numbers are used for telemetry and may be displayed to the user in some situations.
    //
    // This is also where static per-adapter capabilities are determined.
    // ==============================

    IDDCX_ADAPTER_CAPS AdapterCaps = {};
    AdapterCaps.Size = sizeof(AdapterCaps);

    // Declare basic feature support for the adapter (required)
    AdapterCaps.MaxMonitorsSupported = 1;
    AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
    AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

    // Declare your device strings for telemetry (required)
    AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"Tsunagu-Pad Display";
    AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"Tsunagu-Pad";
    AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"Layar kedua iPad";

    // Declare your hardware and firmware versions (required)
    IDDCX_ENDPOINT_VERSION Version = {};
    Version.Size = sizeof(Version);
    Version.MajorVer = 1;
    AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
    AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;

    // Initialize a WDF context that can store a pointer to the device context object
    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT AdapterInit = {};
    AdapterInit.WdfDevice = m_WdfDevice;
    AdapterInit.pCaps = &AdapterCaps;
    AdapterInit.ObjectAttributes = &Attr;

    // Start the initialization of the adapter, which will trigger the AdapterFinishInit callback later
    IDARG_OUT_ADAPTER_INIT AdapterInitOut;
    NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);

    if (NT_SUCCESS(Status))
    {
        // Store a reference to the WDF adapter handle
        m_Adapter = AdapterInitOut.AdapterObject;

        // Store the device context object into the WDF object context
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
        pContext->pContext = this;
    }
}

void IndirectDeviceContext::FinishInit()
{
    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectMonitorContextWrapper);

    BuildEdid(s_Edid);

    IDDCX_MONITOR_INFO MonitorInfo = {};
    MonitorInfo.Size = sizeof(MonitorInfo);
    MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    MonitorInfo.ConnectorIndex = 0;
    MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
    MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    MonitorInfo.MonitorDescription.DataSize = sizeof(s_Edid);
    MonitorInfo.MonitorDescription.pData = s_Edid;
    // Monitor yang sama di setiap sesi, supaya Windows mengingat letak dan skalanya.
    MonitorInfo.MonitorContainerId = s_ContainerId;

    IDARG_IN_MONITORCREATE MonitorCreate = {};
    MonitorCreate.ObjectAttributes = &Attr;
    MonitorCreate.pMonitorInfo = &MonitorInfo;

    IDARG_OUT_MONITORCREATE MonitorCreateOut;
    NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
    if (NT_SUCCESS(Status))
    {
        auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorCreateOut.MonitorObject);
        pMonitorContextWrapper->pContext = new IndirectMonitorContext(MonitorCreateOut.MonitorObject);

        // Monitor "tercolok". Tsunagu-Pad sendiri yang menyambungkannya ke
        // desktop saat iPad dipakai sebagai layar kedua.
        IDARG_OUT_MONITORARRIVAL ArrivalOut;
        Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);
    }
}

IndirectMonitorContext::IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor) :
    m_Monitor(Monitor)
{
}

IndirectMonitorContext::~IndirectMonitorContext()
{
    m_ProcessingThread.reset();
}

void IndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent)
{
    m_ProcessingThread.reset();

    auto Device = make_shared<Direct3DDevice>(RenderAdapter);
    if (FAILED(Device->Init()))
    {
        // It's important to delete the swap-chain if D3D initialization fails, so that the OS knows to generate a new
        // swap-chain and try again.
        WdfObjectDelete(SwapChain);
    }
    else
    {
        // Create a new swap-chain processing thread
        m_ProcessingThread.reset(new SwapChainProcessor(SwapChain, Device, NewFrameEvent));
    }
}

void IndirectMonitorContext::UnassignSwapChain()
{
    // Stop processing the last swap-chain
    m_ProcessingThread.reset();
}

#pragma endregion

#pragma region DDI Callbacks

_Use_decl_annotations_
NTSTATUS TsunaguPadAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    // This is called when the OS has finished setting up the adapter for use by the IddCx driver. It's now possible
    // to report attached monitors.

    auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
    if (NT_SUCCESS(pInArgs->AdapterInitStatus))
    {
        pDeviceContextWrapper->pContext->FinishInit();
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS TsunaguPadAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);

    // For the sample, do nothing when modes are picked - the swap-chain is taken care of by IddCx

    // ==============================
    // TODO: In a real driver, this function would be used to reconfigure the device to commit the new modes. Loop
    // through pInArgs->pPaths and look for IDDCX_PATH_FLAGS_ACTIVE. Any path not active is inactive (e.g. the monitor
    // should be turned off).
    // ==============================

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS TsunaguPadParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    // EDID-nya milik driver ini sendiri; ukurannya diambil dari registri.
    if (pInArgs->MonitorDescription.DataSize != sizeof(s_Edid) ||
        memcmp(pInArgs->MonitorDescription.pData, s_Edid, sizeof(s_Edid)) != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    vector<Mode> Modes = LoadModes();
    pOutArgs->MonitorModeBufferOutputCount = (UINT) Modes.size();
    if (pInArgs->MonitorModeBufferInputCount < Modes.size())
    {
        // Pemanggil hanya menanyakan jumlahnya bila belum memberi tempat.
        return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    }
    for (size_t i = 0; i < Modes.size(); i++)
    {
        pInArgs->pMonitorModes[i] = CreateIddCxMonitorMode(Modes[i].Width, Modes[i].Height, REFRESH_RATE,
                                                           IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
    }
    pOutArgs->PreferredMonitorModeIdx = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS TsunaguPadMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);

    // Hanya dipakai untuk monitor tanpa EDID; monitor ini selalu punya EDID,
    // tetapi daftar yang sama dikembalikan untuk berjaga-jaga.
    vector<Mode> Modes = LoadModes();
    pOutArgs->DefaultMonitorModeBufferOutputCount = (UINT) Modes.size();
    if (pInArgs->DefaultMonitorModeBufferInputCount == 0)
    {
        return STATUS_SUCCESS;
    }
    if (pInArgs->DefaultMonitorModeBufferInputCount < Modes.size())
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (size_t i = 0; i < Modes.size(); i++)
    {
        pInArgs->pDefaultMonitorModes[i] = CreateIddCxMonitorMode(Modes[i].Width, Modes[i].Height, REFRESH_RATE,
                                                                  IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
    }
    pOutArgs->PreferredMonitorModeIdx = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS TsunaguPadMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);

    // Monitor ini tidak punya batas pemindaian sendiri: ukuran yang bisa
    // dipakai sama dengan daftar ukuran monitornya.
    vector<IDDCX_TARGET_MODE> TargetModes;
    for (const Mode& m : LoadModes())
    {
        TargetModes.push_back(CreateIddCxTargetMode(m.Width, m.Height, REFRESH_RATE));
    }

    pOutArgs->TargetModeBufferOutputCount = (UINT) TargetModes.size();
    if (pInArgs->TargetModeBufferInputCount >= TargetModes.size())
    {
        copy(TargetModes.begin(), TargetModes.end(), pInArgs->pTargetModes);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS TsunaguPadMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    pMonitorContextWrapper->pContext->AssignSwapChain(pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS TsunaguPadMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    pMonitorContextWrapper->pContext->UnassignSwapChain();
    return STATUS_SUCCESS;
}

#pragma endregion
