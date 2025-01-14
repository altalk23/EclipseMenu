#include <modules/config/config.hpp>
#include <modules/gui/gui.hpp>
#include <modules/gui/components/float-toggle.hpp>
#include <modules/gui/components/toggle.hpp>
#include <modules/hack/hack.hpp>

#include <Geode/modify/FMODAudioEngine.hpp>

namespace eclipse::hacks::Global {
    // void setSpeed(FMOD::Channel* channel) {
    //     float speed = 1.f;
    //
    //     if (config::get<bool>("global.audiospeed.sync", false)) {
    //         bool speedhack = config::get<bool>("global.speedhack.toggle", false);
    //         speed = speedhack ? config::get<float>("global.speedhack", 1.f) : 1.f;
    //     }
    //
    //     if (config::get<bool>("global.audiospeed.toggle", false))
    //         speed = config::get<float>("global.audiospeed", 1.f);
    //
    //     FMOD::Sound* sound;
    //     channel->getCurrentSound(&sound);
    //     float freq;
    //     sound->getDefaults(&freq, nullptr);
    //     channel->setFrequency(freq * speed);
    // }
    //
    // void updateChannels() {
    //     FMOD::Channel* audioChannel;
    //     FMOD::System* system = utils::get<FMODAudioEngine>()->m_system;
    //
    //     for (auto i = 0; i < 4; i++) {
    //         system->getChannel(126 + i, &audioChannel);
    //         if (audioChannel)
    //             setSpeed(audioChannel);
    //     }
    // }

    class $hack(AudioSpeed) {
        void init() override {
            auto tab = gui::MenuTab::find("tab.global");

            config::setIfEmpty("global.audiospeed.toggle", false);
            config::setIfEmpty("global.audiospeed", 1.f);

            tab->addFloatToggle("global.audiospeed", 0.0001f, 1000.f, "%.4f")->handleKeybinds();
            tab->addToggle("global.audiospeed.sync")->handleKeybinds()->setDescription();
        }

        [[nodiscard]] const char* getId() const override { return "Audio Speed"; }
        [[nodiscard]] int32_t getPriority() const override { return -8; }

        bool m_lastSpeedhackState = false;
    };

    REGISTER_HACK(AudioSpeed)

    class $modify(AudioSpeedFMODHook, FMODAudioEngine) {
        static bool shouldUpdate() {
            static bool lastSpeedhack = false;

            CACHE_CONFIG_BOOL(speedhack, "global.speedhack.toggle");
            CACHE_CONFIG_BOOL(audiospeed, "global.audiospeed.toggle");
            CACHE_CONFIG_BOOL(syncWithSpeedhack, "global.audiospeed.sync");

            bool toggleSpeedhack = lastSpeedhack != speedhack;
            lastSpeedhack = speedhack;

            return (speedhack || toggleSpeedhack) && syncWithSpeedhack || audiospeed;
        }

        static float getSpeed() {
            CACHE_CONFIG_BOOL(audiospeedToggle, "global.audiospeed.toggle");
            if (audiospeedToggle) {
                CACHE_CONFIG(float, audiospeed, "global.audiospeed", 1.f);
                return audiospeed;
            }

            CACHE_CONFIG_BOOL(syncWithSpeedhack, "global.audiospeed.sync");
            if (syncWithSpeedhack) {
                CACHE_CONFIG(float, speedhack, "global.speedhack", 1.f);
                CACHE_CONFIG_BOOL(speedhackToggle, "global.speedhack.toggle");
                return speedhackToggle ? speedhack : 1.f;
            }

            return 1.f;
        }

        void update(float dt) override {
            FMODAudioEngine::update(dt);
            if (!shouldUpdate()) return;

            FMOD::ChannelGroup* masterGroup;
            if (m_system->getMasterChannelGroup(&masterGroup) != FMOD_OK) return;
            masterGroup->setPitch(getSpeed());
        }
    };
}
