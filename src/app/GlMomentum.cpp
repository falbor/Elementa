#include "GlMomentum.hpp"
#include "GlProcs.hpp"
#include "sim/ContinuumMomentum.hpp"
#include "sim/SimWorld.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace sim::app {
namespace {
constexpr const char *vertexSource = R"glsl(#version 330 core
void main() {
    gl_Position = vec4(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0, 0.0, 1.0);
}
)glsl";

constexpr const char *fragmentHeader = R"glsl(#version 330 core
uniform sampler2D stateTex;
uniform sampler2D rhsTex;
uniform sampler2D currentTex;
uniform vec2 gridSize;
uniform float stepDt;
uniform int closedBoundary;
uniform int diffusionPass;
layout(location = 0) out vec2 resultVelocity;
ivec2 grid() { return ivec2(gridSize); }
bool wall(ivec2 p) {
    if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, grid()))) return closedBoundary != 0;
    return texelFetch(stateTex, p, 0).a > 0.5;
}
bool blocked(ivec2 p, bool vertical) {
    return wall(p) || wall(p - (vertical ? ivec2(0,1) : ivec2(1,0)));
}
ivec2 faceSize(bool vertical) { return grid() + (vertical ? ivec2(0,1) : ivec2(1,0)); }
float original(ivec2 p, bool vertical) {
    if (blocked(p, vertical)) return 0.0;
    vec2 vel = texelFetch(stateTex, p, 0).rg;
    return vertical ? vel.g : vel.r;
}
float sampleVelocity(vec2 p, bool vertical) {
    ivec2 size = faceSize(vertical);
    p = clamp(p + (vertical ? vec2(0.0,0.5) : vec2(0.5,0.0)), vec2(0), vec2(size-1));
    ivec2 base = ivec2(p);
    vec2 f = p - vec2(base);
    float sum = 0.0, weight = 0.0;
    for (int y=0; y<2; ++y) for (int x=0; x<2; ++x) {
        ivec2 q = min(base+ivec2(x,y), size-1);
        if (blocked(q, vertical)) continue;
        float w = (x==0 ? 1.0-f.x : f.x) * (y==0 ? 1.0-f.y : f.y);
        sum += original(q, vertical) * w;
        weight += w;
    }
    return weight > 1e-6 ? sum / weight : 0.0;
}
)glsl";

constexpr const char *fragmentAdvection = R"glsl(
vec2 backtrace(vec2 p) {
    vec2 midpoint = p - 0.5*stepDt*vec2(sampleVelocity(p,false),sampleVelocity(p,true));
    vec2 delta = -stepDt*vec2(sampleVelocity(midpoint,false),sampleVelocity(midpoint,true));
    int steps = max(1,int(ceil(max(abs(delta.x),abs(delta.y))*4.0)));
    vec2 start = p;
    for (int s=1; s<=steps; ++s) {
        vec2 next = start + delta*(float(s)/float(steps));
        ivec2 from = ivec2(floor(p+0.5)), to = ivec2(floor(next+0.5));
        if (wall(to) || (to.x!=from.x && to.y!=from.y &&
            (wall(ivec2(to.x,from.y)) || wall(ivec2(from.x,to.y))))) break;
        p = next;
    }
    return p;
}
float advected(ivec2 p, bool vertical) {
    if (any(greaterThanEqual(p,faceSize(vertical))) || blocked(p,vertical)) return 0.0;
    vec2 position = vec2(p) - (vertical ? vec2(0,0.5) : vec2(0.5,0));
    return sampleVelocity(backtrace(position),vertical);
}
float viscosity(ivec2 p) {
    return texelFetch(stateTex,clamp(p,ivec2(0),grid()-1),0).b;
}
float faceViscosity(ivec2 p, bool vertical) {
    return 0.5*(viscosity(p)+viscosity(p-(vertical ? ivec2(0,1) : ivec2(1,0))));
}
float diffused(ivec2 p, bool vertical) {
    ivec2 size = faceSize(vertical);
    if (any(greaterThanEqual(p,size)) || blocked(p,vertical)) return 0.0;
    vec2 rhs = texelFetch(rhsTex,p,0).rg;
    float sum = vertical ? rhs.g : rhs.r, denominator = 1.0;
    float nu = faceViscosity(p,vertical);
    for (int d=0; d<4; ++d) {
        ivec2 q = p + ivec2(d==0 ? -1 : d==1 ? 1 : 0, d==2 ? -1 : d==3 ? 1 : 0);
        if (any(lessThan(q,ivec2(0))) || any(greaterThanEqual(q,size)) || blocked(q,vertical)) continue;
        float coefficient = stepDt*0.5*(nu+faceViscosity(q,vertical));
        vec2 neighbor = texelFetch(currentTex,q,0).rg;
        sum += coefficient*(vertical ? neighbor.g : neighbor.r);
        denominator += coefficient;
    }
    return sum*(1.0/denominator);
}
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    resultVelocity = diffusionPass == 0 ? vec2(advected(p,false),advected(p,true))
                                       : vec2(diffused(p,false),diffused(p,true));
}
)glsl";

