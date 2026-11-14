/*
 * Tsunagu-Pad Display: monitor virtual untuk layar kedua iPad di Windows.
 *
 * Driver Indirect Display (IddCx, mode pengguna/UMDF) yang menyediakan satu
 * monitor tanpa kabel. Tsunagu-Pad menyambungkan monitor itu ke desktop saat
 * iPad dipakai sebagai layar kedua lalu melepasnya lagi, dan menangkap
 * gambarnya lewat DXGI Desktop Duplication seperti monitor lain.
 *
 * Berdasarkan contoh IddSampleDriver dari Microsoft
 * (github.com/microsoft/Windows-driver-samples, video/IndirectDisplay),
 * Copyright (c) Microsoft Corporation, dengan lisensi Microsoft Public License
 * (MS-PL); lihat LICENSE-MS-PL.txt di folder ini. Perubahan untuk Tsunagu-Pad:
 * satu monitor dengan EDID sendiri, daftar ukuran dibaca dari registri, tanpa
 * WPP tracing.
 */
#pragma once

#define NOMINMAX
#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <memory>
#include <vector>

namespace Microsoft
{
    namespace WRL
    {
        namespace Wrappers
        {
            // Pembungkus handle thread untuk kelas pembungkus WRL yang sudah ada.
            typedef HandleT<HandleTraits::HANDLENullTraits> Thread;
        }
    }
}

namespace TsunaguPad
{
    namespace Display
    {
        struct Mode
        {
            DWORD Width;
            DWORD Height;
        };

        /// Perangkat Direct3D di GPU yang dipakai Windows untuk menggambar monitor ini.
        struct Direct3DDevice
        {
            Direct3DDevice(LUID AdapterLuid);
            HRESULT Init();

            LUID AdapterLuid;
            Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
            Microsoft::WRL::ComPtr<ID3D11Device> Device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
        };

        /// Thread yang mengambil dan melepas frame dari swap-chain monitor.
        class SwapChainProcessor
        {
        public:
            SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent);
            ~SwapChainProcessor();

        private:
            static DWORD CALLBACK RunThread(LPVOID Argument);

            void Run();
            void RunCore();

            IDDCX_SWAPCHAIN m_hSwapChain;
            std::shared_ptr<Direct3DDevice> m_Device;
            HANDLE m_hAvailableBufferEvent;
            Microsoft::WRL::Wrappers::Thread m_hThread;
            Microsoft::WRL::Wrappers::Event m_hTerminateEvent;
        };

        class IndirectDeviceContext
        {
        public:
            IndirectDeviceContext(_In_ WDFDEVICE WdfDevice);
            virtual ~IndirectDeviceContext();

            void InitAdapter();
            void FinishInit();

        protected:
            WDFDEVICE m_WdfDevice;
            IDDCX_ADAPTER m_Adapter;
        };

        class IndirectMonitorContext
        {
        public:
            IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor);
            virtual ~IndirectMonitorContext();

            void AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent);
            void UnassignSwapChain();

        private:
            IDDCX_MONITOR m_Monitor;
            std::unique_ptr<SwapChainProcessor> m_ProcessingThread;
        };
    }
}
