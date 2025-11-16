#pragma once
#include <functional.hpp>
#include <Geode/platform/platform.hpp>
#include "recorder.hpp"

namespace eclipse::recorder {
    class WMFRecorder : public Recorder {
    public:
        WMFRecorder();
        ~WMFRecorder();
        void start() override;
        void stop() override;

        geode::Result<> handleRecordThread(ffmpeg::events::Recorder& recorder) override;
        void visitFrame() override;

        void setCallback(Function<void(std::string const&)>&& callback) { m_callback = std::move(callback); }

    private:
        std::vector<uint8_t> m_encodedData;
        int64_t m_pts = 0;
        int64_t m_dts = 0;
        int64_t m_denom = 0;
        std::span<uint8_t> m_currentFrame;

        class Impl;
        std::unique_ptr<Impl> m_impl;
    };
};