// Preserve the renderer/ImGui state, including pixel buffer bindings that
// would otherwise reinterpret CPU upload/readback pointers as buffer offsets.
struct StateGuard {
    GlProcs &gl;
    GLint drawFbo{}, readFbo{}, program{}, vao{}, active{}, viewport[4]{}, pack{}, unpack{}, packRow{}, unpackRow{};
    GLint packBuffer{}, unpackBuffer{}, polygon[2]{}, texture[3]{}, sampler[3]{};
    GLboolean colorMask[4]{};
    static constexpr GLenum caps[] = {GL_BLEND,     GL_DEPTH_TEST,         GL_STENCIL_TEST,     GL_SCISSOR_TEST,
                                      GL_CULL_FACE, GL_RASTERIZER_DISCARD, GL_FRAMEBUFFER_SRGB, GL_DITHER};
    std::array<GLboolean, std::size(caps)> enabled{};
    explicit StateGuard(GlProcs &functions) : gl(functions) {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &packRow);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpackRow);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
        glGetIntegerv(GL_POLYGON_MODE, polygon);
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
        for (int i = 0; i < 3; ++i) {
            gl.ActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture[i]);
            if (gl.BindSampler) {
                glGetIntegerv(GL_SAMPLER_BINDING, &sampler[i]);
                gl.BindSampler(i, 0);
            }
        }
        for (size_t i = 0; i < enabled.size(); ++i) {
            enabled[i] = glIsEnabled(caps[i]);
            glDisable(caps[i]);
        }
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    ~StateGuard() {
        gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
        gl.BindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
        gl.UseProgram(program);
        gl.BindVertexArray(vao);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        for (int i = 0; i < 3; ++i) {
            gl.ActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, texture[i]);
            if (gl.BindSampler) gl.BindSampler(i, sampler[i]);
        }
        gl.ActiveTexture(active);
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
        gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
        glPixelStorei(GL_PACK_ALIGNMENT, pack);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpack);
        glPixelStorei(GL_PACK_ROW_LENGTH, packRow);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, unpackRow);
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        glPolygonMode(GL_FRONT_AND_BACK, polygon[0]);
        for (size_t i = 0; i < enabled.size(); ++i)
            if (enabled[i]) glEnable(caps[i]);
    }
};
} // namespace

struct GlMomentum::Impl {
    GlProcs gl;
    SDL_GLContext context = nullptr;
    GLuint program = 0, vao = 0, fbo = 0, texture[4]{};
    GLint passLocation = -1, dtLocation = -1, closedLocation = -1;
    int width = 0, height = 0;
    bool ready = false;
    std::vector<float> upload, download;
    GLuint compile(GLenum type, const char *const *source, int count) {
        const GLuint shader = gl.CreateShader(type);
        gl.ShaderSource(shader, count, source, nullptr);
        gl.CompileShader(shader);
        GLint success = 0;
        gl.GetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char log[4096]{};
            gl.GetShaderInfoLog(shader, sizeof(log), nullptr, log);
            std::cerr << "GPU momentum shader: " << log << '\n';
            gl.DeleteShader(shader);
            return 0;
        }
        return shader;
    }
};

