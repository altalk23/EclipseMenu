#include "GLRecorder.hpp"

#include <memory>
#include <thread>
#include <utility>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/loader/Log.hpp>
#include <Geode/utils/general.hpp>
#include <modules/config/config.hpp>
#include <modules/debug/benchmark.hpp>
#include <modules/recorder/DSPRecorder.hpp>
#include <modules/utils/SingletonCache.hpp>
#include <utils.hpp>

namespace eclipse::recorder {
    namespace ffmpeg = ffmpeg::events;

    void GLRecorder::start() {
        m_currentFrame.resize(m_renderSettings.m_width * m_renderSettings.m_height * 4, 0);
        Recorder::start();
    }

    void GLRecorder::captureFrame(float width, float height) {
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, m_currentFrame.data());
    }

    geode::Result<> GLRecorder::handleRecordThread(ffmpeg::Recorder& recorder) {
        while (m_recording) {
            geode::log::debug("Processing frame...");
            GEODE_UNWRAP(recorder.writeFrame(m_currentFrame));
            geode::log::debug("Frame processed.");
            

            // break if we're not recording anymore (to avoid waiting forever)
            if (!m_recording) break;

            m_frameReady.set(false);
            m_frameReady.wait_for(true);
        }
        return geode::Ok();
    }
}
