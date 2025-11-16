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

#define WGL_ACCESS_READ_ONLY_NV           0x00000000
#define WGL_ACCESS_READ_WRITE_NV          0x00000001
#define WGL_ACCESS_WRITE_DISCARD_NV       0x00000002

typedef HANDLE (WINAPI * PFNWGLDXOPENDEVICENVPROC) (void *dxDevice);
typedef BOOL (WINAPI * PFNWGLDXCLOSEDEVICENVPROC) (HANDLE hDevice);
typedef HANDLE (WINAPI * PFNWGLDXREGISTEROBJECTNVPROC) (HANDLE hDevice, void *dxObject, GLuint name, GLenum type, GLenum access);
typedef BOOL (WINAPI * PFNWGLDXUNREGISTEROBJECTNVPROC) (HANDLE hDevice, HANDLE hObject);
typedef BOOL (WINAPI * PFNWGLDXLOCKOBJECTSNVPROC) (HANDLE hDevice, GLint count, HANDLE *hObjects);
typedef BOOL (WINAPI * PFNWGLDXUNLOCKOBJECTSNVPROC) (HANDLE hDevice, GLint count, HANDLE *hObjects);

static PFNWGLDXOPENDEVICENVPROC wglDXOpenDeviceNV;
static PFNWGLDXCLOSEDEVICENVPROC wglDXCloseDeviceNV;
static PFNWGLDXREGISTEROBJECTNVPROC wglDXRegisterObjectNV;
static PFNWGLDXUNREGISTEROBJECTNVPROC wglDXUnregisterObjectNV;
static PFNWGLDXLOCKOBJECTSNVPROC wglDXLockObjectsNV;
static PFNWGLDXUNLOCKOBJECTSNVPROC wglDXUnlockObjectsNV;