GlMomentum::GlMomentum() : m_impl(std::make_unique<Impl>()) {}
GlMomentum::~GlMomentum() {
    shutdown();
}
bool GlMomentum::isReady() const noexcept {
    return m_impl->ready;
}
void GlMomentum::shutdown() {
    auto &s = *m_impl;
    if (s.context && SDL_GL_GetCurrentContext() == s.context) {
        if (s.program) s.gl.DeleteProgram(s.program);
        if (s.vao) s.gl.DeleteVertexArrays(1, &s.vao);
        if (s.fbo) s.gl.DeleteFramebuffers(1, &s.fbo);
        glDeleteTextures(4, s.texture);
    }
    s.program = s.vao = s.fbo = 0;
    std::fill(std::begin(s.texture), std::end(s.texture), 0);
    s.ready = false;
    s.context = nullptr;
}

bool GlMomentum::initialize(int width, int height) {
    shutdown();
    auto &s = *m_impl;
    s.context = SDL_GL_GetCurrentContext();
    if (!s.context || width < 1 || height < 1 || !s.gl.loadAll()) return false;
    StateGuard state(s.gl);
    GLint maximum = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
    if (width >= maximum || height >= maximum) return false;
    const char *fragment[] = {fragmentHeader, fragmentAdvection};
    const GLuint vertex = s.compile(GL_VERTEX_SHADER, &vertexSource, 1);
    const GLuint frag = s.compile(GL_FRAGMENT_SHADER, fragment, 2);
    if (!vertex || !frag) {
        if (vertex) s.gl.DeleteShader(vertex);
        if (frag) s.gl.DeleteShader(frag);
        return false;
    }
    s.program = s.gl.CreateProgram();
    s.gl.AttachShader(s.program, vertex);
    s.gl.AttachShader(s.program, frag);
    s.gl.LinkProgram(s.program);
    s.gl.DeleteShader(vertex);
    s.gl.DeleteShader(frag);
    GLint linked = 0;
    s.gl.GetProgramiv(s.program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[4096]{};
        s.gl.GetProgramInfoLog(s.program, sizeof(log), nullptr, log);
        std::cerr << log << '\n';
        return false;
    }
    s.width = width;
    s.height = height;
    s.upload.resize(static_cast<size_t>(width + 1) * (height + 1) * 4);
    s.download.resize(static_cast<size_t>(width + 1) * (height + 1) * 2);
    s.gl.GenVertexArrays(1, &s.vao);
    s.gl.GenFramebuffers(1, &s.fbo);
    glGenTextures(4, s.texture);
    s.gl.ActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < 4; ++i) {
        glBindTexture(GL_TEXTURE_2D, s.texture[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, i == 0 ? GL_RGBA32F : GL_RG32F, width + 1, height + 1, 0,
                     i == 0 ? GL_RGBA : GL_RG, GL_FLOAT, nullptr);
    }
    s.gl.BindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    s.gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.texture[1], 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    if (s.gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
    s.gl.UseProgram(s.program);
    s.gl.Uniform1i(s.gl.GetUniformLocation(s.program, "stateTex"), 0);
    s.gl.Uniform1i(s.gl.GetUniformLocation(s.program, "rhsTex"), 1);
    s.gl.Uniform1i(s.gl.GetUniformLocation(s.program, "currentTex"), 2);
    s.gl.Uniform2f(s.gl.GetUniformLocation(s.program, "gridSize"), static_cast<float>(width),
                   static_cast<float>(height));
    s.passLocation = s.gl.GetUniformLocation(s.program, "diffusionPass");
    s.dtLocation = s.gl.GetUniformLocation(s.program, "stepDt");
    s.closedLocation = s.gl.GetUniformLocation(s.program, "closedBoundary");
    s.ready = glGetError() == GL_NO_ERROR;
    if (s.ready) {
        // Verify this driver before connecting it to the live world. The
        // probe has walls, spatially varying viscosity and both face fields.
        sim::ThreadPool pool(4);
        sim::ContinuumMomentum reference(width, height, &pool);
        std::vector<float> u((width + 1) * height), v(width * (height + 1)), nu(width * height);
        std::vector<uint8_t> solid(width * height);
        for (size_t i = 0; i < u.size(); ++i)
            u[i] = 0.2f * std::sin(static_cast<float>(i) * 0.13f);
        for (size_t i = 0; i < v.size(); ++i)
            v[i] = 0.2f * std::cos(static_cast<float>(i) * 0.17f);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                nu[y * width + x] = 0.12f * (0.5f + 0.5f * std::sin(x * 0.13f));
                solid[y * width + x] = x == width / 2 && y > height / 4 && y < 3 * height / 4;
            }
        auto cpuU = u, cpuV = v;
        reference.advance(cpuU, cpuV, nu, solid, 1.0f, true);
        if (!advance(u, v, nu, solid, 1.0f, true)) return false;
        float maximumError = 0.0f;
        for (size_t i = 0; i < u.size(); ++i)
            maximumError = std::max(maximumError, std::abs(u[i] - cpuU[i]));
        for (size_t i = 0; i < v.size(); ++i)
            maximumError = std::max(maximumError, std::abs(v[i] - cpuV[i]));
        if (maximumError > 5e-5f) {
            std::cerr << "Verifica moto GPU fallita: errore=" << maximumError << "; uso CPU.\n";
            s.ready = false;
        }
    }
    return s.ready;
}

