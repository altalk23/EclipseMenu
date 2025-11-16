#include "WMFRecorder.hpp"
#include "modules/recorder/recorder.hpp"
#include <cstdint>
#include <dxgiformat.h>

#ifdef GEODE_IS_WINDOWS
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl.h>
#include <Geode/cocos/platform/third_party/win32/OGLES/GL/wglew.h>

#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/loader/Log.hpp>
#include <Geode/utils/general.hpp>
#include <modules/config/config.hpp>
#include <modules/debug/benchmark.hpp>
#include <modules/recorder/DSPRecorder.hpp>
#include <modules/utils/SingletonCache.hpp>
#include <utils.hpp>

auto RGBA2NV12 = R"HLSL(
Texture2D<float4> src : register(t0);
RWTexture2D<float> dstY : register(u0);
RWTexture2D<float2> dstUV : register(u1);
cbuffer FrameSize : register(b0)
{
    uint width;
    uint height;
    uint padding[2];
};

[numthreads(16,16,1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint x = id.x;
    uint y = id.y;

    if (x >= width || y >= height) {
        return;
    }

    float4 rgba = src.Load(int3(x, y, 0));
    float r = rgba.r * 255;
    float g = rgba.g * 255;
    float b = rgba.b * 255;

    float Y = clamp(0.257*r + 0.504*g + 0.098*b + 16, 0, 255) / 255.0;
    dstY[int2(x, y)] = Y;

    if ((x % 2 == 0) && (y % 2 == 0)) {
        uint x1 = min(x+1, width-1);
        uint y1 = min(y+1, height-1);

        float4 c0 = src.Load(int3(x, y, 0));
        float4 c1 = src.Load(int3(x1, y, 0));
        float4 c2 = src.Load(int3(x, y1, 0));
        float4 c3 = src.Load(int3(x1, y1, 0));

        float rAvg = 0.25*(c0.r + c1.r + c2.r + c3.r)*255.0;
        float gAvg = 0.25*(c0.g + c1.g + c2.g + c3.g)*255.0;
        float bAvg = 0.25*(c0.b + c1.b + c2.b + c3.b)*255.0;

        float U = clamp(-0.147*rAvg - 0.289*gAvg + 0.436*bAvg + 128.0, 0.0, 255.0) / 255.0;
        float V = clamp(0.615*rAvg - 0.515*gAvg - 0.100*bAvg + 128.0, 0.0, 255.0) / 255.0;

        dstUV[int2(x/2, y/2)] = float2(U, V);
    }
}
)HLSL";

namespace eclipse::recorder {
    namespace ffmpeg = ffmpeg::events;
    using Microsoft::WRL::ComPtr;

    class WMFRecorder::Impl {
    public:
        WMFRecorder* self = nullptr;

        Impl(WMFRecorder* recorder) : self(recorder) {}

        LONGLONG rtStart = 0;
        ComPtr<IMFTransform> encoder;
        ComPtr<ID3D12Device> d3dDevice;
        ComPtr<ID3D12CommandQueue> d3dCommandQueue;
        ComPtr<ID3D12GraphicsCommandList> d3dCommandList;
        ComPtr<IMFDXGIDeviceManager> dxgiManager;
        UINT resetToken = 0;

        bool initialized = false;
        bool started = false;

        HANDLE dxDeviceHandle;
        HANDLE glDxHandle;
        GLuint glTexture;

        ComPtr<ID3D12DescriptorHeap> descriptorHeap;

        ComPtr<ID3D12Resource> rgbaTexture;
        ComPtr<ID3D12Resource> nv12Texture;

        D3D12_CPU_DESCRIPTOR_HANDLE nv12YUAV;
        D3D12_CPU_DESCRIPTOR_HANDLE nv12UVUAV;
        D3D12_CPU_DESCRIPTOR_HANDLE rgbaSRV;

        ComPtr<ID3D12RootSignature> rootSignature;
        ComPtr<ID3D12PipelineState> computePipeline;
        
        ComPtr<ID3D12Resource> frameSizeBuffer;
        ComPtr<ID3DBlob> csBlob;
        ComPtr<ID3DBlob> errorBlob;

        std::thread m_mftEventThread;
        std::atomic_bool m_mftThreadRunning{false};

