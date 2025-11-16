#pragma once
#include <functional.hpp>
#include <Geode/platform/platform.hpp>
#include "recorder.hpp"

namespace eclipse::recorder {
    class GLRecorder : public Recorder {
    public:
        void start() override;

        void captureFrame(float width, float height) override;
        geode::Result<> handleRecordThread(ffmpeg::events::Recorder& recorder) override;


        void setCallback(Function<void(std::string const&)>&& callback) { m_callback = std::move(callback); }

    private:
        std::vector<uint8_t> m_currentFrame;
    };
};
