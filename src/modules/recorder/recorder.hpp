#pragma once
#include <functional.hpp>
#include <Geode/platform/platform.hpp>
#include "spinlock.hpp"
#include "ffmpeg-api/events.hpp"
#include "rendertexture.hpp"

namespace eclipse::recorder {
    class Recorder {
    public:
        virtual ~Recorder() = default;

        virtual void start();
        virtual void stop();

        virtual geode::Result<> handleRecordThread(ffmpeg::events::Recorder& recorder) = 0;

        virtual void visitFrame() = 0;

        bool isRecording() const { return m_recording; }
        std::string getRecordingDuration() const;

        void setCallback(Function<void(std::string const&)>&& callback) { m_callback = std::move(callback); }

        static std::vector<std::string> getAvailableCodecs();

        ffmpeg::RenderSettings m_renderSettings{};

    protected:
        void setupProjection();

        void recordThread();

        volatile bool m_recording = false;
        utils::spinlock m_frameReady;
        uint64_t m_recordingDuration = 0;
        cocos2d::CCDirectorDelegate* m_projectionDelegate = nullptr;
        RenderTexture m_renderTexture{};

        Function<void(std::string const&)> m_callback;
    };
};