        std::mutex m_sampleQueueMutex;
        std::condition_variable m_sampleQueueCv;
        std::deque<ComPtr<IMFSample>> m_sampleQueue;

        ComPtr<IMFMediaEventGenerator> m_eventGenerator;

        bool startPipeline();

        void init();
        void start();
        void stop();

        void visitFrame();
        bool drain();

        void returnErr(HRESULT hr, std::string const& msg) {
            std::array<WCHAR, 512> lpMsgBuf = {};
            FormatMessageW(
                FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr,
                hr,
                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                lpMsgBuf.data(),
                512, nullptr
            );

            self->m_callback(fmt::format("{}-{}-{}", msg, hr, geode::utils::string::wideToUtf8(lpMsgBuf.data())));
            return;
        }

        bool returnErrFalse(HRESULT hr, std::string const& msg) {
            returnErr(hr, msg);
            return false;
        }

        void returnErrUnlock(HRESULT hr, std::string const& msg) {
            wglDXUnlockObjectsNV(dxDeviceHandle, 1, &glDxHandle);
            returnErr(hr, msg);
        }
    };

    void WMFRecorder::Impl::init() {
        if (initialized) {
            return;
        }

        HRESULT hr = D3DCompile(
            RGBA2NV12,
            strlen(RGBA2NV12),
            nullptr,
            nullptr,
            nullptr,
            "main",
            "cs_5_0",
            0,
            0,
            &csBlob,
            &errorBlob
        );
        if (FAILED(hr)) {
            if (errorBlob) {
                std::string errorMsg((char*)errorBlob->GetBufferPointer(), errorBlob->GetBufferSize());
                this->returnErr(hr, fmt::format("Failed to compile HLSL shader: {}", errorMsg));
            } else {
                this->returnErr(hr, "Failed to compile HLSL shader: Unknown error.");
            }
            return;
        }
        initialized = true;
    }