auto RGBA2NV12 = R"HLSL(
Texture2D<float4> src : register(t0);
RWTexture2D<uint> dstY : register(u0);
RWTexture2D<uint2> dstUV : register(u1);

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
        float rAvg = 0.25*(r + src.Load(int3(x+1, y,0)).r*255 + src.Load(int3(x, y+1,0)).r*255 + src.Load(int3(x+1,y+1,0)).r*255);
        float gAvg = 0.25*(g + src.Load(int3(x+1, y,0)).g*255 + src.Load(int3(x, y+1,0)).g*255 + src.Load(int3(x+1,y+1,0)).g*255);
        float bAvg = 0.25*(b + src.Load(int3(x+1, y,0)).b*255 + src.Load(int3(x, y+1,0)).b*255 + src.Load(int3(x+1,y+1,0)).b*255);

        uint U = uint(clamp(-0.148*rAvg - 0.291*gAvg + 0.439*bAvg + 128, 0, 255));
        uint V = uint(clamp(0.439*rAvg - 0.368*gAvg - 0.071*bAvg + 128, 0, 255));

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

        void init();
        void start();
        void stop();

        void captureFrame();
        void drain();
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
                self->m_callback("Failed to compile HLSL shader: " + errorMsg);
            } else {
                self->m_callback("Failed to compile HLSL shader: Unknown error.");
            }
            return;
        }

        initialized = true;
    }


    void WMFRecorder::Impl::start() {
        if (initialized) {
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
        if (FAILED(hr)) {
            self->m_callback("Failed to create D3D11 device.");
            return;
        }

        hr = MFCreateDXGIDeviceManager(&resetToken, &dxgiManager);
        if (FAILED(hr)) {
            self->m_callback("Failed to create DXGI Device Manager.");
            return;
        }

        hr = dxgiManager->ResetDevice(d3dDevice.Get(), resetToken);
        if (FAILED(hr)) {
            self->m_callback("Failed to reset DXGI Device Manager.");
            return;
        }

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

        MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            &inputType,
            &outputType,
            &ppActivate,
            &ppCount
        );

        ppActivate[0]->ActivateObject(IID_PPV_ARGS(&encoder));
        for (auto i = 0; i < ppCount; i++) ppActivate[i]->Release();
        CoTaskMemFree(ppActivate);

        ComPtr<IMFAttributes> attrs;
        encoder->GetAttributes(&attrs);
        attrs->SetUnknown(MF_SA_D3D11_AWARE, dxgiManager.Get());

        wglDXOpenDeviceNV = (decltype(wglDXOpenDeviceNV))wglGetProcAddress("wglDXOpenDeviceNV");
        if (!wglDXOpenDeviceNV) {
            self->m_callback("Failed to get wglDXOpenDeviceNV function.");
            return;
        }
        wglDXCloseDeviceNV = (decltype(wglDXCloseDeviceNV))wglGetProcAddress("wglDXCloseDeviceNV");
        if (!wglDXCloseDeviceNV) {
            self->m_callback("Failed to get wglDXCloseDeviceNV function.");
            return;
        }

        wglDXRegisterObjectNV = (decltype(wglDXRegisterObjectNV))wglGetProcAddress("wglDXRegisterObjectNV");
        if (!wglDXRegisterObjectNV) {
            self->m_callback("Failed to get wglDXRegisterObjectNV function.");
            return;
        }
        wglDXUnregisterObjectNV = (decltype(wglDXUnregisterObjectNV))wglGetProcAddress("wglDXUnregisterObjectNV");
        if (!wglDXUnregisterObjectNV) {
            self->m_callback("Failed to get wglDXUnregisterObjectNV function.");
            return;
        }

        wglDXLockObjectsNV = (decltype(wglDXLockObjectsNV))wglGetProcAddress("wglDXLockObjectsNV");
        if (!wglDXLockObjectsNV) {
            self->m_callback("Failed to get wglDXLockObjectsNV function.");
            return;
        }
        wglDXUnlockObjectsNV = (decltype(wglDXUnlockObjectsNV))wglGetProcAddress("wglDXUnlockObjectsNV");
        if (!wglDXUnlockObjectsNV) {
            self->m_callback("Failed to get wglDXUnlockObjectsNV function.");
            return;
        }

        // input type, reading from nvdia gpu directly
        ComPtr<IMFMediaType> inType;
        MFCreateMediaType(&inType);
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, self->m_renderSettings.m_width, self->m_renderSettings.m_height);
        MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, self->m_renderSettings.m_fps, 1);
        encoder->SetInputType(0, inType.Get(), 0);

        // output type, h264 till i figure out how to change
        ComPtr<IMFMediaType> outType;
        MFCreateMediaType(&outType);
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, self->m_renderSettings.m_width, self->m_renderSettings.m_height);
        MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, self->m_renderSettings.m_fps, 1);
        encoder->SetOutputType(0, outType.Get(), 0);

        encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        dxDeviceHandle = wglDXOpenDeviceNV(d3dDevice.Get());
        glTexture = self->m_renderTexture.m_texture;

        D3D11_TEXTURE2D_DESC descRgba = {};
        descRgba.ArraySize = 1;
        descRgba.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        descRgba.Width = self->m_renderSettings.m_width;
        descRgba.Height = self->m_renderSettings.m_height;
        descRgba.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        descRgba.MipLevels = 1;
        descRgba.SampleDesc.Count = 1;
        descRgba.Usage = D3D11_USAGE_DEFAULT;
        hr = d3dDevice->CreateTexture2D(&descRgba, nullptr, &rgbaTexture);
        if (FAILED(hr)) {
            self->m_callback("Failed to create RGBA texture.");
            return;
        }

        glDxHandle = wglDXRegisterObjectNV(
            dxDeviceHandle,
            rgbaTexture.Get(),
            glTexture,
            GL_TEXTURE_2D,
            WGL_ACCESS_READ_WRITE_NV
        );

        D3D11_TEXTURE2D_DESC descNV12 = {};
        descNV12.ArraySize = 1;
        descNV12.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        descNV12.Width = self->m_renderSettings.m_width;
        descNV12.Height = self->m_renderSettings.m_height;
        descNV12.Format = DXGI_FORMAT_NV12;
        descNV12.MipLevels = 1;
        descNV12.SampleDesc.Count = 1;
        descNV12.Usage = D3D11_USAGE_DEFAULT;
        hr = d3dDevice->CreateTexture2D(&descNV12, nullptr, &nv12Texture);
        if (FAILED(hr)) {
            self->m_callback("Failed to create NV12 texture.");
            return;
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDescY = {};
        uavDescY.Format = DXGI_FORMAT_R8_UINT;
        uavDescY.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDescY.Texture2D.MipSlice = 0;
        hr = d3dDevice->CreateUnorderedAccessView(nv12Texture.Get(), &uavDescY, &nv12YUAV);
        if (FAILED(hr)) {
            self->m_callback("Failed to create NV12 Y UAV.");
            return;
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDescUV = {};
        uavDescUV.Format = DXGI_FORMAT_R8G8_UINT;
        uavDescUV.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDescUV.Texture2D.MipSlice = 0;
        hr = d3dDevice->CreateUnorderedAccessView(nv12Texture.Get(), &uavDescUV, &nv12UVUAV);

        hr = d3dDevice->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &computeShader);
        if (FAILED(hr)) {
            self->m_callback("Failed to create compute shader.");
            return;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        hr = d3dDevice->CreateShaderResourceView(rgbaTexture.Get(), &srvDesc, &rgbaSRV);
        if (FAILED(hr)) {
            self->m_callback("Failed to create RGBA SRV.");
            return;
        }

        UINT dispatchX = (self->m_renderSettings.m_width + 15) / 16;
        UINT dispatchY = (self->m_renderSettings.m_height + 15) / 16;
        
        ID3D11ShaderResourceView* srvs[1] = { rgbaSRV.Get() };
        ID3D11UnorderedAccessView* uavs[2] = { nv12YUAV.Get(), nv12UVUAV.Get() };

        d3dContext->CSSetShader(computeShader.Get(), nullptr, 0);
        d3dContext->CSSetShaderResources(0, 1, srvs);
        d3dContext->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        d3dContext->Dispatch(dispatchX, dispatchY, 1);

        // Unbind
        ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
        d3dContext->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
        ID3D11ShaderResourceView* nullSRVs[1] = { nullptr };
        d3dContext->CSSetShaderResources(0, 1, nullSRVs);
        d3dContext->CSSetShader(nullptr, nullptr, 0);


        rtStart = 0;
    }

    void WMFRecorder::Impl::stop() {
        if (!initialized) {
            return;
        }

        encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);

        while (true) {
            this->drain();
            auto _ = self->handleFrame();
        
            if (self->m_currentFrame.size() == 0) {
                break;
            }
        }

        encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);

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

        wglDXLockObjectsNV(dxDeviceHandle, 1, &glDxHandle);

        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12Texture.Get(), 0, FALSE, &buffer);

        if (FAILED(hr)) {
            wglDXUnlockObjectsNV(dxDeviceHandle, 1, &glDxHandle);
            self->m_callback("Failed to create DXGI surface buffer.");
            return;
        }

        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buffer.Get());

        sample->SetSampleTime(rtStart);
        sample->SetSampleDuration(10 * 1000 * 1000 / self->m_renderSettings.m_fps);
        rtStart += 10 * 1000 * 1000 / self->m_renderSettings.m_fps;

        hr = encoder->ProcessInput(0, sample.Get(), 0);
        if (FAILED(hr)) {
            wglDXUnlockObjectsNV(dxDeviceHandle, 1, &glDxHandle);
            self->m_callback("Failed to process input sample.");
            return;
        }

        wglDXUnlockObjectsNV(dxDeviceHandle, 1, &glDxHandle);
    }

    void WMFRecorder::Impl::drain() {
        MFT_OUTPUT_STREAM_INFO info;
        encoder->GetOutputStreamInfo(0, &info);

        ComPtr<IMFMediaBuffer> buffer;
        MFCreateMemoryBuffer(info.cbSize, &buffer);

        MFT_OUTPUT_DATA_BUFFER mftOutput;
        mftOutput.dwStreamID = 0;
        mftOutput.pSample = nullptr;
        mftOutput.pEvents = nullptr;
        mftOutput.pSample = nullptr;
        MFCreateSample(&mftOutput.pSample);

        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buffer.Get());
        mftOutput.pSample = sample.Get();

        DWORD status = 0;
        HRESULT hr = encoder->ProcessOutput(0, 1, &mftOutput, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            self->m_currentFrame = std::span<uint8_t>();
            return;
        }
            

        if (SUCCEEDED(hr)) {
            sample->GetSampleTime(&self->m_pts);
            sample->GetSampleDuration(&self->m_dts);
            self->m_denom = 10 * 1000 * 1000;

            size_t curSize = self->m_encodedData.size();
            BYTE* data = nullptr;
            DWORD maxLen = 0, curLen = 0;
            buffer->Lock(&data, &maxLen, &curLen);
            self->m_encodedData.insert(self->m_encodedData.end(), data, data + curLen);
            buffer->Unlock();

            self->m_currentFrame = std::span(self->m_encodedData.begin() + curSize, self->m_encodedData.end());
        }
        else {
            self->m_currentFrame = std::span<uint8_t>();
        }
    }

    void WMFRecorder::start() {
        if (m_recording) return; 

        m_impl->start();
        Recorder::start();
    }

    void WMFRecorder::stop() {
        if (!m_recording) return;

        Recorder::stop();
        m_impl->stop();

        m_encodedData.clear();
        m_currentFrame = std::span<uint8_t>();
    }

    void WMFRecorder::captureFrame(float width, float height) {
        if (!m_recording) return;

        m_impl->captureFrame();
        m_encodedData.clear();
        
        m_impl->drain();
    }

    geode::Result<> WMFRecorder::handleFrame() {
        return m_ffmpegRecorder.writePacket(m_currentFrame, m_dts, m_pts, m_denom);
    }

    WMFRecorder::WMFRecorder() : m_impl(std::make_unique<Impl>(this)) {}
    WMFRecorder::~WMFRecorder() = default;
}

#endif
