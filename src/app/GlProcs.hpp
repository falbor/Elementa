#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <SDL3/SDL_opengl.h>
#include <SDL3/SDL_video.h>

#include <cstring>
#include <type_traits>

namespace sim::app {

// Dynamically loaded OpenGL 1.2..3.3 Core entry points (no global #define gl* macros).
struct GlProcs {
    // GL 1.3
    PFNGLACTIVETEXTUREPROC ActiveTexture = nullptr;

    // GL 1.4 / 2.0 blend state
    PFNGLBLENDEQUATIONPROC BlendEquation = nullptr;
    PFNGLBLENDEQUATIONSEPARATEPROC BlendEquationSeparate = nullptr;
    PFNGLBLENDFUNCSEPARATEPROC BlendFuncSeparate = nullptr;

    // GL 2.0 shaders, programs & uniforms
    PFNGLATTACHSHADERPROC AttachShader = nullptr;
    PFNGLCOMPILESHADERPROC CompileShader = nullptr;
    PFNGLCREATEPROGRAMPROC CreateProgram = nullptr;
    PFNGLCREATESHADERPROC CreateShader = nullptr;
    PFNGLDELETEPROGRAMPROC DeleteProgram = nullptr;
    PFNGLDELETESHADERPROC DeleteShader = nullptr;
    PFNGLDRAWBUFFERSPROC DrawBuffers = nullptr;
    PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog = nullptr;
    PFNGLGETPROGRAMIVPROC GetProgramiv = nullptr;
    PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog = nullptr;
    PFNGLGETSHADERIVPROC GetShaderiv = nullptr;
    PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation = nullptr;
    PFNGLISPROGRAMPROC IsProgram = nullptr;
    PFNGLLINKPROGRAMPROC LinkProgram = nullptr;
    PFNGLSHADERSOURCEPROC ShaderSource = nullptr;
    PFNGLUNIFORM1FPROC Uniform1f = nullptr;
    PFNGLUNIFORM1IPROC Uniform1i = nullptr;
    PFNGLUNIFORM2FPROC Uniform2f = nullptr;
    PFNGLUNIFORM3FPROC Uniform3f = nullptr;
    PFNGLUNIFORM4FPROC Uniform4f = nullptr;
    PFNGLUSEPROGRAMPROC UseProgram = nullptr;

    // GL 3.0 framebuffers & vertex array objects
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer = nullptr;
    PFNGLBINDVERTEXARRAYPROC BindVertexArray = nullptr;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus = nullptr;
    PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers = nullptr;
    PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays = nullptr;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D = nullptr;
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers = nullptr;
    PFNGLGENVERTEXARRAYSPROC GenVertexArrays = nullptr;

    // GL 3.3 optional sampler binding (used defensively for state restore)
    PFNGLBINDSAMPLERPROC BindSampler = nullptr;

    template <typename Fn> static bool loadProc(Fn &out, const char *name) noexcept {
        static_assert(std::is_pointer_v<Fn>, "GlProcs member must be a function pointer");
        const SDL_FunctionPointer fp = SDL_GL_GetProcAddress(name);
        static_assert(sizeof(Fn) == sizeof(fp), "Function pointer size mismatch");
        if (!fp) {
            out = nullptr;
            return false;
        }
        std::memcpy(&out, &fp, sizeof(out));
        return out != nullptr;
    }

    bool loadAll() noexcept {
        reset();
        bool ok = true;
        ok = loadProc(ActiveTexture, "glActiveTexture") && ok;
        ok = loadProc(BlendEquation, "glBlendEquation") && ok;
        ok = loadProc(BlendEquationSeparate, "glBlendEquationSeparate") && ok;
        ok = loadProc(BlendFuncSeparate, "glBlendFuncSeparate") && ok;

        ok = loadProc(AttachShader, "glAttachShader") && ok;
        ok = loadProc(CompileShader, "glCompileShader") && ok;
        ok = loadProc(CreateProgram, "glCreateProgram") && ok;
        ok = loadProc(CreateShader, "glCreateShader") && ok;
        ok = loadProc(DeleteProgram, "glDeleteProgram") && ok;
        ok = loadProc(DeleteShader, "glDeleteShader") && ok;
        ok = loadProc(DrawBuffers, "glDrawBuffers") && ok;
        ok = loadProc(GetProgramInfoLog, "glGetProgramInfoLog") && ok;
        ok = loadProc(GetProgramiv, "glGetProgramiv") && ok;
        ok = loadProc(GetShaderInfoLog, "glGetShaderInfoLog") && ok;
        ok = loadProc(GetShaderiv, "glGetShaderiv") && ok;
        ok = loadProc(GetUniformLocation, "glGetUniformLocation") && ok;
        ok = loadProc(IsProgram, "glIsProgram") && ok;
        ok = loadProc(LinkProgram, "glLinkProgram") && ok;
        ok = loadProc(ShaderSource, "glShaderSource") && ok;
        ok = loadProc(Uniform1f, "glUniform1f") && ok;
        ok = loadProc(Uniform1i, "glUniform1i") && ok;
        ok = loadProc(Uniform2f, "glUniform2f") && ok;
        ok = loadProc(Uniform3f, "glUniform3f") && ok;
        ok = loadProc(Uniform4f, "glUniform4f") && ok;
        ok = loadProc(UseProgram, "glUseProgram") && ok;

        ok = loadProc(BindFramebuffer, "glBindFramebuffer") && ok;
        ok = loadProc(BindVertexArray, "glBindVertexArray") && ok;
        ok = loadProc(CheckFramebufferStatus, "glCheckFramebufferStatus") && ok;
        ok = loadProc(DeleteFramebuffers, "glDeleteFramebuffers") && ok;
        ok = loadProc(DeleteVertexArrays, "glDeleteVertexArrays") && ok;
        ok = loadProc(FramebufferTexture2D, "glFramebufferTexture2D") && ok;
        ok = loadProc(GenFramebuffers, "glGenFramebuffers") && ok;
        ok = loadProc(GenVertexArrays, "glGenVertexArrays") && ok;

        // Optional in case context is 3.2 fallback without ARB_sampler_objects
        (void)loadProc(BindSampler, "glBindSampler");
        return ok;
    }

    void reset() noexcept { *this = GlProcs{}; }
};

} // namespace sim::app
