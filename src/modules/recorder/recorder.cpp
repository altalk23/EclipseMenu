#include "recorder.hpp"

#include <utils.hpp>
#include <modules/recorder/DSPRecorder.hpp>
#include <modules/debug/benchmark.hpp>

namespace eclipse::recorder {
    namespace ffmpeg = ffmpeg::events;


    class ProjectionDelegate : public cocos2d::CCDirectorDelegate {
        void updateProjection() override {
            kmGLMatrixMode(KM_GL_PROJECTION);
            kmGLLoadIdentity();
            kmMat4 orthoMatrix;
            auto size = utils::get<cocos2d::CCDirector>()->m_obWinSizeInPoints;
            kmMat4OrthographicProjection(&orthoMatrix, 0, size.width, size.height, 0, -1024, 1024);
            kmGLMultMatrix(&orthoMatrix);
            kmGLMatrixMode(KM_GL_MODELVIEW);
            kmGLLoadIdentity();
        }
    };

    void Recorder::setupProjection() {
        if (!m_projectionDelegate) {
            m_projectionDelegate = new ProjectionDelegate();
        }
        utils::get<cocos2d::CCDirector>()->setDelegate(m_projectionDelegate);
    }

    void Recorder::start() {
        m_renderTexture = RenderTexture(m_renderSettings.m_width, m_renderSettings.m_height);
        m_renderTexture.begin();

        m_recording = true;

        DSPRecorder::get()->start();

        this->setupProjection();
    }

    void Recorder::stop() {
        if (!m_recording) return;

        m_recording = false;

        // make sure to let the recording thread know that we're stopping
        m_frameReady.set(true);

        m_renderTexture.end();
        DSPRecorder::get()->stop();

        auto director = utils::get<cocos2d::CCDirector>();
        if (auto& delegate = utils::get<cocos2d::CCDirector>()->m_pProjectionDelegate) {
            delete delegate;
            delegate = nullptr;
        }

        director->setProjection(cocos2d::ccDirectorProjection::kCCDirectorProjection2D);
    }

    void Recorder::recordThread() {
        geode::utils::thread::setName("Eclipse Recorder Thread");
        geode::log::debug("Recorder thread started.");

        ffmpeg::Recorder ffmpegRecorder;
        if (!ffmpegRecorder.isValid()) {
            stop();
            m_callback("Failed to initialize ffmpeg recorder.");
            geode::log::debug("Recorder thread stopped.");
            return;
        }

        auto res = ffmpegRecorder.init(m_renderSettings);
        if (res.isErr()) {
            stop();
            m_callback(res.unwrapErr());
            ffmpegRecorder.stop();
            m_frameReady.set(false); // unlock the main thread if it's waiting
            geode::log::debug("Recorder thread stopped.");
            return;
        }

        // wait for the first frame
        m_frameReady.set(false);
        m_frameReady.wait_for(true);

        {
            // record the time it took to record the video (to show in the end)
            debug::Timer timer("Recording", &m_recordingDuration);
            
            res = this->handleRecordThread(ffmpegRecorder);
            if (res.isErr()) {
                m_callback(res.unwrapErr());

                // stop recording if an error occurred
                this->stop();
            }
        }

        geode::log::debug("Recorder thread stopped.");

        ffmpegRecorder.stop();

        DSPRecorder::get()->stop();
        auto data = DSPRecorder::get()->getData();

        std::filesystem::path tempPath = m_renderSettings.m_outputFile.parent_path() / "music.mp4";

        res = ffmpeg::AudioMixer::mixVideoRaw(m_renderSettings.m_outputFile, data, tempPath);
        if (res.isErr()) return m_callback(res.unwrapErr());

        std::error_code ec;
        std::filesystem::remove(m_renderSettings.m_outputFile, ec);
        if (ec) return m_callback("Failed to remove old video file.");

        ec = {};
        std::filesystem::rename(tempPath, m_renderSettings.m_outputFile, ec);
        if (ec) return m_callback("Failed to rename temporary video file.");
    }

    std::string Recorder::getRecordingDuration() const {
        // m_recordingDuration is in nanoseconds
        double inSeconds = m_recordingDuration / 1'000'000'000.0;
        return utils::formatTime(inSeconds);
    }

    std::vector<std::string> Recorder::getAvailableCodecs() {
        return ffmpeg::Recorder::getAvailableCodecs();
    }
}
