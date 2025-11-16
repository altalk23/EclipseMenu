#include "WMFRecorder.hpp"
#include "modules/recorder/recorder.hpp"
#include <cstdint>
#include <dxgiformat.h>

#ifdef GEODE_IS_WINDOWS
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <d3d11.h>
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

// #define WGL_ACCESS_READ_ONLY_NV           0x00000000
// #define WGL_ACCESS_READ_WRITE_NV          0x00000001
// #define WGL_ACCESS_WRITE_DISCARD_NV       0x00000002

// typedef HANDLE (WINAPI * PFNWGLDXOPENDEVICENVPROC) (void *dxDevice);
// typedef BOOL (WINAPI * PFNWGLDXCLOSEDEVICENVPROC) (HANDLE hDevice);
// typedef HANDLE (WINAPI * PFNWGLDXREGISTEROBJECTNVPROC) (HANDLE hDevice, void *dxObject, GLuint name, GLenum type, GLenum access);
// typedef BOOL (WINAPI * PFNWGLDXUNREGISTEROBJECTNVPROC) (HANDLE hDevice, HANDLE hObject);
// typedef BOOL (WINAPI * PFNWGLDXLOCKOBJECTSNVPROC) (HANDLE hDevice, GLint count, HANDLE *hObjects);
// typedef BOOL (WINAPI * PFNWGLDXUNLOCKOBJECTSNVPROC) (HANDLE hDevice, GLint count, HANDLE *hObjects);

// static PFNWGLDXOPENDEVICENVPROC wglDXOpenDeviceNV;
// static PFNWGLDXCLOSEDEVICENVPROC wglDXCloseDeviceNV;
// static PFNWGLDXREGISTEROBJECTNVPROC wglDXRegisterObjectNV;
// static PFNWGLDXUNREGISTEROBJECTNVPROC wglDXUnregisterObjectNV;
// static PFNWGLDXLOCKOBJECTSNVPROC wglDXLockObjectsNV;
// static PFNWGLDXUNLOCKOBJECTSNVPROC wglDXUnlockObjectsNV;

auto RGBA2NV12 = R"HLSL(
Texture2D<float4> src : register(t0);
RWTexture2D<uint1> dstY : register(u0);
RWTexture2D<uint2> dstUV : register(u1);
cbuffer FrameSize : register(b0)
{
    uint4 width;
    uint4 height;
    uint4 padding[2];
};