    bool WMFRecorder::Impl::startPipeline() {
        D3D12_DESCRIPTOR_RANGE1 uavRanges[2];
        uavRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRanges[0].NumDescriptors = 1;
        uavRanges[0].BaseShaderRegister = 0;
        uavRanges[0].RegisterSpace = 0;
        uavRanges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
        uavRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        uavRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRanges[1].NumDescriptors = 1;
        uavRanges[1].BaseShaderRegister = 1;
        uavRanges[1].RegisterSpace = 0;
        uavRanges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
        uavRanges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE1 srvRanges[1];
        srvRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[0].NumDescriptors = 1;
        srvRanges[0].BaseShaderRegister = 0;
        srvRanges[0].RegisterSpace = 0;
        srvRanges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
        srvRanges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER1 rootParams[3];
        rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[0].DescriptorTable.NumDescriptorRanges = 2;
        rootParams[0].DescriptorTable.pDescriptorRanges = uavRanges;
        rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[1].DescriptorTable.pDescriptorRanges = srvRanges;
        rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParams[2].Constants.Num32BitValues = 4;
        rootParams[2].Constants.ShaderRegister = 0;
        rootParams[2].Constants.RegisterSpace = 0;
        rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc = {};
        rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rootSigDesc.Desc_1_1.NumParameters = _countof(rootParams);
        rootSigDesc.Desc_1_1.pParameters = rootParams;
        rootSigDesc.Desc_1_1.NumStaticSamplers = 0;
        rootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
        rootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        // Serialize & create root signature
        ComPtr<ID3DBlob> serializedRootSig;
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3D12SerializeVersionedRootSignature(
            &rootSigDesc,
            &serializedRootSig,
            &errorBlob
        );
        if (FAILED(hr)) {
            if (errorBlob) {
                std::string errorMsg((char*)errorBlob->GetBufferPointer(), errorBlob->GetBufferSize());
                this->returnErr(hr, fmt::format("Failed to serialize root signature: {}", errorMsg));
            } else {
                this->returnErr(hr, "Failed to serialize root signature: Unknown error.");
            }
            return false;
        }

        hr = d3dDevice->CreateRootSignature(
            0,
            serializedRootSig->GetBufferPointer(),
            serializedRootSig->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature)
        );
        if (FAILED(hr)) return this->returnErrFalse(hr, "Failed to create root signature");

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = rootSignature.Get();
        psoDesc.CS.pShaderBytecode = csBlob->GetBufferPointer();
        psoDesc.CS.BytecodeLength = csBlob->GetBufferSize();
        hr = d3dDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&computePipeline));
        if (FAILED(hr)) return this->returnErrFalse(hr, "Failed to create compute PSO");

        return true;
    }

    void WMFRecorder::Impl::start() {
        if (!initialized) {
            self->m_callback("WMFRecorder not initialized properly.");
            return;
        }
        if (started) {
            return;
        }
        MFStartup(MF_VERSION);

        D3D_FEATURE_LEVEL featureLevel;
        HRESULT hr = D3D12CreateDevice(
            nullptr,
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&d3dDevice)
        );
        if (FAILED(hr)) return this->returnErr(hr, "Failed to create D3D12 device.");

        hr = MFCreateDXGIDeviceManager(&resetToken, &dxgiManager);
        if (FAILED(hr)) return this->returnErr(hr, "Failed to create DXGI Device Manager.");

        hr = dxgiManager->ResetDevice(d3dDevice.Get(), resetToken);
        if (FAILED(hr)) return this->returnErr(hr, "Failed to reset DXGI Device Manager.");

        IMFActivate** ppActivate = nullptr;
        UINT32 ppCount = 0;
        MFT_REGISTER_TYPE_INFO inputType = {
            MFMediaType_Video,
            MFVideoFormat_NV12
        };
        MFT_REGISTER_TYPE_INFO outputType = {
            MFMediaType_Video,
            MFVideoFormat_H264
        };

        hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            &inputType,
            &outputType,
            &ppActivate,
            &ppCount
        );
        if (FAILED(hr)) return this->returnErr(hr, "Failed to enumerate video encoders.");

        if (ppCount == 0) {
            self->m_callback("No suitable video encoder found.");
            return;
        }

        for (auto i = 0; i < ppCount; i++) {
            LPWSTR encoderName = nullptr;
            UINT32 nameLength = 0;
            hr = ppActivate[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &encoderName, &nameLength);
            if (FAILED(hr)) return this->returnErr(hr, "Failed to get encoder name.");

            GUID encoderCLSID;
            hr = ppActivate[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &encoderCLSID);
            if (FAILED(hr)) return this->returnErr(hr, "Failed to get encoder CLSID.");

            LPWSTR encoderGUIDString = nullptr;
            hr = StringFromCLSID(encoderCLSID, &encoderGUIDString);
            if (FAILED(hr)) return this->returnErr(hr, "Failed to get encoder GUID string.");

            geode::log::info("Using encoder: {} - {}", geode::utils::string::wideToUtf8(encoderName), geode::utils::string::wideToUtf8(encoderGUIDString));
        }

        hr = ppActivate[2]->ActivateObject(__uuidof(IMFTransform), (void**) &encoder);
        if (FAILED(hr)) return this->returnErr(hr, "Failed to activate encoder MFT.");
        for (auto i = 0; i < ppCount; i++) ppActivate[i]->Release();
        CoTaskMemFree(ppActivate);

        ComPtr<IMFAttributes> attrs;
        encoder->GetAttributes(&attrs);
        attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);

        hr = encoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)dxgiManager.Get());
        if (FAILED(hr)) return this->returnErr(hr, "Failed to set D3D manager.");

        if (!this->startPipeline()) return;

        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.NodeMask = 0;

        hr = d3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&d3dCommandQueue));
        if (FAILED(hr)) return this->returnErr(hr, "Failed to create D3D12 command queue.");

        ComPtr<ID3D12CommandAllocator> commandAllocator;
        hr = d3dDevice->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&commandAllocator)
        );
        if (FAILED(hr)) return this->returnErr(hr, "Failed to create D3D12 command allocator.");
        
        hr = d3dDevice->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            commandAllocator.Get(),
            computePipeline.Get(),
            IID_PPV_ARGS(&d3dCommandList)
        );
        if (FAILED(hr)) return this->returnErr(hr, "Failed to create D3D12 command list.");


        // output type, h264 till i figure out how to change
        ComPtr<IMFMediaType> outType;
        MFCreateMediaType(&outType);
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, self->m_renderSettings.m_width, self->m_renderSettings.m_height);
        MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, self->m_renderSettings.m_fps, 1);
        hr = encoder->SetOutputType(0, outType.Get(), 0);
        if (FAILED(hr)) return this->returnErr(hr, "Failed to set output type.");

        // input type, reading from nvdia gpu directly
        ComPtr<IMFMediaType> inType;
        MFCreateMediaType(&inType);
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, self->m_renderSettings.m_width, self->m_renderSettings.m_height);
        MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, self->m_renderSettings.m_fps, 1);
        hr = encoder->SetInputType(0, inType.Get(), 0);
        if (FAILED(hr)) return this->returnErr(hr, "Failed to set input type.");

        encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        dxDeviceHandle = wglDXOpenDeviceNV(d3dDevice.Get());
        glTexture = self->m_renderTexture.m_texture;

        D3D12_RESOURCE_DESC descRgba = {};
        descRgba.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        descRgba.Width = self->m_renderSettings.m_width;
        descRgba.Height = self->m_renderSettings.m_height;
        descRgba.DepthOrArraySize = 1;
        descRgba.MipLevels = 1;
        descRgba.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        descRgba.SampleDesc.Count = 1;
        descRgba.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        descRgba.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        hr = d3dDevice->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &descRgba,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&rgbaTexture)
        );
        if (FAILED(hr)) return this->returnErr(hr, "Failed to create RGBA texture.");

        // bind the gl texture to d3d directly
        glDxHandle = wglDXRegisterObjectNV(
            dxDeviceHandle,
            rgbaTexture.Get(),
            glTexture,
            GL_TEXTURE_2D,
            WGL_ACCESS_READ_WRITE_NV
        );
        if (!glDxHandle) {
            self->m_callback("Failed to register GL texture with DX.");
            return;
        }

        D3D12_RESOURCE_DESC descNV12 = {};
        descNV12.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        descNV12.Width = self->m_renderSettings.m_width;
        descNV12.Height = self->m_renderSettings.m_height;
        descNV12.DepthOrArraySize = 1;
        descNV12.MipLevels = 1;
        descNV12.Format = DXGI_FORMAT_NV12;
        descNV12.SampleDesc.Count = 1;
        descNV12.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        descNV12.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        hr = d3dDevice->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &descNV12,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&nv12Texture)
        );
        if (FAILED(hr)) return this->returnErr(hr, "Failed to create RGBA texture.");

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 3;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        heapDesc.NodeMask = 0;
        hr = d3dDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
        if (FAILED(hr)) return this->returnErr(hr, "Failed to create descriptor heap.");

        UINT descriptorSize = d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = descriptorHeap->GetGPUDescriptorHandleForHeapStart();

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDescY = {};
        uavDescY.Format = DXGI_FORMAT_R8_UNORM;
        uavDescY.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDescY.Texture2D.MipSlice = 0;
        uavDescY.Texture2D.PlaneSlice = 0;
        d3dDevice->CreateUnorderedAccessView(
            nv12Texture.Get(),
            nullptr,
            &uavDescY,
            cpuHandle
        );
        D3D12_GPU_DESCRIPTOR_HANDLE uavGPUHandle = gpuHandle;
        nv12YUAV = cpuHandle;

        cpuHandle.ptr += descriptorSize;
        gpuHandle.ptr += descriptorSize;

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDescUV = {};
        uavDescUV.Format = DXGI_FORMAT_R8G8_UNORM;
        uavDescUV.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uavDescUV.Texture2D.MipSlice = 0;
        uavDescUV.Texture2D.PlaneSlice = 1;
        d3dDevice->CreateUnorderedAccessView(
            nv12Texture.Get(),
            nullptr,
            &uavDescUV,
            cpuHandle
        );
        nv12UVUAV = cpuHandle;

        cpuHandle.ptr += descriptorSize;
        gpuHandle.ptr += descriptorSize;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        d3dDevice->CreateShaderResourceView(
            rgbaTexture.Get(),
            &srvDesc,
            cpuHandle
        );
        rgbaSRV = cpuHandle;
        D3D12_GPU_DESCRIPTOR_HANDLE srvGPUHandle = gpuHandle;

        cpuHandle.ptr += descriptorSize;

        commandAllocator->Reset();
        d3dCommandList->Reset(commandAllocator.Get(), computePipeline.Get());
        
        ID3D12DescriptorHeap* heaps[] = { descriptorHeap.Get() };
        d3dCommandList->SetDescriptorHeaps(_countof(heaps), heaps);
        d3dCommandList->SetComputeRootSignature(rootSignature.Get());
        d3dCommandList->SetComputeRootDescriptorTable(0, uavGPUHandle);
        d3dCommandList->SetComputeRootDescriptorTable(1, srvGPUHandle);
        d3dCommandList->SetComputeRootConstantBufferView(2, frameSizeBuffer->GetGPUVirtualAddress());

        
        d3dCommandList->Close();
        ID3D12CommandList* lists[] = { d3dCommandList.Get() };
        d3dCommandQueue->ExecuteCommandLists(_countof(lists), lists);

        struct FrameSizeCB {
            UINT width;
            UINT height;
            UINT padding[2];
        };

        FrameSizeCB cbData = { self->m_renderSettings.m_width, self->m_renderSettings.m_height, 0, 0 };

        heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

        D3D12_RESOURCE_DESC cbDesc = {};
        cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        cbDesc.Alignment = 0;
        cbDesc.Width = sizeof(FrameSizeCB);
        cbDesc.Height = 1;
        cbDesc.DepthOrArraySize = 1;
        cbDesc.MipLevels = 1;
        cbDesc.Format = DXGI_FORMAT_UNKNOWN;
        cbDesc.SampleDesc.Count = 1;
        cbDesc.SampleDesc.Quality = 0;
        cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        cbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        hr = d3dDevice->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&frameSizeBuffer)
        );
        if (FAILED(hr)) return this->returnErr(hr, "Failed to create frame size buffer");

        // Map and copy data
        UINT8* pData;
        D3D12_RANGE range = {0, 0}; // We do not intend to read from it on CPU
        hr = frameSizeBuffer->Map(0, &range, reinterpret_cast<void**>(&pData));
        if (FAILED(hr)) return this->returnErr(hr, "Failed to map frame size buffer");

        memcpy(pData, &cbData, sizeof(FrameSizeCB));
        frameSizeBuffer->Unmap(0, nullptr);

        rtStart = 0;

        encoder.As(&m_eventGenerator);
        if (!m_eventGenerator) {
            self->m_callback("Encoder does not support media event generator interface.");
            return;
        }

        encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        started = true;
    }

    void WMFRecorder::Impl::stop() {
        if (!initialized) {
            self->m_callback("WMFRecorder not initialized properly.");
            return;
        }
        if (encoder) {
            encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);

            while (true) {
                // this->drain();
                // auto _ = self->handleRecordThread();
            
                if (self->m_currentFrame.size() == 0) {
                    break;
                }
            }

            encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }        

        if (glDxHandle) {
            wglDXUnregisterObjectNV(dxDeviceHandle, glDxHandle);
            glDxHandle = nullptr;
        }
        if (dxDeviceHandle) {
            wglDXCloseDeviceNV(dxDeviceHandle);
            dxDeviceHandle = nullptr;
        }

        rgbaTexture.Reset();
        nv12Texture.Reset();
        computePipeline.Reset();
        frameSizeBuffer.Reset();
        glTexture = 0;
        encoder.Reset();
        dxgiManager.Reset();
        d3dDevice.Reset();

        MFShutdown();
    }

    void WMFRecorder::Impl::visitFrame() {
        if (!initialized) {
            self->m_callback("WMFRecorder not initialized properly.");
            return;
        }
        if (!started) {
            return;
        }

        auto ret = wglDXLockObjectsNV(dxDeviceHandle, 1, &glDxHandle);
        if (!ret) {
            self->m_callback("Failed to lock GL object for DX access.");
            return;
        }

        glViewport(0, 0, self->m_renderTexture.m_width, self->m_renderTexture.m_height);

        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &self->m_renderTexture.m_oldFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, self->m_renderTexture.m_fbo);

        auto director = utils::get<cocos2d::CCDirector>();
        director->setProjection(cocos2d::kCCDirectorProjectionCustom);
        utils::get<PlayLayer>()->visit();

        director->setProjection(cocos2d::kCCDirectorProjection2D);

        glBindFramebuffer(GL_FRAMEBUFFER, self->m_renderTexture.m_oldFBO);
        director->setViewport();

        UINT dispatchX = (self->m_renderSettings.m_width + 15) / 16;
        UINT dispatchY = (self->m_renderSettings.m_height + 15) / 16;
        d3dCommandList->Dispatch(dispatchX, dispatchY, 1);

        d3dCommandList->Close();
        ID3D12CommandList* lists[] = { d3dCommandList.Get() };
        d3dCommandQueue->ExecuteCommandLists(_countof(lists), lists);

        wglDXUnlockObjectsNV(dxDeviceHandle, 1, &glDxHandle);

        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = MFCreateDXGISurfaceBuffer(
            __uuidof(ID3D12Resource),
            nv12Texture.Get(),
            0,
            FALSE,
            &buffer
        );
        if (FAILED(hr)) return this->returnErrUnlock(hr, "Failed to create DXGI surface y buffer.");

        ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) return this->returnErrUnlock(hr, "Failed to create sample.");

        hr = sample->AddBuffer(buffer.Get());
        if (FAILED(hr)) return this->returnErrUnlock(hr, "Failed to add buffer to sample.");

        hr = sample->SetSampleTime(rtStart);
        if (FAILED(hr)) return this->returnErrUnlock(hr, "Failed to set sample time.");

        hr = sample->SetSampleDuration(10 * 1000 * 1000 / self->m_renderSettings.m_fps);
        if (FAILED(hr)) return this->returnErrUnlock(hr, "Failed to set sample duration.");

        rtStart += 10 * 1000 * 1000 / self->m_renderSettings.m_fps;

        // Access RGBA texture
        {
            // Assume rgbaTexture is your GPU resource (ID3D12Resource)
            D3D12_RESOURCE_DESC desc = rgbaTexture->GetDesc();

            // Create a readback buffer
            D3D12_RESOURCE_DESC readbackDesc = {};
            readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            readbackDesc.Alignment = 0;
            readbackDesc.Width = desc.Width * desc.Height * 4; // RGBA8
            readbackDesc.Height = 1;
            readbackDesc.DepthOrArraySize = 1;
            readbackDesc.MipLevels = 1;
            readbackDesc.Format = DXGI_FORMAT_UNKNOWN;
            readbackDesc.SampleDesc.Count = 1;
            readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            readbackDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_READBACK;

            ComPtr<ID3D12Resource> readbackBuffer;
            HRESULT hr = d3dDevice->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &readbackDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&readbackBuffer)
            );
            if (FAILED(hr)) { /* handle error */ }

            // Copy texture to buffer
            D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
            srcLocation.pResource = rgbaTexture.Get();
            srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcLocation.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
            dstLocation.pResource = readbackBuffer.Get();
            dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            d3dDevice->GetCopyableFootprints(&desc, 0, 1, 0, &dstLocation.PlacedFootprint, nullptr, nullptr, nullptr);

            d3dCommandList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
            d3dCommandList->Close();
            ID3D12CommandList* lists[] = { d3dCommandList.Get() };
            d3dCommandQueue->ExecuteCommandLists(_countof(lists), lists);
            // Wait for GPU to finish copy before mapping

            // Map the buffer
            void* mappedData = nullptr;
            readbackBuffer->Map(0, nullptr, &mappedData);

            UINT x = 10;
            UINT y = 20;
            BYTE* row = reinterpret_cast<BYTE*>(mappedData) + y * dstLocation.PlacedFootprint.Footprint.RowPitch;
            BYTE* pixel = row + x * 4; // RGBA8
            BYTE r = pixel[0];
            BYTE g = pixel[1];
            BYTE b = pixel[2];
            BYTE a = pixel[3];

            geode::log::debug("Pixel at ({}, {}): R={} G={} B={} A={}", x, y, r, g, b, a);

            readbackBuffer->Unmap(0, nullptr);
        }

        // Access NV12 texture
        {
            D3D12_RESOURCE_DESC desc = nv12Texture->GetDesc();

            // Readback buffer
            UINT yPlaneSize = desc.Width * desc.Height;
            UINT uvPlaneSize = (desc.Width * desc.Height) / 2;
            UINT totalSize = yPlaneSize + uvPlaneSize;

            D3D12_RESOURCE_DESC readbackDesc = {};
            readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            readbackDesc.Alignment = 0;
            readbackDesc.Width = totalSize;
            readbackDesc.Height = 1;
            readbackDesc.DepthOrArraySize = 1;
            readbackDesc.MipLevels = 1;
            readbackDesc.Format = DXGI_FORMAT_UNKNOWN;
            readbackDesc.SampleDesc.Count = 1;
            readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            readbackDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_READBACK;

            ComPtr<ID3D12Resource> readbackBuffer;
            HRESULT hr = d3dDevice->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &readbackDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&readbackBuffer)
            );
            if (FAILED(hr)) { /* handle error */ }

            // Copy NV12 texture to readback buffer
            D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
            srcLocation.pResource = nv12Texture.Get();
            srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcLocation.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
            dstLocation.pResource = readbackBuffer.Get();
            dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
            footprint.Footprint.Width = desc.Width;
            footprint.Footprint.Height = desc.Height;
            footprint.Footprint.Depth = 1;
            footprint.Footprint.RowPitch = desc.Width; // Y plane
            footprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
            dstLocation.PlacedFootprint = footprint;

            d3dCommandList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
            d3dCommandList->Close();
            ID3D12CommandList* lists[] = { d3dCommandList.Get() };
            d3dCommandQueue->ExecuteCommandLists(_countof(lists), lists);
            // Wait for GPU to finish copy before mapping

            void* mappedData = nullptr;
            readbackBuffer->Map(0, nullptr, &mappedData);

            UINT x = 10;
            UINT y = 20;
            BYTE* row = reinterpret_cast<BYTE*>(mappedData) + y * desc.Width;
            BYTE yValue = row[x];

            // UV plane
            BYTE* uvPlane = reinterpret_cast<BYTE*>(mappedData) + yPlaneSize;
            BYTE uValue = uvPlane[(y / 2) * (desc.Width) + (x & ~1)];       // U at even index
            BYTE vValue = uvPlane[(y / 2) * (desc.Width) + (x & ~1) + 1];   // V at odd index

            geode::log::debug("NV12 Pixel at ({}, {}): Y={} U={} V={}", x, y, yValue, uValue, vValue);

            readbackBuffer->Unmap(0, nullptr);
        }

        {
            std::lock_guard lock(m_sampleQueueMutex);
            m_sampleQueue.push_back(sample);
        }
        m_sampleQueueCv.notify_one();

        self->m_frameReady.set(true);
    }

    bool WMFRecorder::Impl::drain() {
        if (!initialized) {
            self->m_callback("WMFRecorder not initialized properly.");
            return false;
        }
        if (!started) {
            return false;
        }

        self->m_currentFrame = std::span<uint8_t>();

        MFT_OUTPUT_STREAM_INFO info;
        HRESULT hr = encoder->GetOutputStreamInfo(0, &info);
        if (FAILED(hr)) return this->returnErrFalse(hr, "Failed to get output stream info.");

        ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreateMemoryBuffer(self->m_renderSettings.m_width * self->m_renderSettings.m_height * 3 / 2, &buffer);
        if (FAILED(hr)) return this->returnErrFalse(hr, "Failed to create memory buffer.");

        MFT_OUTPUT_DATA_BUFFER mftOutput;
        mftOutput.dwStreamID = 0;
        mftOutput.pSample = nullptr;
        mftOutput.pEvents = nullptr;
        mftOutput.pSample = nullptr;
        hr = MFCreateSample(&mftOutput.pSample);
        if (FAILED(hr)) return this->returnErrFalse(hr, "Failed to create sample for output.");
        mftOutput.pSample->AddBuffer(buffer.Get());

        DWORD status = 0;
        hr = encoder->ProcessOutput(0, 1, &mftOutput, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return false;
        }
        else if (FAILED(hr)) return this->returnErrFalse(hr, "Failed to process output.");

        geode::log::debug("Status after ProcessOutput: {}", status);
            
        mftOutput.pSample->GetSampleTime(&self->m_pts);
        mftOutput.pSample->GetSampleDuration(&self->m_dts);
        self->m_denom = 10 * 1000 * 1000;

        size_t curSize = self->m_encodedData.size();
        BYTE* data = nullptr;
        DWORD maxLen = 0, curLen = 0;
        buffer->Lock(&data, &maxLen, &curLen);
        geode::log::debug("maxlen, curlen: {}, {}", maxLen, curLen);
        self->m_encodedData.insert(self->m_encodedData.end(), data, data + curLen);
        buffer->Unlock();

        self->m_currentFrame = std::span(self->m_encodedData.begin() + curSize, self->m_encodedData.end());
        if (curSize == 0) {
            geode::log::warn("No encoded data available after ProcessOutput.");
            return false;
        }

        geode::log::debug("Encoded data size: {} bytes", self->m_currentFrame.size());

        return true;
    }

    void WMFRecorder::start() {
        m_impl->init();
        if (!m_impl->initialized || m_recording) return;

        Recorder::start();
        m_impl->start();
        std::thread(&WMFRecorder::recordThread, this).detach();
    }

    void WMFRecorder::stop() {
        if (!m_impl->initialized || !m_recording) return;

        Recorder::stop();
        m_impl->stop();

        m_encodedData.clear();
        m_currentFrame = std::span<uint8_t>();
    }

    void WMFRecorder::visitFrame() {
        // don't capture if we're not recording
        if (!m_recording) return;

        m_impl->visitFrame();
    }

    geode::Result<> WMFRecorder::handleRecordThread(ffmpeg::Recorder& recorder) {
        if (!m_impl->initialized || !m_recording) {
            return geode::Err("WMFRecorder not initialized or not recording.");
        }

        if (!m_impl->started) {
            return geode::Err("WMFRecorder not started.");
        }
        
        while (true) {
            ComPtr<IMFMediaEvent> event;
            HRESULT hr = m_impl->m_eventGenerator->GetEvent(0, &event);
            if (FAILED(hr)) {
                geode::log::error("GetEvent failed: 0x{:X}", hr);
                break;
            }

            MediaEventType type = Unknown;
            event->GetType(&type);

            geode::log::debug("Received MFT event of type: {}", type);

            switch (type) {
                case METransformNeedInput: {
                    // pop one sample (wait until available or timeout)
                    ComPtr<IMFSample> sample;
                    {
                        std::unique_lock lock(m_impl->m_sampleQueueMutex);
                        if (m_impl->m_sampleQueue.empty()) {
                            // wait up to a short time for a sample
                            m_impl->m_sampleQueueCv.wait_for(lock, std::chrono::milliseconds(200));
                            if (m_impl->m_sampleQueue.empty()) {
                                // no sample available; notify MFT we can't give input now
                                // optionally continue and let MFT ask again
                                break;
                            }
                        }
                        sample = m_impl->m_sampleQueue.front();
                        geode::log::debug("Popped sample from queue; remaining samples: {}", m_impl->m_sampleQueue.size());
                    }

                    // ProcessInput on encoder thread
                    geode::log::debug("Processing input sample in async loop...");
                    hr = m_impl->encoder->ProcessInput(0, sample.Get(), 0);
                    if (hr == MF_E_NOTACCEPTING) {
                        geode::log::debug("Encoder not accepting input at the moment.");
                        break;
                    }
                    else if (FAILED(hr)) this->m_impl->returnErr(hr, "Failed to process input sample.");

                    {
                        std::unique_lock lock(m_impl->m_sampleQueueMutex);
                        m_impl->m_sampleQueue.pop_front();
                    }
                    break;
                }

                case METransformHaveOutput: {
                    // call ProcessOutput repeatedly until MF_E_TRANSFORM_NEED_MORE_INPUT
                    while (this->m_impl->drain()) {
                        GEODE_UNWRAP(recorder.writePacket(m_currentFrame, m_dts, m_pts, m_denom));

                        // notify consumer thread that a packet is ready
                        m_frameReady.set(true);
                    }
                    break;
                }

                default:
                    // ignore other events or log
                    break;
            } // switch

            geode::log::debug("Releasing MFT event...");

            // free event
            event->Release();
        } // while

        geode::log::debug("MFT event loop terminating.");
        return geode::Ok();
    }

    WMFRecorder::WMFRecorder() : m_impl(std::make_unique<Impl>(this)) {}
    WMFRecorder::~WMFRecorder() = default;
}

#endif
