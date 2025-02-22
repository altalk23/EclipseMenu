#include "blur.hpp"

#include <modules/config/config.hpp>
#include <modules/gui/theming/manager.hpp>

// COMPLETELY stolen from cgytrus/SimplePatchLoader (with permission)

#ifdef GEODE_IS_DESKTOP

#include <Geode/loader/Setting.hpp>
#include <Geode/modify/CCEGLViewProtocol.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/CCNode.hpp>

#include <modules/gui/cocos/cocos.hpp>
#include <imgui-cocos.hpp>
#include <utils.hpp>
#include <numbers>
#include <memory>

namespace eclipse::gui::blur {

    RenderTexture ppRt0;
    RenderTexture ppRt1;
    GLuint ppVao = 0;
    GLuint ppVbo = 0;
    Shader ppShader;
    GLint ppShaderFast = 0;
    GLint ppShaderFirst = 0;
    GLint ppShaderRadius = 0;

    float blurTimer = 0.f;
    float blurProgress = 0.f;

    geode::Patch* visitVtablePatch = nullptr;

    geode::Result<std::string> Shader::compile(const std::filesystem::path& vertexPath, const std::filesystem::path& fragmentPath) {
        auto vertexSource = geode::utils::file::readString(vertexPath);

        if (!vertexSource)
            return geode::Err("failed to read vertex shader at path {}: {}", vertexPath.string(), vertexSource.unwrapErr());

        auto fragmentSource = geode::utils::file::readString(fragmentPath);
        if (!fragmentSource)
            return geode::Err("failed to read fragment shader at path {}: {}", fragmentPath.string(), fragmentSource.unwrapErr());

        auto getShaderLog = [](GLuint id) -> std::string {
            GLint length, written;
            glGetShaderiv(id, GL_INFO_LOG_LENGTH, &length);

            if (length <= 0)
                return "";

            auto stuff = std::make_unique<char[]>(length + 1);
            glGetShaderInfoLog(id, length, &written, stuff.get());
            std::string result(stuff.get());

            return result;
        };
        GLint res;

        vertex = glCreateShader(GL_VERTEX_SHADER);
        auto oglSucks = vertexSource.unwrap();
        auto oglSucksP = oglSucks.c_str();
        glShaderSource(vertex, 1, &oglSucksP, nullptr);
        glCompileShader(vertex);
        auto vertexLog = getShaderLog(vertex);

        glGetShaderiv(vertex, GL_COMPILE_STATUS, &res);
        if (!res) {
            glDeleteShader(vertex);
            vertex = 0;
            return geode::Err("vertex shader compilation failed:\n{}", vertexLog);
        }

        fragment = glCreateShader(GL_FRAGMENT_SHADER);
        oglSucks = fragmentSource.unwrap();
        oglSucksP = oglSucks.c_str();
        glShaderSource(fragment, 1, &oglSucksP, nullptr);
        glCompileShader(fragment);
        auto fragmentLog = getShaderLog(fragment);

        glGetShaderiv(fragment, GL_COMPILE_STATUS, &res);
        if (!res) {
            glDeleteShader(vertex);
            glDeleteShader(fragment);
            vertex = 0;
            fragment = 0;
            return geode::Err("fragment shader compilation failed:\n{}", fragmentLog);
        }

        program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);