[numthreads(16,16,1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    uint x = DTid.x;
    uint y = DTid.y;

    float4 rgba = src.Load(int3(x, y, 0));
    float r = rgba.r * 255;
    float g = rgba.g * 255;
    float b = rgba.b * 255;

    uint Y = uint(clamp(0.257*r + 0.504*g + 0.098*b + 16, 0, 255));
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

        uint U = uint(clamp(-0.147*rAvg - 0.289*gAvg + 0.436*bAvg + 128.0, 0.0, 255.0));
        uint V = uint(clamp(0.615*rAvg - 0.515*gAvg - 0.100*bAvg + 128.0, 0.0, 255.0));

        dstUV[int2(x/2, y/2)] = uint2(U, V);
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
        ComPtr<ID3D11Device> d3dDevice;
        ComPtr<ID3D11DeviceContext> d3dContext;
        ComPtr<IMFDXGIDeviceManager> dxgiManager;
        UINT resetToken = 0;

        bool initialized = false;
        bool started = false;

        HANDLE dxDeviceHandle;
        HANDLE glDxHandle;
        GLuint glTexture;
        ComPtr<ID3D11Texture2D> rgbaTexture;
        ComPtr<ID3D11Texture2D> nv12Texture;
        ComPtr<ID3D11UnorderedAccessView> nv12YUAV;
        ComPtr<ID3D11UnorderedAccessView> nv12UVUAV;
        ComPtr<ID3D11ComputeShader> computeShader;
        ComPtr<ID3D11ShaderResourceView> rgbaSRV;
        ComPtr<ID3DBlob> csBlob;
        ComPtr<ID3DBlob> errorBlob;

        std::thread m_mftEventThread;
        std::atomic_bool m_mftThreadRunning{false};

        std::mutex m_sampleQueueMutex;
        std::condition_variable m_sampleQueueCv;
        std::deque<ComPtr<IMFSample>> m_sampleQueue;

        ComPtr<IMFMediaEventGenerator> m_eventGenerator;

        void init();
        void start();
        void stop();

        void captureFrame();
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

        // wglDXOpenDeviceNV = (decltype(wglDXOpenDeviceNV))wglGetProcAddress("wglDXOpenDeviceNV");
        // if (!wglDXOpenDeviceNV) {
        //     self->m_callback("Failed to get wglDXOpenDeviceNV function.");
        //     return;
        // }
        // wglDXCloseDeviceNV = (decltype(wglDXCloseDeviceNV))wglGetProcAddress("wglDXCloseDeviceNV");
        // if (!wglDXCloseDeviceNV) {
        //     self->m_callback("Failed to get wglDXCloseDeviceNV function.");
        //     return;
        // }

        // wglDXRegisterObjectNV = (decltype(wglDXRegisterObjectNV))wglGetProcAddress("wglDXRegisterObjectNV");
        // if (!wglDXRegisterObjectNV) {
        //     self->m_callback("Failed to get wglDXRegisterObjectNV function.");
        //     return;
        // }
        // wglDXUnregisterObjectNV = (decltype(wglDXUnregisterObjectNV))wglGetProcAddress("wglDXUnregisterObjectNV");
        // if (!wglDXUnregisterObjectNV) {
        //     self->m_callback("Failed to get wglDXUnregisterObjectNV function.");
        //     return;
        // }

        // wglDXLockObjectsNV = (decltype(wglDXLockObjectsNV))wglGetProcAddress("wglDXLockObjectsNV");
        // if (!wglDXLockObjectsNV) {
        //     self->m_callback("Failed to get wglDXLockObjectsNV function.");
        //     return;
        // }
        // wglDXUnlockObjectsNV = (decltype(wglDXUnlockObjectsNV))wglGetProcAddress("wglDXUnlockObjectsNV");
        // if (!wglDXUnlockObjectsNV) {
        //     self->m_callback("Failed to get wglDXUnlockObjectsNV function.");
        //     return;
        // }

        initialized = true;
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
        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &d3dDevice,
            &featureLevel,
            &d3dContext
        );
        if (FAILED(hr)) return this->returnErr(hr, "Failed to create D3D11 device.");

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

        D3D11_TEXTURE2D_DESC descRgba = {};
        descRgba.ArraySize = 1;
        descRgba.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
        descRgba.Width = self->m_renderSettings.m_width;
        descRgba.Height = self->m_renderSettings.m_height;
        descRgba.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // assumes cocos2d using rgba8, i hope it does
        descRgba.MipLevels = 1;
        descRgba.SampleDesc.Count = 1;
        descRgba.Usage = D3D11_USAGE_DEFAULT;
        hr = d3dDevice->CreateTexture2D(&descRgba, nullptr, &rgbaTexture);
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

        D3D11_TEXTURE2D_DESC descNV12 = {};
        descNV12.ArraySize = 1;
        descNV12.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
        descNV12.Width = self->m_renderSettings.m_width;
        descNV12.Height = self->m_renderSettings.m_height;
        descNV12.Format = DXGI_FORMAT_NV12;
        descNV12.MipLevels = 1;
        descNV12.SampleDesc.Count = 1;
        descNV12.Usage = D3D11_USAGE_DEFAULT;
        hr = d3dDevice->CreateTexture2D(&descNV12, nullptr, &nv12Texture);
        if (FAILED(hr)) return this->returnErr(hr, "Failed to create NV12 texture.");

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDescY = {};
        uavDescY.Format = DXGI_FORMAT_R8_UNORM;
        uavDescY.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDescY.Texture2D.MipSlice = 0;
        hr = d3dDevice->CreateUnorderedAccessView(nv12Texture.Get(), &uavDescY, &nv12YUAV);
        if (FAILED(hr)) return this->returnErr(hr, "Failed to create NV12 Y UAV.");

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDescUV = {};
        uavDescUV.Format = DXGI_FORMAT_R8G8_UNORM;
        uavDescUV.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDescUV.Texture2D.MipSlice = 0;
        hr = d3dDevice->CreateUnorderedAccessView(nv12Texture.Get(), &uavDescUV, &nv12UVUAV);
        if (FAILED(hr)) return this->returnErr(hr, "Failed to create NV12 UV UAV.");

        hr = d3dDevice->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &computeShader);
        if (FAILED(hr)) return this->returnErr(hr, "Failed to create compute shader.");

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        hr = d3dDevice->CreateShaderResourceView(rgbaTexture.Get(), &srvDesc, &rgbaSRV);
        if (FAILED(hr)) return this->returnErr(hr, "Failed to create RGBA SRV.");

        struct FrameSizeCB {
            UINT width;
            UINT height;
            UINT padding[2];
        };

        D3D11_BUFFER_DESC cbd = {};
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.ByteWidth = sizeof(FrameSizeCB);
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = 0;

        FrameSizeCB cb = { self->m_renderSettings.m_width, self->m_renderSettings.m_height };
        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = &cb;

        ComPtr<ID3D11Buffer> frameSizeBuffer;
        hr = d3dDevice->CreateBuffer(&cbd, &initData, &frameSizeBuffer);
        if (FAILED(hr)) return this->returnErr(hr, "Failed to create frame size constant buffer.");

        UINT dispatchX = (self->m_renderSettings.m_width + 15) / 16;
        UINT dispatchY = (self->m_renderSettings.m_height + 15) / 16;
        ID3D11ShaderResourceView* srvs[1] = { rgbaSRV.Get() };
        ID3D11UnorderedAccessView* uavs[2] = { nv12YUAV.Get(), nv12UVUAV.Get() };
        ID3D11Buffer* cbuffers[1] = { frameSizeBuffer.Get() };
        d3dContext->CSSetShader(computeShader.Get(), nullptr, 0);
        d3dContext->CSSetShaderResources(0, 1, srvs);
        d3dContext->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        d3dContext->CSSetConstantBuffers(0, 1, cbuffers);
        d3dContext->Dispatch(dispatchX, dispatchY, 1);

        ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
        ID3D11ShaderResourceView* nullSRVs[1] = { nullptr };
        ID3D11Buffer* nullCBuffers[1] = { nullptr };
        d3dContext->CSSetShader(nullptr, nullptr, 0);
        d3dContext->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
        d3dContext->CSSetShaderResources(0, 1, nullSRVs);
        d3dContext->CSSetConstantBuffers(0, 1, nullCBuffers);
        rtStart = 0;

        encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        encoder.As(&m_eventGenerator);
        if (!m_eventGenerator) {
            self->m_callback("Encoder does not support media event generator interface.");
            return;
        }

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
        nv12YUAV.Reset();
        nv12UVUAV.Reset();
        computeShader.Reset();
        rgbaSRV.Reset();
        glTexture = 0;
        encoder.Reset();
        dxgiManager.Reset();
        d3dContext.Reset();
        d3dDevice.Reset();

        MFShutdown();
    }

    void WMFRecorder::Impl::captureFrame() {
        if (!initialized) {
            self->m_callback("WMFRecorder not initialized properly.");
            return;
        }
        if (!started) {
            return;
        }

        wglDXLockObjectsNV(dxDeviceHandle, 1, &glDxHandle);

        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12Texture.Get(), 0, FALSE, &buffer);
        if (FAILED(hr)) return this->returnErrUnlock(hr, "Failed to create DXGI surface buffer.");

        ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) return this->returnErrUnlock(hr, "Failed to create sample.");

        hr = sample->AddBuffer(buffer.Get());
        if (FAILED(hr)) return this->returnErrUnlock(hr, "Failed to add buffer to sample.");

        hr = sample->SetSampleTime(rtStart);
        if (FAILED(hr)) return this->returnErrUnlock(hr, "Failed to set sample time.");

        hr = sample->SetSampleDuration(10 * 1000 * 1000 / self->m_renderSettings.m_fps);
        if (FAILED(hr)) return this->returnErrUnlock(hr, "Failed to set sample duration.");

        if (rtStart == 0) {
            ComPtr<ID3D11Texture2D> staging;
            D3D11_TEXTURE2D_DESC desc;
            rgbaTexture->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.BindFlags = 0;
            desc.MiscFlags = 0; 
            d3dDevice->CreateTexture2D(&desc, nullptr, &staging);
            d3dContext->CopyResource(staging.Get(), rgbaTexture.Get());
            D3D11_MAPPED_SUBRESOURCE mapped;
            d3dContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
            auto ptr = mapped.pData;

            std::ofstream file(geode::dirs::getGeodeDir() / "frame.rgba", std::ios::binary);
            std::vector<uint8_t> data(self->m_renderSettings.m_width * self->m_renderSettings.m_height * 4);

            glPixelStorei(GL_PACK_ALIGNMENT, 1);

            // Read pixels from the current framebuffer
            glReadPixels(0, 0, self->m_renderSettings.m_width, self->m_renderSettings.m_height, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
            file.write(reinterpret_cast<char*>(ptr), self->m_renderSettings.m_width * self->m_renderSettings.m_height * 4);

            // for (UINT row = 0; row < desc.Height; ++row) {
            //     // Each row: mapped.RowPitch bytes, but only Width*4 are actual pixel data
            //     file.write(reinterpret_cast<char*>(mapped.pData) + row * mapped.RowPitch, desc.Width * 4);
            // }
            // // write Y plane
            // for (UINT row = 0; row < desc.Height; ++row) {
            //     file.write(reinterpret_cast<char*>(static_cast<BYTE*>(mapped.pData) + row * mapped.RowPitch), desc.Width);
            // }

            // // write UV plane (half height)
            // BYTE* uvBase = static_cast<BYTE*>(mapped.pData) + mapped.RowPitch * desc.Height;
            // for (UINT row = 0; row < desc.Height / 2; ++row) {
            //     file.write(reinterpret_cast<char*>(uvBase + row * mapped.RowPitch), desc.Width);
            // }
            file.close();

            d3dContext->Unmap(staging.Get(), 0);
        }

        rtStart += 10 * 1000 * 1000 / self->m_renderSettings.m_fps;

        {
            std::lock_guard lock(m_sampleQueueMutex);
            m_sampleQueue.push_back(sample);
        }
        m_sampleQueueCv.notify_one();

        wglDXUnlockObjectsNV(dxDeviceHandle, 1, &glDxHandle);
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
    }

    void WMFRecorder::stop() {
        if (!m_impl->initialized || !m_recording) return;

        Recorder::stop();
        m_impl->stop();

        m_encodedData.clear();
        m_currentFrame = std::span<uint8_t>();
    }

    void WMFRecorder::captureFrame(float width, float height) {
        if (!m_impl->initialized || !m_recording) return;

        m_impl->captureFrame();
        m_encodedData.clear();
    }

    void WMFRecorder::visitFrame() {
        // don't capture if we're not recording
        if (!m_recording) return;

        m_renderTexture.capture(utils::get<PlayLayer>(), m_frameReady, [&](float width, float height) {
            this->captureFrame(width, height);
        });
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