bool GlMomentum::advance(std::span<float> u, std::span<float> v, std::span<const float> viscosity,
                         std::span<const uint8_t> solid, float dt, bool closed) {
    auto &s = *m_impl;
    const int W = s.width, H = s.height, stride = W + 1;
    if (!s.ready || SDL_GL_GetCurrentContext() != s.context || u.size() != static_cast<size_t>(W + 1) * H ||
        v.size() != static_cast<size_t>(H + 1) * W || viscosity.size() != static_cast<size_t>(W) * H ||
        solid.size() != viscosity.size())
        return false;
    StateGuard state(s.gl);
    for (int y = 0; y <= H; ++y)
        for (int x = 0; x <= W; ++x) {
            const size_t i = static_cast<size_t>(y * stride + x) * 4;
            const int cell = std::min(y, H - 1) * W + std::min(x, W - 1);
            s.upload[i] = y < H ? u[y * stride + x] : 0.0f;
            s.upload[i + 1] = x < W ? v[y * W + x] : 0.0f;
            s.upload[i + 2] = viscosity[cell];
            s.upload[i + 3] = solid[cell] ? 1.0f : 0.0f;
        }
    s.gl.ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s.texture[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W + 1, H + 1, GL_RGBA, GL_FLOAT, s.upload.data());
    s.gl.BindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    s.gl.BindVertexArray(s.vao);
    s.gl.UseProgram(s.program);
    glViewport(0, 0, W + 1, H + 1);
    s.gl.Uniform1f(s.dtLocation, dt);
    s.gl.Uniform1i(s.closedLocation, closed ? 1 : 0);
    auto draw = [&](GLuint output) {
        s.gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    };
    // Do not bind the render target as a sampled texture, even on the inactive
    // uniform branch: texture feedback is undefined in OpenGL.
    s.gl.ActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s.texture[0]);
    s.gl.ActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, s.texture[0]);
    s.gl.Uniform1i(s.passLocation, 0);
    draw(s.texture[1]);
    s.gl.ActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s.texture[1]);
    s.gl.Uniform1i(s.passLocation, 1);
    GLuint current = s.texture[1];
    for (int iteration = 0; iteration < 12; ++iteration) {
        s.gl.ActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, current);
        current = s.texture[2 + (iteration & 1)];
        draw(current);
    }
    glReadPixels(0, 0, W + 1, H + 1, GL_RG, GL_FLOAT, s.download.data());
    if (glGetError() != GL_NO_ERROR || std::any_of(s.download.begin(), s.download.end(), [](float value) {
            return !std::isfinite(value) || std::abs(value) > 12.001f;
        })) {
        s.ready = false;
        std::cerr << "Moto GPU disattivato dopo errore; fallback CPU.\n";
        return false;
    }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x <= W; ++x)
            u[y * stride + x] = s.download[(y * stride + x) * 2];
    for (int y = 0; y <= H; ++y)
        for (int x = 0; x < W; ++x)
            v[y * W + x] = s.download[(y * stride + x) * 2 + 1];
    return true;
}