        return geode::Ok(fmt::format(
            "shader compilation successful. logs:\nvert:\n{}\nfrag:\n{}",
            vertexLog, fragmentLog
        ));
    }

    geode::Result<std::string> Shader::link() {
        if (!vertex)
            return geode::Err("vertex shader not compiled");
        if (!fragment)
            return geode::Err("fragment shader not compiled");

        auto getProgramLog = [](GLuint id) -> std::string {
            GLint length, written;
            glGetProgramiv(id, GL_INFO_LOG_LENGTH, &length);

            if (length <= 0)
                return "";

            auto stuff = std::make_unique<char[]>(length + 1);
            glGetProgramInfoLog(id, length, &written, stuff.get());
            std::string result(stuff.get());

            return result;
        };
        GLint res;

        glLinkProgram(program);
        auto programLog = getProgramLog(program);

        glDeleteShader(vertex);
        glDeleteShader(fragment);
        vertex = 0;
        fragment = 0;

        glGetProgramiv(program, GL_LINK_STATUS, &res);
        if (!res) {
            glDeleteProgram(program);
            program = 0;
            return geode::Err("shader link failed:\n{}", programLog);
        }

        return geode::Ok(fmt::format("shader link successful. log:\n{}", programLog));
    }

    void Shader::cleanup() {
        if (program)
            glDeleteProgram(program);

        program = 0;
    }

    void RenderTexture::setup(GLsizei width, GLsizei height) {
        GLint drawFbo = 0;
        GLint readFbo = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);

        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);

        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

        glGenRenderbuffers(1, &rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            geode::log::error("pp fbo not complete, uh oh! i guess i will have to cut off ur pp now");

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
    }

    void RenderTexture::resize(GLsizei width, GLsizei height) const {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);

        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    }

    void RenderTexture::cleanup() {
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (tex) glDeleteTextures(1, &tex);
        if (rbo) glDeleteRenderbuffers(1, &rbo);
        fbo = 0;
        tex = 0;
        rbo = 0;
    }

    void setupPostProcess() {
        if (utils::shouldUseLegacyDraw()) return;

        auto size = utils::get<cocos2d::CCEGLView>()->getFrameSize() * geode::utils::getDisplayFactor();

        ppRt0.setup((GLsizei)size.width, (GLsizei)size.height);
        ppRt1.setup((GLsizei)size.width, (GLsizei)size.height);

        GLfloat ppVertices[] = {
            // positions   // texCoords
            -1.0f,  1.0f,  0.0f, 1.0f,
            -1.0f, -1.0f,  0.0f, 0.0f,
             1.0f, -1.0f,  1.0f, 0.0f,

            -1.0f,  1.0f,  0.0f, 1.0f,
             1.0f, -1.0f,  1.0f, 0.0f,
             1.0f,  1.0f,  1.0f, 1.0f
        };
        glGenVertexArrays(1, &ppVao);
        glGenBuffers(1, &ppVbo);
        glBindVertexArray(ppVao);
        glBindBuffer(GL_ARRAY_BUFFER, ppVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(ppVertices), &ppVertices, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*)nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*)(2 * sizeof(GLfloat)));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        auto vertexPath = std::string{ utils::get<cocos2d::CCFileUtils>()->fullPathForFilename("pp-vert.glsl"_spr, false) };
        auto fragmentPath = std::string{ utils::get<cocos2d::CCFileUtils>()->fullPathForFilename("pp-frag.glsl"_spr, false) };

        auto res = ppShader.compile(vertexPath, fragmentPath);
        if (!res) return geode::log::error("Failed to compile shader: {}", res.unwrapErr());
        // log::info("{}", res.unwrap());

        glBindAttribLocation(ppShader.program, 0, "aPosition");
        glBindAttribLocation(ppShader.program, 1, "aTexCoords");

        auto res2 = ppShader.link();
        if (!res2) return geode::log::error("Failed to link shader: {}", res2.unwrapErr());
        // log::info("{}", res.unwrap());

        cocos2d::ccGLUseProgram(ppShader.program);
        glUniform1i(glGetUniformLocation(ppShader.program, "screen"), 0);
        glUniform2f(glGetUniformLocation(ppShader.program, "screenSize"), size.width, size.height);
        ppShaderFast = glGetUniformLocation(ppShader.program, "fast");
        ppShaderFirst = glGetUniformLocation(ppShader.program, "first");
        ppShaderRadius = glGetUniformLocation(ppShader.program, "radius");
    }

    void cleanupPostProcess() {
        ppRt0.cleanup();
        ppRt1.cleanup();

        if (ppVao)
            glDeleteVertexArrays(1, &ppVao);
        if (ppVbo)
            glDeleteBuffers(1, &ppVbo);
        ppVao = 0;
        ppVbo = 0;

        ppShader.cleanup();
        ppShaderFast = 0;
        ppShaderFirst = 0;
        ppShaderRadius = 0;
    }

    // hook time yippee

    void CCSceneVisit(cocos2d::CCScene* self) {
        if (ppShader.program == 0 || blurProgress == 0.f)
            return self->CCNode::visit();

        float blur = 0.05f * (1.f - std::cos(static_cast<float>(std::numbers::pi) * blurProgress)) * 0.5f;
        if (blur == 0.f)
            return self->CCNode::visit();

        GLint drawFbo = 0;
        GLint readFbo = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);

        glBindFramebuffer(GL_FRAMEBUFFER, ppRt0.fbo);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        self->CCNode::visit();

        glBindVertexArray(ppVao);
        cocos2d::ccGLUseProgram(ppShader.program);
        glUniform1i(ppShaderFast, true);
        glUniform1f(ppShaderRadius, blur);

        glBindFramebuffer(GL_FRAMEBUFFER, ppRt1.fbo);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        glUniform1i(ppShaderFirst, GL_TRUE);
        glBindTexture(GL_TEXTURE_2D, ppRt0.tex);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);

        glUniform1i(ppShaderFirst, GL_FALSE);
        glBindTexture(GL_TEXTURE_2D, ppRt1.tex);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindVertexArray(0);
    }

    template <typename T>
    std::vector<uint8_t> toBytes(T value) {
        std::vector<uint8_t> bytes(sizeof(T));
        std::memcpy(bytes.data(), &value, sizeof(T));
        return bytes;
    }

    void hookCCSceneVisit() {
        auto virtualOffset = geode::addresser::getVirtualOffset(&cocos2d::CCScene::visit);

        // find the vtable address
        auto tmpScene = new cocos2d::CCScene();
        auto vtable = *reinterpret_cast<uintptr_t*>(tmpScene);
        delete tmpScene;

        // find the visit function entry in the vtable
        auto visitEntry = reinterpret_cast<uintptr_t*>(vtable + virtualOffset);
        uintptr_t hookAddr = reinterpret_cast<uintptr_t>(&CCSceneVisit);
        auto hookAddrBytes = toBytes(hookAddr);

        // write the hook address to the vtable
        auto res = geode::Mod::get()->patch(visitEntry, hookAddrBytes);
        if (!res) {
            geode::log::error("Failed to hook CCScene::visit: {}", res.unwrapErr());
        } else {
            visitVtablePatch = res.unwrap();
            geode::log::debug("Enabled virtual cocos2d::CCScene::visit hook at 0x{:x} for " GEODE_MOD_ID, reinterpret_cast<uintptr_t>(visitEntry));
        }
    }

    class $modify(BlurCCEGLVPHook, cocos2d::CCEGLViewProtocol) {
        void setFrameSize(float width, float height) override {
            CCEGLViewProtocol::setFrameSize(width, height);

            if (!cocos2d::CCDirector::get()->getOpenGLView())
                return;

            ppRt0.resize((GLsizei)width, (GLsizei)height);
            ppRt1.resize((GLsizei)width, (GLsizei)height);
        }
    };

    class $modify(BlurGMHook, GameManager) {
        void reloadAllStep5() {
            GameManager::reloadAllStep5();
            cleanupPostProcess();
            setupPostProcess();
        }
    };

    void init() {
        geode::listenForSettingChanges<bool>("legacy-render", [](bool value) {
            geode::queueInMainThread([]() {
                cleanupPostProcess();
                setupPostProcess();
            });
        });
        geode::queueInMainThread([]() {
            cleanupPostProcess();
            setupPostProcess();
        });

        auto tm = ThemeManager::get();
        toggle(tm->getBlurEnabled());
    }

    void update(float) {
        auto tm = ThemeManager::get();
        auto duration = tm->getBlurSpeed();
        auto toggled = Engine::get()->isToggled();

        auto deltaTimeMod = utils::get<cocos2d::CCDirector>()->getActualDeltaTime();
        blurTimer += toggled ? deltaTimeMod : -deltaTimeMod;
        blurTimer = std::clamp(blurTimer, 0.f, duration);

        if (!tm->getBlurEnabled() || tm->getRenderer() == RendererType::Cocos2d) {
            blurProgress = 0.f;
            return;
        }

        if (duration == 0.f) blurProgress = toggled ? 1.f : 0.f;
        else blurProgress = blurTimer / duration;
    }

    void toggle(bool enabled) {
        if (!visitVtablePatch) {
            if (enabled) hookCCSceneVisit();
        } else {
            if (enabled) { (void) visitVtablePatch->enable(); }
            else { (void) visitVtablePatch->disable(); }
        }
    }

}

#else

namespace eclipse::gui::blur {

    void init() {}

    void update(float) {}

    void toggle(bool) {}

}

#endif