int validateGpuMomentum(bool integratedWorlds) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << SDL_GetError() << '\n';
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *window = SDL_CreateWindow("GPU momentum validation", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : nullptr;
    int status = 0;
    if (!context) {
        std::cerr << SDL_GetError() << '\n';
        status = 1;
    } else
        try {
            std::cout << "GPU momentum device: " << reinterpret_cast<const char *>(glGetString(GL_RENDERER)) << '\n';
            sim::ThreadPool pool(8);
            if (!integratedWorlds)
                for (auto size : {std::pair{64, 48}, std::pair{480, 270}}) {
                    const int W = size.first, H = size.second;
                    GlMomentum gpu;
                    if (!gpu.initialize(W, H)) throw std::runtime_error("GPU momentum initialization failed");
                    sim::ContinuumMomentum cpu(W, H, &pool), accelerated(W, H, &pool);
                    accelerated.setBackend([&](auto u, auto v, auto nu, auto solid, float dt, bool closed) {
                        if (!gpu.advance(u, v, nu, solid, dt, closed))
                            throw std::runtime_error("GPU momentum execution failed");
                        return true;
                    });
                    std::vector<float> sourceU((W + 1) * H), sourceV(W * (H + 1)), nu(W * H);
                    std::vector<uint8_t> solid(W * H);
                    for (int scenario = 0; scenario < 5; ++scenario) {
                        for (int y = 0; y < H; ++y)
                            for (int x = 0; x < W; ++x) {
                                solid[y * W + x] = scenario >= 2 && ((x == W / 2 && y > H / 4 && y < 3 * H / 4) ||
                                                                     (y == H / 2 && x > W / 4 && x < W / 2));
                                nu[y * W + x] = scenario < 2    ? 0.0f
                                                : scenario == 4 ? 0.8f
                                                                : 0.12f * (0.5f + 0.5f * std::sin(x * 0.13f));
                            }
                        for (size_t i = 0; i < sourceU.size(); ++i)
                            sourceU[i] = scenario == 0 ? 0.0f
                                         : scenario == 1
                                             ? 0.15f
                                             : (scenario == 4 ? 3.0f : 0.2f) * std::sin(static_cast<float>(i) * 0.13f);
                        for (size_t i = 0; i < sourceV.size(); ++i)
                            sourceV[i] = scenario == 0 ? 0.0f
                                         : scenario == 1
                                             ? -0.05f
                                             : (scenario == 4 ? 2.0f : 0.2f) * std::cos(static_cast<float>(i) * 0.17f);
                        auto cu = sourceU, cv = sourceV, gu = sourceU, gv = sourceV;
                        float error = 0.0f;
                        for (int step = 0; step < 8; ++step) {
                            cpu.advance(cu, cv, nu, solid, 1.0f, scenario % 2 == 0);
                            accelerated.advance(gu, gv, nu, solid, 1.0f, scenario % 2 == 0);
                            for (size_t i = 0; i < cu.size(); ++i)
                                error = std::max(error, std::abs(cu[i] - gu[i]));
                            for (size_t i = 0; i < cv.size(); ++i)
                                error = std::max(error, std::abs(cv[i] - gv[i]));
                        }
                        std::cout << W << 'x' << H << " case=" << scenario << " max error=" << error << '\n';
                        if (error > 2e-4f) throw std::runtime_error("GPU/CPU momentum mismatch");
                    }
                    std::fill(solid.begin(), solid.end(), 0);
                    for (int y = 0; y < H; ++y)
                        for (int x = 0; x < W; ++x)
                            nu[y * W + x] = y > H / 2 ? 0.008f : 0.0f;
                    for (int y = 0; y < H; ++y)
                        for (int x = 0; x <= W; ++x)
                            sourceU[y * (W + 1) + x] = y > H / 2 ? 0.3f * std::sin(x * 0.1f) : 0.0f;
                    for (int y = 0; y <= H; ++y)
                        for (int x = 0; x < W; ++x)
                            sourceV[y * W + x] = y > H / 2 ? 0.2f * std::cos(x * 0.1f) : 0.0f;
                    std::vector<double> cpuTimes, gpuTimes;
                    for (int iteration = 0; iteration < 6; ++iteration)
                        for (int backend = 0; backend < 2; ++backend) {
                            auto u = sourceU, v = sourceV;
                            const auto start = std::chrono::steady_clock::now();
                            (backend == 0 ? cpu : accelerated).advance(u, v, nu, solid, 1.0f, true);
                            const double ms =
                                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                                    .count();
                            if (iteration > 0) (backend == 0 ? cpuTimes : gpuTimes).push_back(ms);
                        }
                    std::sort(cpuTimes.begin(), cpuTimes.end());
                    std::sort(gpuTimes.begin(), gpuTimes.end());
                    std::cout << W << 'x' << H << " median CPU=" << cpuTimes[2] << " ms GPU=" << gpuTimes[2]
                              << " ms (upload and synchronous readback included)\n";
                }
            if (integratedWorlds) {
                for (auto preset : {sim::PresetId::Laboratory, sim::PresetId::Volcano, sim::PresetId::Hydrodynamics,
                                    sim::PresetId::Explosives, sim::PresetId::ElectricGrid, sim::PresetId::ZeroGOrb}) {
                    constexpr int W = 480, H = 270;
                    sim::SimWorld world(W, H, 0);
                    world.loadPreset(preset);
                    world.enableContinuum();
                    GlMomentum gpu;
                    if (!gpu.initialize(W, H)) throw std::runtime_error("GPU world initialization failed");
                    world.setContinuumMomentumBackend([&](auto u, auto v, auto nu, auto solid, float dt, bool closed) {
                        if (!gpu.advance(u, v, nu, solid, dt, closed))
                            throw std::runtime_error("GPU world unexpectedly fell back to CPU");
                        return true;
                    });
                    double divergence = 0.0;
                    float maxFill = 0.0f;
                    for (int tick = 0; tick < 60; ++tick) {
                        world.step();
                        if (!world.stats().continuumPressureConverged)
                            throw std::runtime_error("GPU world pressure did not converge");
                        divergence = std::max(divergence, world.stats().continuumDivergence);
                        for (int y = 0; y < H; ++y)
                            for (int x = 0; x < W; ++x) {
                                float fill = 0.0f;
                                for (const auto &material : sim::getAllMaterials()) {
                                    const float q = world.fluidAmountAt(material.id, x, y);
                                    if (!std::isfinite(q) || q < 0.0f)
                                        throw std::runtime_error("Invalid GPU world concentration");
                                    if (material.phase == sim::PhaseState::Liquid) fill += q;
                                }
                                maxFill = std::max(maxFill, fill);
                                const auto cell = world.inspect(x, y);
                                if (!std::isfinite(cell.tempK) || cell.tempK <= 0.0f || !std::isfinite(cell.vx) ||
                                    !std::isfinite(cell.vy))
                                    throw std::runtime_error("Invalid GPU world thermodynamic state");
                            }
                    }
                    world.setContinuumMomentumBackend({});
                    std::cout << "GPU world preset=" << static_cast<int>(preset) << " max fill=" << maxFill
                              << " divergence=" << divergence << std::endl;
                    if (maxFill > 1.001f || divergence > 1e-4) throw std::runtime_error("GPU world invariant failed");
                }
            }
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
            status = 1;
        }
    if (context) SDL_GL_DestroyContext(context);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return status;
}
} // namespace sim::app
