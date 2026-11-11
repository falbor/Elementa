#include "GlShaderPipeline.hpp"

#include "GlProcs.hpp"
#include "sim/Material.hpp"
#include "sim/MaterialOptics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace sim::app {

namespace {

constexpr float kMinTempSqrt = 12.2474487139f; // sqrt(150.0f)
constexpr float kMaxTempSqrt = 70.7106781187f; // sqrt(5000.0f)
constexpr float kInvTempSqrtSpan = 1.0f / (kMaxTempSqrt - kMinTempSqrt);

constexpr std::array<uint8_t, kMaterialCount> buildOccupancyLUT() noexcept {
    std::array<uint8_t, kMaterialCount> lut{};
    for (size_t i = 0; i < kMaterialCount; ++i) {
        const auto &p = kMaterialTable[i];
        if (p.phase == PhaseState::StaticSolid) {
            lut[i] = 255u;
        } else if (p.phase == PhaseState::Powder) {
            lut[i] = 191u; // 0.75
        } else if (p.phase == PhaseState::Liquid) {
            lut[i] = 140u; // 0.55
        } else if (p.phase == PhaseState::EmptyGas && p.id != MaterialId::Air) {
            lut[i] = 38u; // 0.15 (Smoke, Steam, Methane)
        } else {
            lut[i] = 0u; // Air, Energy
        }
    }
    return lut;
}

constexpr std::array<float, kMaterialCount> buildLifeScaleLUT() noexcept {
    std::array<float, kMaterialCount> lut{};
    for (size_t i = 0; i < kMaterialCount; ++i) {
        const auto id = kMaterialTable[i].id;
        if (id == MaterialId::Smoke || id == MaterialId::Steam || id == MaterialId::Methane) {
            lut[i] = 1.0f / 420.0f;
        } else {
            lut[i] = 1.0f / 45.0f;
        }
    }
    return lut;
}

constexpr std::array<uint8_t, kMaterialCount> buildMovableLUT() noexcept {
    std::array<uint8_t, kMaterialCount> lut{};
    for (size_t i = 0; i < kMaterialCount; ++i) {
        lut[i] = kMaterialTable[i].isMovable() ? 1u : 0u;
    }
    return lut;
}

inline constexpr auto kOccupancyLUT = buildOccupancyLUT();
inline constexpr auto kLifeScaleLUT = buildLifeScaleLUT();
inline constexpr auto kMovableLUT = buildMovableLUT();

inline uint8_t floatToByte01(float v) noexcept {
    if (!(v > 0.0f)) return 0u;
    if (v >= 1.0f) return 255u;
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

inline uint8_t encodeVelocityByte(float v) noexcept {
    if (!std::isfinite(v)) return 128u;
    return floatToByte01(v * 0.125f + 0.5f);
}

// --- GLSL 3.30 Core Shader Chunks (each chunk < 3.5 KB for MSVC C2026 & -Woverlength-strings) ---

constexpr const char *kFullscreenVertChunk = R"glsl(#version 330 core
out vec2 vUv;
void main() {
    vec2 pos = vec2((gl_VertexID == 1) ? 3.0 : -1.0,
                    (gl_VertexID == 2) ? 3.0 : -1.0);
    vUv = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)glsl";

constexpr const char *kFragHeaderSingleChunk = R"glsl(#version 330 core
in vec2 vUv;
out vec4 fragColor;
)glsl";

constexpr const char *kFragHeaderMrtChunk = R"glsl(#version 330 core
in vec2 vUv;
layout(location = 0) out vec4 fragScene;
layout(location = 1) out vec4 fragRefract;
)glsl";

constexpr const char *kGlslLutChunk = R"glsl(
const int MAT_AIR          = 0;
const int MAT_BEDROCK      = 1;
const int MAT_STONE        = 2;
const int MAT_SAND         = 3;
const int MAT_WATER        = 4;
const int MAT_SALT_WATER   = 5;
const int MAT_SALT         = 6;
const int MAT_ICE          = 7;
const int MAT_SNOW         = 8;
const int MAT_WOOD         = 9;
const int MAT_PLANT        = 10;
const int MAT_OIL          = 11;
const int MAT_ACID         = 12;
const int MAT_METAL        = 13;
const int MAT_MOLTEN_METAL = 14;
const int MAT_GLASS        = 15;
const int MAT_MOLTEN_GLASS = 16;
const int MAT_LAVA         = 17;
const int MAT_OBSIDIAN     = 18;
const int MAT_GUNPOWDER    = 19;
const int MAT_NITRO        = 20;
const int MAT_FIRE         = 21;
const int MAT_SMOKE        = 22;
const int MAT_STEAM        = 23;
const int MAT_METHANE      = 24;
const int MAT_SPARK        = 25;
const int MAT_PLASMA       = 26;
const int MAT_BATTERY      = 27;

const int PHASE_GAS    = 0;
const int PHASE_POWDER = 1;
const int PHASE_LIQUID = 2;
const int PHASE_SOLID  = 3;
const int PHASE_ENERGY = 4;

struct MatInfo {
    int phase;
    vec3 baseColor;
    float colorVar;
    float ior;
    float refractStrength;
    vec3 emissive;
    float detailScale;
    vec3 sigma;
    float shadowOpacity;
    float translucency;
    float roughness;
    float metallic;
    float specularStrength;
};

int decodeMatId(float encoded) {
    return clamp(int(floor(encoded * 255.0 + 0.5)), 0, 27);
}

float decodeTempK(float encoded) {
    float s = 12.2474487 + encoded * 58.4632294;
    return s * s;
}

vec2 decodeVelocity(vec2 encoded) {
    return (encoded - vec2(128.0 / 255.0)) * 8.0;
}

MatInfo fetchMatInfo(sampler2D lutTex, int matId) {
    int id = clamp(matId, 0, 27);
    vec4 r0 = texelFetch(lutTex, ivec2(id, 0), 0);
    vec4 r1 = texelFetch(lutTex, ivec2(id, 1), 0);
    vec4 r2 = texelFetch(lutTex, ivec2(id, 2), 0);
    vec4 r3 = texelFetch(lutTex, ivec2(id, 3), 0);

    float pF = floor((r0.x + 0.25) * (1.0 / 65536.0));
    float rem = r0.x - pF * 65536.0;
    float g255 = floor((rem + 0.25) * (1.0 / 256.0));
    float r255 = rem - g255 * 256.0;
    float var255 = floor((r0.y + 0.25) * (1.0 / 256.0));
    float b255 = r0.y - var255 * 256.0;

    MatInfo info;
    info.phase = int(pF);
    info.baseColor = clamp(vec3(r255, g255, b255) / 255.0, 0.0, 1.0);
    info.colorVar = clamp(var255 / 255.0, 0.0, 1.0);
    info.ior = r0.z;
    info.refractStrength = r0.w;
    info.emissive = r1.xyz;
    info.detailScale = r1.w;
    info.sigma = r2.xyz;
    info.shadowOpacity = r2.w;
    info.translucency = r3.x;
    info.roughness = r3.y;
    info.metallic = r3.z;
    info.specularStrength = r3.w;
    return info;
}

bool isGasOrEnergyMat(int phase) {
    return phase == PHASE_GAS || phase == PHASE_ENERGY;
}

bool isOpticalTransparentMat(int matId) {
    return matId == MAT_WATER || matId == MAT_SALT_WATER || matId == MAT_OIL ||
           matId == MAT_ACID || matId == MAT_NITRO || matId == MAT_GLASS || matId == MAT_ICE;
}

bool isWispyGasMat(int matId) {
    return matId == MAT_SMOKE || matId == MAT_STEAM || matId == MAT_METHANE;
}
)glsl";

constexpr const char *kGlslNoiseAndColorChunk = R"glsl(
float ign(vec2 p) {
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(p, magic.xy)));
}

float hash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec2 hash22(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

float valueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm2(vec2 p) {
    float v = 0.55 * valueNoise(p);
    v += 0.30 * valueNoise(p * 2.07 + vec2(13.1, 7.7));
    v += 0.15 * valueNoise(p * 4.19 + vec2(3.7, 21.3));
    return v;
}

vec2 voronoiF1F2(vec2 p) {
    vec2 n = floor(p);
    vec2 f = fract(p);
    float d1 = 8.0;
    float d2 = 8.0;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            vec2 g = vec2(float(i), float(j));
            vec2 o = hash22(n + g);
            vec2 r = g + o - f;
            float d = dot(r, r);
            if (d < d1) {
                d2 = d1;
                d1 = d;
            } else if (d < d2) {
                d2 = d;
            }
        }
    }
    return sqrt(vec2(d1, d2));
}

vec3 srgbToLinear(vec3 c) {
    return pow(max(c, vec3(0.0)), vec3(2.2));
}

vec3 blackbodyHDR(float tempK) {
    if (tempK < 720.0) return vec3(0.0);
    float t = clamp((tempK - 720.0) / 3200.0, 0.0, 1.5);
    float intensity = min(2.2, 0.16 + 1.55 * pow(max(min(t, 1.25), 0.0001), 0.85));
    vec3 col = vec3(0.0);
    if (tempK < 1200.0) {
        float u = (tempK - 720.0) / 480.0;
        col = vec3(clamp(0.35 + 0.65 * u, 0.0, 1.0), clamp(0.28 * u * u, 0.0, 1.0), 0.0);
    } else if (tempK < 2000.0) {
        float u = (tempK - 1200.0) / 800.0;
        col = vec3(1.0, clamp(0.28 + 0.58 * u, 0.0, 1.0), clamp(0.32 * u * u, 0.0, 1.0));
    } else if (tempK < 3200.0) {
        float u = (tempK - 2000.0) / 1200.0;
        col = vec3(1.0, clamp(0.86 + 0.14 * u, 0.0, 1.0), clamp(0.32 + 0.60 * u, 0.0, 1.0));
    } else {
        float u = clamp((tempK - 3200.0) / 1800.0, 0.0, 1.0);
        col = vec3(1.0 - 0.14 * u, 1.0 - 0.04 * u, 1.0);
    }
    return pow(max(col, vec3(0.0)), vec3(2.0)) * intensity;
}
)glsl";

constexpr const char *kGlslLightingHelpersChunk = R"glsl(
vec3 computeBackground(vec2 simUv, vec2 simSize, vec2 canvasSize,
                       vec3 ambientSky, vec3 sunColor, float sunVis, vec3 localLight, float timeSec) {
    vec2 simPx = simUv * simSize;
    float y01 = clamp(simUv.y, 0.0, 1.0);

    vec3 topBg = vec3(0.022, 0.032, 0.058);
    vec3 botBg = vec3(0.011, 0.015, 0.028);
    vec3 bg = mix(topBg, botBg, y01);

    float strata = fbm2(vec2(simPx.x * 0.014, simPx.y * 0.030) + vec2(timeSec * 0.01, 0.0));
    bg += vec3(0.005, 0.007, 0.012) * (strata - 0.5);

    bg *= (0.45 + 0.95 * clamp(length(ambientSky) * 1.4, 0.05, 1.6));

    // Subtle atmospheric sun shaft on chamber background
    float shaftAngle = clamp(1.0 - (simUv.x * 0.55 + simUv.y * 0.45), 0.0, 1.0);
    bg += sunColor * (0.024 * sunVis * shaftAngle);

    // Subtle 32px minor and 128px major lab grid ticks
    vec2 pxPerSim = max(canvasSize / max(simSize, vec2(1.0)), vec2(1.0));
    vec2 grid32 = abs(fract(simPx / 32.0 + 0.5) - 0.5) * 32.0 * pxPerSim;
    float line32 = 1.0 - smoothstep(0.5, 1.35, min(grid32.x, grid32.y));
    vec2 grid128 = abs(fract(simPx / 128.0 + 0.5) - 0.5) * 128.0 * pxPerSim;
    float line128 = 1.0 - smoothstep(0.6, 1.5, min(grid128.x, grid128.y));
    bg += vec3(0.010, 0.015, 0.025) * line32 + vec3(0.018, 0.026, 0.042) * line128;

    // Local emitter bounce illumination on background chamber wall
    bg += localLight * 0.16;
    return bg;
}

vec3 computeGgxLit(vec3 albedoLin, vec3 N, vec3 ambientSky, vec3 sunColor, float sunVis,
                   vec3 localLight, float roughness, float metallic, float specStrength, float ao) {
    vec3 V = vec3(0.0, 0.0, 1.0);
    vec3 L = normalize(vec3(-0.46, 0.89, 0.52));
    vec3 H = normalize(L + V);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);

    float a = max(0.06, roughness * roughness);
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159265 * denom * denom);

    vec3 F0 = mix(vec3(0.04), albedoLin, metallic);
    vec3 F = F0 + (1.0 - F0) * pow(clamp(1.0 - HdotV, 0.0, 1.0), 5.0);
    vec3 kD = (1.0 - F) * (1.0 - metallic);

    vec3 specular = F * (D * specStrength * 0.22);
    float rim = pow(clamp(1.0 - NdotV, 0.0, 1.0), 2.4);

    vec3 irr = ambientSky * (0.65 + 0.35 * N.y) + sunColor * (NdotL * sunVis) + localLight * 0.92;
    vec3 diffTerm = albedoLin * kD * irr * ao;
    vec3 specTerm = specular * (sunColor * (NdotL * sunVis) + localLight * 0.45 + ambientSky * 0.18);
    vec3 rimTerm = (albedoLin * 0.35 + vec3(0.15)) * rim * (0.14 * sunVis * length(sunColor) + 0.16 * length(localLight));
    return diffTerm + specTerm + rimTerm;
}
)glsl";

// Pass 1a: Shadow & 2D Local Light Raymarch
constexpr const char *kPass1ShadowFragChunk = R"glsl(
uniform sampler2D uSimStateTex;
uniform sampler2D uSimLightTex;
uniform sampler2D uSimKinematicsTex;
uniform sampler2D uMatLutTex;
uniform vec2 uSimSize;
uniform vec2 uInvSimSize;
uniform int uShadowsEnabled;
uniform float uSunIntensity;

float marchSunConeRay(vec2 originSimUv, vec2 dirSim, float jitter) {
    float vis = 1.0;
    float dist = 1.05 + 0.55 * jitter;
    for (int s = 0; s < 24; ++s) {
        float stepLen = 1.05 + float(s) * 0.24;
        dist += stepLen;
        vec2 pUv = originSimUv + dirSim * (dist * uInvSimSize);
        if (pUv.x < 0.0 || pUv.x > 1.0 || pUv.y < 0.0 || pUv.y > 1.0) {
            break;
        }
        int mid = decodeMatId(texture(uSimStateTex, pUv).r);
        if (mid != MAT_AIR) {
            float op = texelFetch(uMatLutTex, ivec2(mid, 2), 0).w;
            if (op > 0.001) {
                vis *= pow(clamp(1.0 - op, 0.0, 1.0), stepLen * 0.78);
                if (vis < 0.015) return 0.0;
            }
        }
    }
    return vis;
}

void main() {
    vec2 simUv = vec2(vUv.x, 1.0 - vUv.y);
    vec4 simLightRaw = texture(uSimLightTex, simUv);
    vec3 simLight = simLightRaw.rgb * 2.5;

    if (uShadowsEnabled == 0) {
        fragColor = vec4(simLight, 1.0);
        return;
    }

    float occL = texture(uSimKinematicsTex, simUv - vec2(uInvSimSize.x, 0.0)).a;
    float occR = texture(uSimKinematicsTex, simUv + vec2(uInvSimSize.x, 0.0)).a;
    float occU = texture(uSimKinematicsTex, simUv - vec2(0.0, uInvSimSize.y)).a;
    float occD = texture(uSimKinematicsTex, simUv + vec2(0.0, uInvSimSize.y)).a;
    vec2 outGradSim = vec2(occL - occR, occU - occD);
    float gLen = length(outGradSim);
    vec2 pushUv = (gLen > 0.04) ? (outGradSim / gLen) * (0.90 * uInvSimSize) : vec2(0.0);

    // 3-ray directional sun cone (upper-left sun in simUv: -x, -y)
    float sunVis = 1.0;
    if (uSunIntensity > 0.001) {
        vec2 baseDir = normalize(vec2(-0.46, -0.89));
        vec3 angles = vec3(-0.085, 0.0, 0.085);
        vec3 weights = vec3(0.25, 0.50, 0.25);
        sunVis = 0.0;
        for (int r = 0; r < 3; ++r) {
            float c = cos(angles[r]);
            float s = sin(angles[r]);
            vec2 rDir = vec2(baseDir.x * c - baseDir.y * s, baseDir.x * s + baseDir.y * c);
            float j = ign(gl_FragCoord.xy + vec2(float(r) * 17.0, float(r) * 29.0));
            sunVis += weights[r] * marchSunConeRay(simUv + pushUv, rDir, j);
        }
    }
)glsl";

constexpr const char *kPass1ShadowPart2Chunk = R"glsl(
    // 16-ray 2D local light raymarch (R = 64 sim px)
    vec3 rayLight = vec3(0.0);
    float baseAngle = ign(gl_FragCoord.xy + vec2(37.0, 11.0)) * 6.2831853;
    float jitterR = ign(gl_FragCoord.xy + vec2(7.0, 53.0));
    vec2 rayOrigin = simUv + pushUv * 0.75;

    for (int i = 0; i < 16; ++i) {
        float ang = baseAngle + float(i) * 0.39269908;
        vec2 dir = vec2(cos(ang), sin(ang));
        float rayTrans = 1.0;

        for (int s = 0; s < 14; ++s) {
            float t = (float(s) + 0.35 + 0.30 * jitterR) / 14.0;
            float r = 1.4 + 62.6 * t * t;
            float dr = (62.6 * 2.0 * t) / 14.0;
            vec2 pUv = rayOrigin + dir * (r * uInvSimSize);
            if (pUv.x < 0.0 || pUv.x > 1.0 || pUv.y < 0.0 || pUv.y > 1.0) break;

            vec4 pst = texture(uSimStateTex, pUv);
            int pMat = decodeMatId(pst.r);
            if (pMat != MAT_AIR || pst.b > 0.245) {
                vec3 em = texelFetch(uMatLutTex, ivec2(pMat, 1), 0).rgb;
                float pTemp = decodeTempK(pst.b);
                vec3 bb = (pTemp > 720.0) ? blackbodyHDR(pTemp) * 0.75 : vec3(0.0);
                float pCharge = texture(uSimLightTex, pUv).a;
                vec3 corona = (pCharge > 0.08) ? vec3(0.20, 0.58, 1.0) * (pCharge * 0.80) : vec3(0.0);
                vec3 emit = em * 1.25 + bb + corona;
                if (dot(emit, vec3(1.0)) > 0.001) {
                    float atten = dr / (1.0 + 0.09 * r + 0.0045 * r * r);
                    rayLight += emit * rayTrans * atten;
                }
                if (pMat != MAT_AIR) {
                    float pOp = texelFetch(uMatLutTex, ivec2(pMat, 2), 0).w;
                    rayTrans *= pow(clamp(1.0 - pOp, 0.0, 1.0), dr * 0.72);
                    if (rayTrans < 0.02) break;
                }
            }
        }
    }

    rayLight *= (1.45 / 16.0);
    vec3 combinedLocal = min(rayLight * 0.78 + simLight * 0.52, vec3(3.5));
    fragColor = vec4(combinedLocal, clamp(sunVis, 0.0, 1.0));
}
)glsl";

// Pass 1b: Bilateral geometry-aware 9-tap tent smooth for FBO_Shadow
constexpr const char *kPass1TentSmoothFragChunk = R"glsl(
uniform sampler2D uShadowRawTex;
uniform sampler2D uSimKinematicsTex;
uniform vec2 uShadowTexelSize;
uniform int uShadowsEnabled;

void main() {
    vec4 center = texture(uShadowRawTex, vUv);
    if (uShadowsEnabled == 0) {
        fragColor = center;
        return;
    }

    vec2 simUv = vec2(vUv.x, 1.0 - vUv.y);
    float occC = texture(uSimKinematicsTex, simUv).a;

    vec3 weights = vec3(1.0, 2.0, 1.0);
    vec3 sumRgb = vec3(0.0);
    float sumRgbW = 0.0;
    float sumAlpha = 0.0;
    float sumAlphaW = 0.0;

    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float kw = weights[x + 1] * weights[y + 1];
            vec2 off = vec2(float(x), float(y)) * uShadowTexelSize;
            vec4 s = texture(uShadowRawTex, clamp(vUv + off, vec2(0.001), vec2(0.999)));
            vec2 nSimUv = vec2(vUv.x + off.x, 1.0 - (vUv.y + off.y));
            float occN = texture(uSimKinematicsTex, clamp(nSimUv, vec2(0.0), vec2(1.0))).a;
            float geomW = 1.0 - 0.75 * clamp(abs(occC - occN), 0.0, 1.0);
            float wRgb = kw * geomW;
            sumRgb += s.rgb * wRgb;
            sumRgbW += wRgb;
            sumAlpha += s.a * kw;
            sumAlphaW += kw;
        }
    }

    fragColor = vec4(sumRgb / max(sumRgbW, 1e-4), sumAlpha / max(sumAlphaW, 1e-4));
}
)glsl";

// Pass 2 Part A: Procedural micro-texture & normal bump for all 28 materials
constexpr const char *kPass2MaterialsChunk = R"glsl(
uniform sampler2D uSimStateTex;
uniform sampler2D uSimLightTex;
uniform sampler2D uSimKinematicsTex;
uniform sampler2D uMatLutTex;
uniform sampler2D uShadowTex;
uniform vec2 uSimSize;
uniform vec2 uInvSimSize;
uniform vec2 uCanvasSize;
uniform float uTime;
uniform float uAmbientLight;
uniform float uSunIntensity;

float sampleOcc(vec2 simUv) {
    return texture(uSimKinematicsTex, clamp(simUv, vec2(0.0), vec2(1.0))).a;
}

vec3 computeNormal25D(vec2 simUv, float scale) {
    vec2 d1 = uInvSimSize * 1.1;
    vec2 d2 = uInvSimSize * 2.3;
    float gX1 = sampleOcc(simUv - vec2(d1.x, 0.0)) - sampleOcc(simUv + vec2(d1.x, 0.0));
    float gY1 = sampleOcc(simUv + vec2(0.0, d1.y)) - sampleOcc(simUv - vec2(0.0, d1.y));
    float gX2 = sampleOcc(simUv - vec2(d2.x, 0.0)) - sampleOcc(simUv + vec2(d2.x, 0.0));
    float gY2 = sampleOcc(simUv + vec2(0.0, d2.y)) - sampleOcc(simUv - vec2(0.0, d2.y));
    vec2 grad = (vec2(gX1, gY1) * 0.65 + vec2(gX2, gY2) * 0.35) * scale;
    return normalize(vec3(grad, 0.72));
}

float computeInteriorAO(vec2 simUv) {
    float o = 0.0;
    o += sampleOcc(simUv + vec2(-3.0, -3.0) * uInvSimSize);
    o += sampleOcc(simUv + vec2( 3.0, -3.0) * uInvSimSize);
    o += sampleOcc(simUv + vec2(-3.0,  3.0) * uInvSimSize);
    o += sampleOcc(simUv + vec2( 3.0,  3.0) * uInvSimSize);
    o += sampleOcc(simUv + vec2( 0.0, -6.0) * uInvSimSize);
    o += sampleOcc(simUv + vec2( 0.0,  6.0) * uInvSimSize);
    float avg = o * (1.0 / 6.0);
    return clamp(1.08 - avg * 0.36, 0.62, 1.0);
}

float probeGasDir24(vec2 simUv, vec2 dir) {
    float minD = 24.0;
    for (int i = 0; i < 6; ++i) {
        float fi = float(i);
        float d = 1.0 + fi * (1.2 + 0.68 * fi);
        vec2 pUv = simUv + dir * (d * uInvSimSize);
        if (pUv.x < 0.0 || pUv.x > 1.0 || pUv.y < 0.0 || pUv.y > 1.0) {
            return min(minD, d);
        }
        int mid = decodeMatId(texture(uSimStateTex, pUv).r);
        MatInfo pm = fetchMatInfo(uMatLutTex, mid);
        if (isGasOrEnergyMat(pm.phase)) {
            return min(minD, d - 0.5);
        }
    }
    return minD;
}

float computeOpticalDepth24(vec2 simUv, int matId) {
    float depthUp = 24.0;
    for (int i = 0; i < 10; ++i) {
        float fi = float(i);
        float d = 1.0 + fi * (1.0 + 0.17 * fi);
        vec2 pUv = simUv - vec2(0.0, d * uInvSimSize.y);
        if (pUv.y < 0.0) {
            depthUp = d;
            break;
        }
        int mid = decodeMatId(texture(uSimStateTex, pUv).r);
        MatInfo pm = fetchMatInfo(uMatLutTex, mid);
        if (isGasOrEnergyMat(pm.phase)) {
            depthUp = d - 0.5;
            break;
        }
    }

    if (matId == MAT_GLASS || matId == MAT_ICE) {
        float minD = depthUp;
        minD = min(minD, probeGasDir24(simUv, vec2(0.0, 1.0)));
        minD = min(minD, probeGasDir24(simUv, vec2(-1.0, 0.0)));
        minD = min(minD, probeGasDir24(simUv, vec2(1.0, 0.0)));
        return clamp(minD, 0.0, 24.0);
    }

    // Thin free-falling stream check (only MAT_AIR triggers lateral thinning)
    float airL = 8.0;
    float airR = 8.0;
    vec3 lat = vec3(1.5, 3.5, 6.0);
    for (int i = 0; i < 3; ++i) {
        vec2 lUv = simUv - vec2(lat[i] * uInvSimSize.x, 0.0);
        vec2 rUv = simUv + vec2(lat[i] * uInvSimSize.x, 0.0);
        if (lUv.x >= 0.0 && decodeMatId(texture(uSimStateTex, lUv).r) == MAT_AIR) airL = min(airL, lat[i]);
        if (rUv.x <= 1.0 && decodeMatId(texture(uSimStateTex, rUv).r) == MAT_AIR) airR = min(airR, lat[i]);
    }
    if (airL < 6.0 && airR < 6.0) {
        return clamp(min(depthUp, 0.55 * (airL + airR)), 0.0, 24.0);
    }
    return clamp(depthUp, 0.0, 24.0);
}
)glsl";

constexpr const char *kPass2ShadingChunk = R"glsl(
void shadeOpaqueProcedural(int matId, MatInfo m, vec2 simPx, vec2 simUv, float seed01,
                           float tempK, float lifeNorm, float charge,
                           inout vec3 albedoSrgb, inout vec3 bump, inout float roughness, inout vec3 extraEmissive) {
    float seedDelta = (seed01 - 0.5) * 2.0 * m.colorVar;
    vec2 sp = simPx * m.detailScale;

    if (matId == MAT_BEDROCK) {
        float row = floor(sp.y * 0.14);
        float bx = fract(sp.x * 0.07 + row * 0.5);
        float by = fract(sp.y * 0.14);
        float mortar = (1.0 - smoothstep(0.0, 0.09, min(bx, 1.0 - bx))) +
                       (1.0 - smoothstep(0.0, 0.12, min(by, 1.0 - by)));
        float rock = fbm2(sp * 0.35);
        albedoSrgb += (rock - 0.5) * 0.08 - clamp(mortar, 0.0, 1.0) * 0.09 + seedDelta * 0.55;
        bump.xy += vec2((bx < 0.1 ? -0.35 : (bx > 0.9 ? 0.35 : 0.0)),
                        (by < 0.12 ? 0.35 : (by > 0.88 ? -0.35 : 0.0))) + (rock - 0.5) * 0.25;
    } else if (matId == MAT_STONE) {
        float warp = 2.4 * fbm2(sp * 0.06);
        float strata = sin(sp.y * 0.45 + warp) * 0.5 + 0.5;
        float pits = valueNoise(sp * 1.4);
        float quartz = smoothstep(0.91, 0.99, valueNoise(sp * 2.3 + vec2(4.2, 9.1)));
        albedoSrgb += (strata - 0.5) * 0.10 + (pits - 0.5) * 0.07 + vec3(0.14, 0.14, 0.16) * quartz + seedDelta * 0.65;
        bump.xy += vec2(pits - 0.5, strata - 0.5) * 0.38;
    } else if (matId == MAT_SAND) {
        float micro = valueNoise(sp * 2.8);
        float dune = sin(sp.x * 0.08 + sp.y * 0.16) * 0.5 + 0.5;
        albedoSrgb += vec3(seedDelta * 1.08, seedDelta * 0.94, seedDelta * 0.68);
        albedoSrgb += (micro - 0.5) * 0.08 + (dune - 0.5) * 0.04;
        bump.xy += vec2(micro - 0.5, valueNoise(sp * 2.8 + 5.7) - 0.5) * 0.42;
    } else if (matId == MAT_SALT || matId == MAT_SNOW) {
        float glint = smoothstep(0.89, 0.98, valueNoise(sp * 2.6 + seed01 * 7.0));
        float drift = fbm2(sp * 0.12);
        albedoSrgb += seedDelta * 0.75 + (drift - 0.5) * 0.06 + vec3(0.12) * glint;
        if (matId == MAT_SNOW) {
            albedoSrgb += vec3(-0.02, 0.0, 0.03) * (1.0 - drift);
        }
        roughness = mix(roughness, 0.16, glint);
    } else if (matId == MAT_WOOD) {
        float warp = 1.8 * fbm2(vec2(sp.x * 0.05, sp.y * 0.025));
        float ring = sin(sp.y * 0.65 + sp.x * 0.05 + warp);
        float fiber = valueNoise(vec2(sp.x * 0.35, sp.y * 1.8));
        albedoSrgb += vec3(0.065, 0.045, 0.022) * ring + (fiber - 0.5) * 0.06 + seedDelta * 0.45;
        float charAmt = smoothstep(450.0, 680.0, tempK);
        albedoSrgb = mix(albedoSrgb, vec3(0.07, 0.06, 0.055), charAmt * 0.85);
        roughness = mix(roughness, 0.96, charAmt);
        if (tempK > 620.0) {
            float ember = smoothstep(0.60, 0.88, valueNoise(sp * 1.1 + uTime * 0.6)) * smoothstep(620.0, 900.0, tempK);
            extraEmissive += vec3(1.1, 0.25, 0.02) * ember;
        }
        bump.xy += vec2(0.0, ring * 0.28 + (fiber - 0.5) * 0.22);
    } else if (matId == MAT_PLANT) {
)glsl";

constexpr const char *kPass2ShadingPart2Chunk = R"glsl(
        vec2 vf = voronoiF1F2(sp * 0.35);
        float vein = 1.0 - smoothstep(0.02, 0.16, vf.y - vf.x);
        float dap = fbm2(sp * 0.22);
        albedoSrgb += vec3(-0.02, 0.09, -0.02) * (dap - 0.4) + vec3(0.03, 0.08, 0.02) * vein + seedDelta * 0.7;
        bump.xy += vec2(dap - 0.5, vein * 0.35) * 0.35;
    } else if (matId == MAT_METAL) {
        float brush = valueNoise(vec2(sp.x * 0.18, sp.y * 2.6)) - 0.5;
        float diag = 0.5 + 0.5 * sin(sp.x * 0.07 + sp.y * 0.12);
        float seam = 1.0 - smoothstep(0.0, 0.06, min(fract(sp.x * 0.04), fract(sp.y * 0.04)));
        albedoSrgb += brush * 0.05 + diag * 0.06 - seam * 0.07 + seedDelta * 0.35;
        // Thermal temper oxidation tint > 500 K
        float temper = smoothstep(500.0, 1050.0, tempK);
        vec3 temperCol = mix(vec3(0.95, 0.78, 0.42), vec3(0.45, 0.55, 0.95), smoothstep(620.0, 920.0, tempK));
        albedoSrgb = mix(albedoSrgb, albedoSrgb * temperCol, temper * 0.72);
        bump.xy += vec2(0.0, brush * 0.22);
    } else if (matId == MAT_BATTERY) {
        float stripe = sin(sp.y * 0.9) * 0.5 + 0.5;
        albedoSrgb = mix(vec3(0.14, 0.22, 0.30), albedoSrgb, 0.45 + 0.55 * stripe) + seedDelta * 0.3;
        float busGlow = smoothstep(0.55, 0.95, stripe) * clamp(charge * 0.28, 0.25, 1.35);
        extraEmissive += vec3(0.10, 0.65, 1.15) * busGlow;
    } else if (matId == MAT_OBSIDIAN) {
        float arc = sin(length(fract(sp * 0.07) - vec2(0.2, 0.8)) * 18.0);
        float glint = smoothstep(0.88, 0.99, valueNoise(sp * 1.9));
        albedoSrgb += vec3(0.04, 0.02, 0.09) * arc + vec3(0.08, 0.05, 0.14) * glint + seedDelta * 0.55;
        bump.xy += vec2(arc * 0.22, (valueNoise(sp * 0.5) - 0.5) * 0.25);
    } else if (matId == MAT_GUNPOWDER) {
        float speck = valueNoise(sp * 3.1 + seed01 * 5.0);
        vec3 sulfur = smoothstep(0.84, 0.96, speck) * vec3(0.16, 0.15, 0.04);
        albedoSrgb += seedDelta * 0.85 + sulfur - (1.0 - speck) * 0.05;
        bump.xy += vec2(speck - 0.5, valueNoise(sp * 3.1 + 11.0) - 0.5) * 0.45;
    } else if (matId == MAT_LAVA) {
)glsl";

constexpr const char *kPass2ShadingPart3Chunk = R"glsl(
        vec2 vf = voronoiF1F2(sp * 0.16 + vec2(uTime * 0.12, -uTime * 0.08));
        float fissure = 1.0 - smoothstep(0.03, 0.22, vf.y - vf.x);
        float heatN = clamp((tempK - 1020.0) / 620.0, 0.15, 1.35);
        float crust = clamp((1.0 - fissure) * (1.15 - 0.65 * heatN), 0.0, 0.88);
        vec3 slagCol = vec3(0.16, 0.08, 0.07) + vec3(0.06) * fbm2(sp * 0.4);
        vec3 magmaCol = mix(vec3(1.0, 0.28, 0.03), vec3(1.0, 0.86, 0.28), clamp(fissure * heatN, 0.0, 1.0));
        albedoSrgb = mix(magmaCol, slagCol, crust);
        extraEmissive += pow(max(magmaCol, vec3(0.0)), vec3(1.8)) * (1.25 + 0.9 * fissure) * (1.0 - crust * 0.85);
        roughness = mix(0.25, 0.88, crust);
    } else if (matId == MAT_MOLTEN_METAL || matId == MAT_MOLTEN_GLASS) {
        float swirl = fbm2(sp * 0.18 + vec2(uTime * 0.45, -uTime * 0.30));
        float skin = smoothstep(0.68, 0.92, swirl) * clamp((1600.0 - tempK) / 500.0, 0.0, 0.65);
        albedoSrgb = mix(albedoSrgb + (swirl - 0.5) * 0.15, albedoSrgb * 0.38, skin);
        extraEmissive += pow(max(m.emissive, vec3(0.0)), vec3(1.4)) * (1.35 - skin * 0.85);
    } else if (matId == MAT_FIRE) {
        float lCore = clamp(lifeNorm * (45.0 / 26.0), 0.0, 1.0);
        float tongue = fbm2(vec2(sp.x * 0.45, sp.y * 0.28 + uTime * 7.5));
        float flameMask = clamp(0.45 + 0.65 * lCore + (tongue - 0.5) * 0.65, 0.0, 1.0);
        vec3 flameCol = mix(vec3(0.95, 0.18, 0.02), vec3(1.0, 0.88, 0.38), flameMask * lCore);
        albedoSrgb = flameCol;
        extraEmissive += pow(max(flameCol, vec3(0.0)), vec3(1.5)) * (1.45 + 1.2 * lCore);
    } else if (matId == MAT_SPARK) {
        float pulse = 0.75 + 0.25 * sin(uTime * 28.0 + seed01 * 12.0);
        albedoSrgb = vec3(1.0, 0.95, 0.62);
        extraEmissive += vec3(2.2, 1.75, 0.72) * pulse;
    } else if (matId == MAT_PLASMA) {
        float arc1 = abs(fbm2(sp * 0.32 + vec2(uTime * 8.0, -uTime * 6.0)) - 0.5) * 2.0;
        float filament = 1.0 - smoothstep(0.02, 0.28, arc1);
        vec3 pCol = mix(vec3(0.55, 0.22, 1.0), vec3(0.48, 0.92, 1.0), filament);
        albedoSrgb = pCol;
        extraEmissive += pCol * (1.75 + 1.6 * filament);
    } else {
        albedoSrgb += seedDelta;
    }
}
)glsl";

constexpr const char *kPass2MainChunk = R"glsl(
void main() {
    vec2 simUv = vec2(vUv.x, 1.0 - vUv.y);
    vec2 simPx = simUv * uSimSize;

    vec4 st = texture(uSimStateTex, simUv);
    int matId = decodeMatId(st.r);
    float seed01 = st.g;
    float tempK = decodeTempK(st.b);
    float lifeNorm = st.a;

    vec4 lightRaw = texture(uSimLightTex, simUv);
    float charge = lightRaw.a * 4.0;
    vec4 kin = texture(uSimKinematicsTex, simUv);
    float shockwave = kin.b;

    vec4 sh = texture(uShadowTex, vUv);
    vec3 localLight = sh.rgb;
    float sunVis = sh.a;

    vec3 sunColor = vec3(1.05, 0.98, 0.88) * uSunIntensity * 1.45;
    vec3 ambientSky = vec3(0.38, 0.48, 0.68) * uAmbientLight * 0.75;
    vec3 atmBg = computeBackground(simUv, uSimSize, uCanvasSize, ambientSky, sunColor, sunVis, localLight, uTime);

    MatInfo m = fetchMatInfo(uMatLutTex, matId);

    // Thermal heat-haze vector (for Air / gases / above hot emitters)
    float hazeAmp = smoothstep(335.0, 920.0, tempK) * 0.58;
    vec2 hazeVec = vec2(0.0);
    if (hazeAmp > 0.002) {
        float hx = fbm2(simPx * 0.13 + vec2(0.0, -uTime * 4.4)) - 0.5;
        float hy = fbm2(simPx * 0.13 + vec2(17.3, -uTime * 5.1)) - 0.5;
        hazeVec = vec2(hx, hy) * hazeAmp;
    }

    if (matId == MAT_AIR) {
        vec3 col = atmBg + vec3(0.16, 0.32, 0.58) * (shockwave * 0.38);
        if (tempK >= 720.0) col += blackbodyHDR(tempK);
        if (charge > 0.04) col += vec3(0.12, 0.48, 1.0) * min(1.5, (charge - 0.04) * 0.48);
        float w = (hazeAmp > 0.005 || shockwave > 0.02) ? 0.55 : 0.0;
        fragScene = vec4(col, 1.0);
        fragRefract = vec4(clamp(hazeVec * 0.5 + vec2(128.0 / 255.0), 0.0, 1.0), 0.0, w);
        return;
    }

    if (isWispyGasMat(matId)) {
        fragScene = vec4(atmBg, 1.0);
        fragRefract = vec4(clamp(hazeVec * 0.5 + vec2(128.0 / 255.0), 0.0, 1.0), 0.0, 0.55);
        return;
    }

    if (isOpticalTransparentMat(matId)) {
)glsl";

constexpr const char *kPass2MainPart2Chunk = R"glsl(
        float normScale = (m.phase == PHASE_SOLID) ? 2.8 : 2.1;
        vec3 N = computeNormal25D(simUv, normScale);
        if (m.phase == PHASE_LIQUID) {
            float w1 = sin(simPx.x * 0.36 - uTime * 3.8 + simPx.y * 0.11);
            float w2 = cos(simPx.x * 0.22 + uTime * 2.7 - simPx.y * 0.19);
            N = normalize(N + vec3((w1 + w2) * 0.14, (w1 - w2) * 0.10, 0.0));
        } else {
            vec2 vf = voronoiF1F2(simPx * 0.12);
            N = normalize(N + vec3(vf.x - 0.4, vf.y - 0.6, 0.0) * 0.16);
        }
        vec3 T = refract(vec3(0.0, 0.0, -1.0), N, 1.0 / max(m.ior, 1.001));
        vec2 snell = T.xy * m.refractStrength * 1.65 + hazeVec * 0.45;
        float optDepth = computeOpticalDepth24(simUv, matId);
        fragScene = vec4(atmBg, 1.0);
        fragRefract = vec4(clamp(snell * 0.5 + vec2(128.0 / 255.0), 0.0, 1.0),
                           clamp(optDepth / 24.0, 0.0, 1.0), 1.0);
        return;
    }

    // Opaque solids, powders, molten liquids, and energy materials
    float normScale = (m.phase == PHASE_SOLID) ? 3.1 : ((m.phase == PHASE_POWDER) ? 1.8 : 1.5);
    vec3 N = computeNormal25D(simUv, normScale);
    vec3 albedoSrgb = m.baseColor;
    vec3 bump = vec3(0.0);
    float rough = m.roughness;
    vec3 extraEm = vec3(0.0);

    shadeOpaqueProcedural(matId, m, simPx, simUv, seed01, tempK, lifeNorm, charge,
                          albedoSrgb, bump, rough, extraEm);

    N = normalize(N + bump * 0.55);
    vec3 albedoLin = srgbToLinear(clamp(albedoSrgb, 0.0, 1.25));
    float ao = (m.phase == PHASE_ENERGY) ? 1.0 : computeInteriorAO(simUv);

    vec3 col = vec3(0.0);
    if (m.phase == PHASE_ENERGY) {
        float energyAlpha = (matId == MAT_FIRE) ? clamp(0.42 + 0.58 * lifeNorm, 0.25, 1.0) : 0.92;
        col = mix(atmBg, albedoLin * 0.45, energyAlpha) + extraEm;
    } else {
        col = computeGgxLit(albedoLin, N, ambientSky, sunColor, sunVis, localLight,
                            rough, m.metallic, m.specularStrength, ao);
        bool hasCustomEm = (matId == MAT_LAVA || matId == MAT_MOLTEN_METAL || matId == MAT_MOLTEN_GLASS ||
                            matId == MAT_BATTERY);
        if (hasCustomEm) {
            col += extraEm;
        } else {
            col += srgbToLinear(max(m.emissive, vec3(0.0))) * 0.75 + extraEm;
        }
    }

    if (tempK >= 720.0) {
        float bbWeight = (m.phase == PHASE_ENERGY) ? 0.40 : ((matId == MAT_LAVA) ? 0.45 : 0.88);
        col += blackbodyHDR(tempK) * bbWeight;
    }
    if (charge > 0.04) {
        float qGlow = min(1.4, (charge - 0.04) * 0.48);
        col += vec3(0.10, 0.48, 1.0) * qGlow;
    }

    fragScene = vec4(max(col, vec3(0.0)), 1.0);
    fragRefract = vec4(128.0 / 255.0, 128.0 / 255.0, 0.0, 0.0);
}
)glsl";

// Pass 3: Optical Transparency, Refraction, Chromatic Dispersion, Beer-Lambert, Caustics, Fresnel & Wispy Gases
constexpr const char *kPass3CompositeMainChunk = R"glsl(
uniform sampler2D uSceneTex;
uniform sampler2D uRefractTex;
uniform sampler2D uShadowTex;
uniform sampler2D uSimStateTex;
uniform sampler2D uSimLightTex;
uniform sampler2D uSimKinematicsTex;
uniform sampler2D uMatLutTex;
uniform vec2 uSimSize;
uniform vec2 uInvSimSize;
uniform float uTime;
uniform float uAmbientLight;
uniform float uSunIntensity;
uniform int uRefractionEnabled;

vec3 sampleSceneRefracted(vec2 uvOff, bool chromatic) {
    if (chromatic) {
        float r = texture(uSceneTex, clamp(vUv + uvOff * 0.82, vec2(0.001), vec2(0.999))).r;
        float g = texture(uSceneTex, clamp(vUv + uvOff,        vec2(0.001), vec2(0.999))).g;
        float b = texture(uSceneTex, clamp(vUv + uvOff * 1.22, vec2(0.001), vec2(0.999))).b;
        return vec3(r, g, b);
    }
    return texture(uSceneTex, clamp(vUv + uvOff, vec2(0.001), vec2(0.999))).rgb;
}

void main() {
    vec4 rData = texture(uRefractTex, vUv);
    float rWeight = rData.a;
    if (rWeight <= 0.01) {
        fragColor = texture(uSceneTex, vUv);
        return;
    }

    vec2 simUv = vec2(vUv.x, 1.0 - vUv.y);
    vec2 simPx = simUv * uSimSize;
    vec4 st = texture(uSimStateTex, simUv);
    int matId = decodeMatId(st.r);
    float seed01 = st.g;
    float tempK = decodeTempK(st.b);
    float lifeNorm = st.a;

    vec4 kin = texture(uSimKinematicsTex, simUv);
    vec2 vel = decodeVelocity(kin.rg);
    float shock = kin.b;
    float charge = texture(uSimLightTex, simUv).a * 4.0;

    vec2 uvOffset = vec2(0.0);
    if (uRefractionEnabled != 0) {
        vec2 refractVec = (rData.rg - vec2(128.0 / 255.0)) * 2.0;
        float shL = texture(uSimKinematicsTex, simUv - vec2(uInvSimSize.x * 1.5, 0.0)).b;
        float shR = texture(uSimKinematicsTex, simUv + vec2(uInvSimSize.x * 1.5, 0.0)).b;
        float shU = texture(uSimKinematicsTex, simUv - vec2(0.0, uInvSimSize.y * 1.5)).b;
        float shD = texture(uSimKinematicsTex, simUv + vec2(0.0, uInvSimSize.y * 1.5)).b;
        vec2 shockGrad = vec2(shL - shR, shD - shU) * (1.2 + 2.5 * shock);
        uvOffset = clamp(refractVec * 0.016 + shockGrad * 0.018, vec2(-0.032), vec2(0.032));
    }

    bool chromatic = (uRefractionEnabled != 0) && (matId == MAT_GLASS || matId == MAT_ICE);
    vec3 refracted = sampleSceneRefracted(uvOffset, chromatic);

    vec4 sh = texture(uShadowTex, vUv);
    vec3 localLight = sh.rgb;
    float sunVis = sh.a;
    vec3 sunColor = vec3(1.05, 0.98, 0.88) * uSunIntensity * 1.45;
    vec3 ambientSky = vec3(0.38, 0.48, 0.68) * uAmbientLight * 0.75;

    if (isOpticalTransparentMat(matId)) {
        MatInfo m = fetchMatInfo(uMatLutTex, matId);
        float depth = rData.b * 24.0;

        // Beer-Lambert transmission + volumetric single-scattering
        vec3 extinction = exp(-clamp(m.sigma * depth, vec3(0.0), vec3(10.0)));
        vec3 baseLin = srgbToLinear(m.baseColor);
        vec3 irr = ambientSky + sunColor * sunVis * 0.88 + localLight * 0.95;
        vec3 inScatter = baseLin * irr * (vec3(1.0) - extinction * 0.68) * (m.phase == PHASE_SOLID ? 0.34 : 0.62);
        vec3 col = refracted * extinction + inScatter;

        // 2.5D normal + dynamic waves / facets
        float occL = texture(uSimKinematicsTex, simUv - vec2(uInvSimSize.x, 0.0)).a;
        float occR = texture(uSimKinematicsTex, simUv + vec2(uInvSimSize.x, 0.0)).a;
        float occU = texture(uSimKinematicsTex, simUv - vec2(0.0, uInvSimSize.y)).a;
        float occD = texture(uSimKinematicsTex, simUv + vec2(0.0, uInvSimSize.y)).a;
        vec3 N = normalize(vec3((occL - occR) * 2.6, (occD - occU) * 2.6, 0.72));
        if (m.phase == PHASE_LIQUID) {
            float w1 = sin(simPx.x * 0.36 - uTime * 3.8 + simPx.y * 0.11);
            float w2 = cos(simPx.x * 0.22 + uTime * 2.7 - simPx.y * 0.19);
            N = normalize(N + vec3((w1 + w2) * 0.16, (w1 - w2) * 0.12, 0.0));
        }

        // Subsurface water & acid caustics modulated by FBO_Shadow
)glsl";

constexpr const char *kPass3CompositePart2Chunk = R"glsl(
        if (matId == MAT_WATER || matId == MAT_SALT_WATER || matId == MAT_ACID) {
            vec2 cUv = simPx + uvOffset * uSimSize * 0.6;
            float c1 = sin(cUv.x * 0.24 + cUv.y * 0.15 - uTime * 2.4);
            float c2 = cos(cUv.x * 0.16 - cUv.y * 0.22 + uTime * 1.9);
            float caustic = pow(max(0.0, c1 * c2), 2.0);
            float depthMask = smoothstep(0.0, 1.5, depth) * exp(-depth * 0.075);
            vec3 cTint = (matId == MAT_ACID) ? vec3(0.45, 1.15, 0.20) : vec3(0.32, 0.78, 1.05);
            col += cTint * caustic * depthMask * (0.28 + 0.85 * sunVis * uSunIntensity + 0.25 * length(localLight));
        }

        // Material-specific optical microstructure
        if (matId == MAT_ICE) {
            vec2 vf = voronoiF1F2(simPx * 0.12);
            float crack = 1.0 - smoothstep(0.015, 0.085, vf.y - vf.x);
            float frost = smoothstep(0.90, 0.99, valueNoise(simPx * 1.4 + seed01 * 5.0));
            col += (vec3(0.42, 0.65, 0.85) * crack * 0.45 + vec3(0.35) * frost) * (0.4 + 0.6 * sunVis);
        } else if (matId == MAT_GLASS) {
            float diagGlint = pow(0.5 + 0.5 * sin((simPx.x + simPx.y) * 0.045), 6.0);
            col += vec3(0.45, 0.62, 0.72) * diagGlint * (0.14 + 0.22 * sunVis);
        } else if (matId == MAT_ACID) {
            float bubble = smoothstep(0.86, 0.97, valueNoise(vec2(simPx.x * 0.9, simPx.y * 0.9 + uTime * 4.5)));
            col += vec3(0.28, 0.95, 0.14) * bubble * 0.45 + srgbToLinear(m.emissive) * 0.55;
        } else if (matId == MAT_OIL) {
            float film = simPx.x * 0.16 + simPx.y * 0.11 + uTime * 1.2 + depth * 0.25;
            vec3 irid = 0.5 + 0.5 * cos(vec3(0.0, 2.094, 4.188) + film);
            col += irid * 0.06 * exp(-depth * 0.25);
        } else if (matId == MAT_NITRO) {
            col += srgbToLinear(m.emissive) * 0.45;
        }

        // Schlick Fresnel sky reflection + GGX sun specular
        vec3 V = vec3(0.0, 0.0, 1.0);
        vec3 L = normalize(vec3(-0.46, 0.89, 0.52));
        vec3 H = normalize(L + V);
        float NdotV = max(dot(N, V), 0.001);
        float NdotL = max(dot(N, L), 0.0);
        float NdotH = max(dot(N, H), 0.0);
        float f0 = pow((m.ior - 1.0) / (m.ior + 1.0), 2.0);
        float fresnel = f0 + (1.0 - f0) * pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0);
        vec3 skyRefl = mix(vec3(0.06, 0.11, 0.20), vec3(0.42, 0.62, 0.88), clamp(N.y * 0.5 + 0.5, 0.0, 1.0));
        skyRefl *= (0.35 + 0.85 * uAmbientLight + 0.45 * uSunIntensity * sunVis);
        col = mix(col, skyRefl + localLight * 0.25, clamp(fresnel * 1.35, 0.0, 0.78));

        float a = max(0.05, m.roughness * m.roughness);
        float a2 = a * a;
        float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
        float D = a2 / (3.14159265 * denom * denom);
        col += vec3(1.0) * (D * fresnel * m.specularStrength * 0.28) * (sunColor * NdotL * sunVis + localLight * 0.55);

        if (matId == MAT_GLASS || matId == MAT_ICE) {
            float rimMask = smoothstep(0.05, 0.42, 1.0 - NdotV);
            vec3 rimCol = (matId == MAT_GLASS) ? vec3(0.32, 0.50, 0.62) : vec3(0.40, 0.68, 0.90);
            col += rimCol * rimMask * (0.35 + 0.75 * uSunIntensity * sunVis + 0.35 * length(localLight));
        }

        // Meniscus highlight & aeration foam on open liquid crests
)glsl";

constexpr const char *kPass3CompositePart3Chunk = R"glsl(
        if (m.phase == PHASE_LIQUID) {
            int upMat = decodeMatId(texture(uSimStateTex, clamp(simUv - vec2(0.0, uInvSimSize.y), 0.0, 1.0)).r);
            if (upMat == MAT_AIR || isWispyGasMat(upMat)) {
                float speed = length(vel);
                float waveCrest = 0.5 + 0.5 * sin(simPx.x * 0.42 - uTime * 4.5 + seed01 * 6.0);
                float foam = clamp((speed - 0.30) * 0.55 + (waveCrest - 0.65) * 0.45, 0.0, 0.85);
                vec3 foamCol = vec3(0.75, 0.88, 0.98) * (0.45 + 0.65 * sunVis * uSunIntensity + 0.35 * length(localLight));
                if (matId == MAT_ACID) foamCol *= vec3(0.78, 1.15, 0.62);
                col = mix(col + vec3(0.10, 0.16, 0.24) * (0.5 + 0.5 * sunVis), foamCol, foam * 0.68);
            }
        }

        if (tempK >= 720.0) col += blackbodyHDR(tempK) * 0.75;
        if (charge > 0.04) col += vec3(0.10, 0.48, 1.0) * min(1.3, (charge - 0.04) * 0.45);
        fragColor = vec4(max(col, vec3(0.0)), 1.0);
        return;
    }

    if (isWispyGasMat(matId)) {
        MatInfo m = fetchMatInfo(uMatLutTex, matId);
        vec2 drift = simPx * 0.18 - vel * (uTime * 1.4) + vec2(0.0, uTime * 1.6);
        float wisp = fbm2(drift + vec2(seed01 * 5.0));
        float detail = fbm2(drift * 2.1 + vec2(4.3, 9.1));
        float density = clamp((0.28 + 0.72 * lifeNorm) * smoothstep(0.18, 0.82, wisp * 0.7 + detail * 0.3), 0.0, 0.90);
        float baseAlpha = (matId == MAT_SMOKE) ? 0.76 : ((matId == MAT_STEAM) ? 0.58 : 0.34);
        float alpha = clamp(density * baseAlpha, 0.04, 0.86);

        vec3 gasLin = srgbToLinear(m.baseColor) * (0.85 + 0.3 * (wisp - 0.5));
        vec3 litGas = gasLin * (ambientSky * 1.35 + sunColor * (0.35 + 0.65 * sunVis) + localLight * 1.25);
        vec3 col = mix(refracted, litGas, alpha);
        if (tempK >= 720.0) col += blackbodyHDR(tempK) * 0.60;
        fragColor = vec4(max(col, vec3(0.0)), 1.0);
        return;
    }

    // Air with thermal heat-haze or pressure shockwave
    vec3 col = refracted + vec3(0.14, 0.28, 0.52) * (shock * 0.22);
    fragColor = vec4(max(col, vec3(0.0)), 1.0);
}
)glsl";

// Pass 4: HDR Bloom Pyramid (Prefilter + 13-tap Downsample + 9-tap Tent Upsample)
constexpr const char *kPass4BloomPrefilterFragChunk = R"glsl(
uniform sampler2D uSrcTex;
uniform vec2 uTexelSize;

vec3 karisClamp(vec3 c) {
    return min(max(c, vec3(0.0)), vec3(8.0));
}

float luminance(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 karisAvg4(vec3 a, vec3 b, vec3 c, vec3 d) {
    float wa = 1.0 / (1.0 + luminance(a));
    float wb = 1.0 / (1.0 + luminance(b));
    float wc = 1.0 / (1.0 + luminance(c));
    float wd = 1.0 / (1.0 + luminance(d));
    return (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
}

vec3 softThreshold(vec3 c, float threshold, float knee) {
    float br = max(c.r, max(c.g, c.b));
    float rq = clamp(br - threshold + knee, 0.0, 2.0 * knee);
    rq = (rq * rq) / (4.0 * knee + 1e-5);
    float w = max(rq, br - threshold) / max(br, 1e-5);
    return c * max(w, 0.0);
}

void main() {
    vec2 t = uTexelSize;
    vec3 A = karisClamp(texture(uSrcTex, vUv + t * vec2(-2.0,  2.0)).rgb);
    vec3 B = karisClamp(texture(uSrcTex, vUv + t * vec2( 0.0,  2.0)).rgb);
    vec3 C = karisClamp(texture(uSrcTex, vUv + t * vec2( 2.0,  2.0)).rgb);
    vec3 D = karisClamp(texture(uSrcTex, vUv + t * vec2(-1.0,  1.0)).rgb);
    vec3 E = karisClamp(texture(uSrcTex, vUv + t * vec2( 1.0,  1.0)).rgb);
    vec3 F = karisClamp(texture(uSrcTex, vUv + t * vec2(-2.0,  0.0)).rgb);
    vec3 G = karisClamp(texture(uSrcTex, vUv).rgb);
    vec3 H = karisClamp(texture(uSrcTex, vUv + t * vec2( 2.0,  0.0)).rgb);
    vec3 I = karisClamp(texture(uSrcTex, vUv + t * vec2(-1.0, -1.0)).rgb);
    vec3 J = karisClamp(texture(uSrcTex, vUv + t * vec2( 1.0, -1.0)).rgb);
    vec3 K = karisClamp(texture(uSrcTex, vUv + t * vec2(-2.0, -2.0)).rgb);
    vec3 L = karisClamp(texture(uSrcTex, vUv + t * vec2( 0.0, -2.0)).rgb);
    vec3 M = karisClamp(texture(uSrcTex, vUv + t * vec2( 2.0, -2.0)).rgb);

    vec3 down = karisAvg4(D, E, I, J) * 0.500 +
                karisAvg4(A, B, G, F) * 0.125 +
                karisAvg4(B, C, H, G) * 0.125 +
                karisAvg4(F, G, L, K) * 0.125 +
                karisAvg4(G, H, M, L) * 0.125;

    vec3 outBloom = softThreshold(down, 0.85, 0.45);
    fragColor = vec4(outBloom, 1.0);
}
)glsl";

constexpr const char *kPass4BloomDownsampleFragChunk = R"glsl(
uniform sampler2D uSrcTex;
uniform vec2 uTexelSize;

void main() {
    vec2 t = uTexelSize;
    vec3 A = texture(uSrcTex, vUv + t * vec2(-2.0,  2.0)).rgb;
    vec3 B = texture(uSrcTex, vUv + t * vec2( 0.0,  2.0)).rgb;
    vec3 C = texture(uSrcTex, vUv + t * vec2( 2.0,  2.0)).rgb;
    vec3 D = texture(uSrcTex, vUv + t * vec2(-1.0,  1.0)).rgb;
    vec3 E = texture(uSrcTex, vUv + t * vec2( 1.0,  1.0)).rgb;
    vec3 F = texture(uSrcTex, vUv + t * vec2(-2.0,  0.0)).rgb;
    vec3 G = texture(uSrcTex, vUv).rgb;
    vec3 H = texture(uSrcTex, vUv + t * vec2( 2.0,  0.0)).rgb;
    vec3 I = texture(uSrcTex, vUv + t * vec2(-1.0, -1.0)).rgb;
    vec3 J = texture(uSrcTex, vUv + t * vec2( 1.0, -1.0)).rgb;
    vec3 K = texture(uSrcTex, vUv + t * vec2(-2.0, -2.0)).rgb;
    vec3 L = texture(uSrcTex, vUv + t * vec2( 0.0, -2.0)).rgb;
    vec3 M = texture(uSrcTex, vUv + t * vec2( 2.0, -2.0)).rgb;

    vec3 down = (D + E + I + J) * (0.500 * 0.25) +
                (A + B + G + F) * (0.125 * 0.25) +
                (B + C + H + G) * (0.125 * 0.25) +
                (F + G + L + K) * (0.125 * 0.25) +
                (G + H + M + L) * (0.125 * 0.25);

    fragColor = vec4(max(down, vec3(0.0)), 1.0);
}
)glsl";

constexpr const char *kPass4BloomUpsampleFragChunk = R"glsl(
uniform sampler2D uSrcTex;
uniform vec2 uTexelSize;

void main() {
    vec2 r = uTexelSize * 1.25;
    vec3 sum = texture(uSrcTex, vUv + vec2(-r.x,  r.y)).rgb * 1.0 +
               texture(uSrcTex, vUv + vec2( 0.0,  r.y)).rgb * 2.0 +
               texture(uSrcTex, vUv + vec2( r.x,  r.y)).rgb * 1.0 +
               texture(uSrcTex, vUv + vec2(-r.x,  0.0)).rgb * 2.0 +
               texture(uSrcTex, vUv).rgb * 4.0 +
               texture(uSrcTex, vUv + vec2( r.x,  0.0)).rgb * 2.0 +
               texture(uSrcTex, vUv + vec2(-r.x, -r.y)).rgb * 1.0 +
               texture(uSrcTex, vUv + vec2( 0.0, -r.y)).rgb * 2.0 +
               texture(uSrcTex, vUv + vec2( r.x, -r.y)).rgb * 1.0;

    fragColor = vec4(sum * (1.0 / 16.0), 1.0);
}
)glsl";

// Pass 5: Narkowicz ACES Filmic Tone Mapping + sRGB + 1/255 TPDF Dither
constexpr const char *kPass5ToneMapFragChunk = R"glsl(
uniform sampler2D uLitHDRTex;
uniform sampler2D uBloomTex;
uniform float uBloomIntensity;

float ignDither(vec2 p) {
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(p, magic.xy)));
}

vec3 acesFilmic(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 linearToSrgb(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    vec3 sel = step(vec3(0.0031308), c);
    return mix(lo, hi, sel);
}

void main() {
    vec3 hdr = texture(uLitHDRTex, vUv).rgb;
    if (uBloomIntensity > 0.001) {
        hdr += texture(uBloomTex, vUv).rgb * (uBloomIntensity * 0.42);
    }

    vec3 mapped = acesFilmic(max(hdr * 1.08, vec3(0.0)));
    vec3 srgb = linearToSrgb(mapped);

    // Triangular PDF dither in [-1/255, +1/255]
    float r1 = ignDither(gl_FragCoord.xy);
    float r2 = ignDither(gl_FragCoord.xy + vec2(17.0, 31.0));
    float dither = (r1 + r2 - 1.0) * (1.0 / 255.0);

    fragColor = vec4(clamp(srgb + vec3(dither), 0.0, 1.0), 1.0);
}
)glsl";

// --- GL Resource Helpers ---

struct ScopedGlState {
    const GlProcs &gl;
    GLint prevViewport[4]{};
    GLint prevProgram = 0;
    GLint prevVao = 0;
    GLint prevActiveTex = GL_TEXTURE0;
    std::array<GLint, 7> prevTex2D{};
    std::array<GLint, 7> prevSampler{};
    GLint prevBlendSrcRgb = GL_ONE;
    GLint prevBlendDstRgb = GL_ZERO;
    GLint prevBlendSrcAlpha = GL_ONE;
    GLint prevBlendDstAlpha = GL_ZERO;
    GLint prevBlendEqRgb = GL_FUNC_ADD;
    GLint prevBlendEqAlpha = GL_FUNC_ADD;
    GLboolean prevBlend = GL_FALSE;
    GLboolean prevDepth = GL_FALSE;
    GLboolean prevScissor = GL_FALSE;
    GLboolean prevCull = GL_FALSE;
    GLboolean prevStencil = GL_FALSE;

    explicit ScopedGlState(const GlProcs &procs) noexcept : gl(procs) {
        glGetIntegerv(GL_VIEWPORT, prevViewport);
        glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);

        for (GLuint u = 0; u < 7; ++u) {
            gl.ActiveTexture(GL_TEXTURE0 + u);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex2D[u]);
            if (gl.BindSampler) {
                glGetIntegerv(GL_SAMPLER_BINDING, &prevSampler[u]);
                gl.BindSampler(u, 0);
            }
        }

        glGetIntegerv(GL_BLEND_SRC_RGB, &prevBlendSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &prevBlendDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlendDstAlpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &prevBlendEqRgb);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevBlendEqAlpha);

        prevBlend = glIsEnabled(GL_BLEND);
        prevDepth = glIsEnabled(GL_DEPTH_TEST);
        prevScissor = glIsEnabled(GL_SCISSOR_TEST);
        prevCull = glIsEnabled(GL_CULL_FACE);
        prevStencil = glIsEnabled(GL_STENCIL_TEST);
    }

    ~ScopedGlState() {
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);

        if (prevProgram == 0 || gl.IsProgram(static_cast<GLuint>(prevProgram))) {
            gl.UseProgram(static_cast<GLuint>(prevProgram));
        } else {
            gl.UseProgram(0);
        }
        gl.BindVertexArray(static_cast<GLuint>(prevVao));

        for (GLuint u = 0; u < 7; ++u) {
            gl.ActiveTexture(GL_TEXTURE0 + u);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex2D[u]));
            if (gl.BindSampler) {
                gl.BindSampler(u, static_cast<GLuint>(prevSampler[u]));
            }
        }
        gl.ActiveTexture(static_cast<GLenum>(prevActiveTex));

        gl.BlendEquationSeparate(static_cast<GLenum>(prevBlendEqRgb), static_cast<GLenum>(prevBlendEqAlpha));
        gl.BlendFuncSeparate(static_cast<GLenum>(prevBlendSrcRgb), static_cast<GLenum>(prevBlendDstRgb),
                             static_cast<GLenum>(prevBlendSrcAlpha), static_cast<GLenum>(prevBlendDstAlpha));
        if (prevBlend)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
        if (prevDepth)
            glEnable(GL_DEPTH_TEST);
        else
            glDisable(GL_DEPTH_TEST);
        if (prevScissor)
            glEnable(GL_SCISSOR_TEST);
        else
            glDisable(GL_SCISSOR_TEST);
        if (prevCull)
            glEnable(GL_CULL_FACE);
        else
            glDisable(GL_CULL_FACE);
        if (prevStencil)
            glEnable(GL_STENCIL_TEST);
        else
            glDisable(GL_STENCIL_TEST);

        glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    }
};

GLuint compileShaderChunks(const GlProcs &gl, GLenum type, const char *const *chunks, GLsizei count,
                           const char *label) {
    GLuint shader = gl.CreateShader(type);
    if (shader == 0) {
        std::cerr << "[GlShaderPipeline] glCreateShader failed for " << label << '\n';
        return 0;
    }
    gl.ShaderSource(shader, count, chunks, nullptr);
    gl.CompileShader(shader);

    GLint ok = GL_FALSE;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint logLen = 0;
        gl.GetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLen);
        std::string log(static_cast<size_t>(std::max(1, logLen)), '\0');
        gl.GetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
        std::cerr << "[GlShaderPipeline] Shader compile error (" << label << "):\n" << log.c_str() << '\n';
        gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint linkProgramFromChunks(const GlProcs &gl, GLuint vertShader, const char *const *fragChunks, GLsizei fragCount,
                             const char *label) {
    GLuint fragShader = compileShaderChunks(gl, GL_FRAGMENT_SHADER, fragChunks, fragCount, label);
    if (fragShader == 0) return 0;

    GLuint prog = gl.CreateProgram();
    if (prog == 0) {
        gl.DeleteShader(fragShader);
        std::cerr << "[GlShaderPipeline] glCreateProgram failed for " << label << '\n';
        return 0;
    }

    gl.AttachShader(prog, vertShader);
    gl.AttachShader(prog, fragShader);
    gl.LinkProgram(prog);
    gl.DeleteShader(fragShader);

    GLint ok = GL_FALSE;
    gl.GetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint logLen = 0;
        gl.GetProgramiv(prog, GL_INFO_LOG_LENGTH, &logLen);
        std::string log(static_cast<size_t>(std::max(1, logLen)), '\0');
        gl.GetProgramInfoLog(prog, static_cast<GLsizei>(log.size()), nullptr, log.data());
        std::cerr << "[GlShaderPipeline] Program link error (" << label << "):\n" << log.c_str() << '\n';
        gl.DeleteProgram(prog);
        return 0;
    }
    return prog;
}

bool consumeGlErrors(const char *stage) noexcept {
    bool clean = true;
    for (int i = 0; i < 16; ++i) {
        const GLenum err = glGetError();
        if (err == GL_NO_ERROR) break;
        std::cerr << "[GlShaderPipeline] OpenGL error at " << stage << " (err=0x" << std::hex << err << std::dec
                  << ")\n";
        clean = false;
    }
    return clean;
}

GLuint createTexture2D(int w, int h, GLenum internalFormat, GLenum format, GLenum type, GLint filter,
                       const void *data = nullptr) {
    while (glGetError() != GL_NO_ERROR) {
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (tex == 0) return 0;
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), w, h, 0, format, type, data);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (!consumeGlErrors("createTexture2D")) {
        glDeleteTextures(1, &tex);
        return 0;
    }
    return tex;
}

bool createSingleFbo(const GlProcs &gl, int w, int h, GLenum internalFormat, GLenum type, GLint filter, GLuint &outFbo,
                     GLuint &outTex, const char *label) {
    outTex = createTexture2D(w, h, internalFormat, GL_RGBA, type, filter, nullptr);
    if (outTex == 0) {
        std::cerr << "[GlShaderPipeline] Failed to create texture for FBO " << label << '\n';
        return false;
    }

    gl.GenFramebuffers(1, &outFbo);
    if (outFbo == 0) {
        glDeleteTextures(1, &outTex);
        outTex = 0;
        std::cerr << "[GlShaderPipeline] Failed to create FBO " << label << '\n';
        return false;
    }

    gl.BindFramebuffer(GL_FRAMEBUFFER, outFbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outTex, 0);
    const GLenum drawBuf = GL_COLOR_ATTACHMENT0;
    gl.DrawBuffers(1, &drawBuf);

    const GLenum status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE || !consumeGlErrors(label)) {
        std::cerr << "[GlShaderPipeline] Incomplete FBO " << label << " (status=0x" << std::hex << status << std::dec
                  << ")\n";
        gl.DeleteFramebuffers(1, &outFbo);
        glDeleteTextures(1, &outTex);
        outFbo = 0;
        outTex = 0;
        return false;
    }
    return true;
}

bool createSceneMrtFbo(const GlProcs &gl, int w, int h, GLuint &outFbo, GLuint &outSceneTex, GLuint &outRefractTex) {
    outSceneTex = createTexture2D(w, h, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, GL_LINEAR, nullptr);
    outRefractTex = createTexture2D(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR, nullptr);
    if (outSceneTex == 0 || outRefractTex == 0) {
        if (outSceneTex) glDeleteTextures(1, &outSceneTex);
        if (outRefractTex) glDeleteTextures(1, &outRefractTex);
        outSceneTex = 0;
        outRefractTex = 0;
        std::cerr << "[GlShaderPipeline] Failed to create MRT textures for FBO_Scene\n";
        return false;
    }

    gl.GenFramebuffers(1, &outFbo);
    if (outFbo == 0) {
        glDeleteTextures(1, &outSceneTex);
        glDeleteTextures(1, &outRefractTex);
        outSceneTex = 0;
        outRefractTex = 0;
        std::cerr << "[GlShaderPipeline] Failed to create FBO_Scene\n";
        return false;
    }

    gl.BindFramebuffer(GL_FRAMEBUFFER, outFbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outSceneTex, 0);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, outRefractTex, 0);
    const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    gl.DrawBuffers(2, bufs);

    const GLenum status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE || !consumeGlErrors("FBO_Scene")) {
        std::cerr << "[GlShaderPipeline] Incomplete MRT FBO_Scene (status=0x" << std::hex << status << std::dec
                  << ")\n";
        gl.DeleteFramebuffers(1, &outFbo);
        glDeleteTextures(1, &outSceneTex);
        glDeleteTextures(1, &outRefractTex);
        outFbo = 0;
        outSceneTex = 0;
        outRefractTex = 0;
        return false;
    }
    return true;
}

void destroyFboAndTex(const GlProcs &gl, GLuint &fbo, GLuint &tex) noexcept {
    if (fbo != 0 && gl.DeleteFramebuffers) {
        gl.DeleteFramebuffers(1, &fbo);
        fbo = 0;
    }
    if (tex != 0) {
        glDeleteTextures(1, &tex);
        tex = 0;
    }
}

void bindTexUnit(const GlProcs &gl, GLuint unit, GLuint tex) noexcept {
    gl.ActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
}

} // namespace

struct GlShaderPipeline::Impl {
    GlProcs gl{};
    bool ready = false;
    int maxTexSize = 4096;

    GLuint emptyVao = 0;
    GLuint texMatLut = 0;

    // Dynamic SimWorld textures
    GLuint texSimState = 0;
    GLuint texSimLight = 0;
    GLuint texSimKinematics = 0;
    int simW = 0;
    int simH = 0;
    uint64_t uploadedTick = 0;
    bool hasUploadedOnce = false;

    std::vector<uint8_t> statePackBuf;
    std::vector<uint8_t> lightPackBuf;
    std::vector<uint8_t> kinPackBuf;

    // Canvas-sized FBOs & textures
    int canvasW = 0;
    int canvasH = 0;
    int shadowW = 0;
    int shadowH = 0;
    int halfW = 0;
    int halfH = 0;
    int quarterW = 0;
    int quarterH = 0;
    int eighthW = 0;
    int eighthH = 0;

    GLuint fboShadowRaw = 0;
    GLuint texShadowRaw = 0;
    GLuint fboShadow = 0;
    GLuint texShadow = 0;

    GLuint fboScene = 0;
    GLuint texScene = 0;
    GLuint texRefractData = 0;

    GLuint fboLitHDR = 0;
    GLuint texLitHDR = 0;

    GLuint fboBloomHalf = 0;
    GLuint texBloomHalf = 0;
    GLuint fboBloomQuarter = 0;
    GLuint texBloomQuarter = 0;
    GLuint fboBloomEighth = 0;
    GLuint texBloomEighth = 0;

    GLuint fboFinal = 0;
    GLuint texFinal = 0;

    // Shader programs & cached uniform locations
    GLuint progShadow = 0;
    struct {
        GLint simStateTex = -1;
        GLint simLightTex = -1;
        GLint simKinematicsTex = -1;
        GLint matLutTex = -1;
        GLint simSize = -1;
        GLint invSimSize = -1;
        GLint shadowsEnabled = -1;
        GLint sunIntensity = -1;
    } uShadow;

    GLuint progShadowSmooth = 0;
    struct {
        GLint shadowRawTex = -1;
        GLint simKinematicsTex = -1;
        GLint shadowTexelSize = -1;
        GLint shadowsEnabled = -1;
    } uShadowSmooth;

    GLuint progScene = 0;
    struct {
        GLint simStateTex = -1;
        GLint simLightTex = -1;
        GLint simKinematicsTex = -1;
        GLint matLutTex = -1;
        GLint shadowTex = -1;
        GLint simSize = -1;
        GLint invSimSize = -1;
        GLint canvasSize = -1;
        GLint time = -1;
        GLint ambientLight = -1;
        GLint sunIntensity = -1;
    } uScene;

    GLuint progComposite = 0;
    struct {
        GLint sceneTex = -1;
        GLint refractTex = -1;
        GLint shadowTex = -1;
        GLint simStateTex = -1;
        GLint simLightTex = -1;
        GLint simKinematicsTex = -1;
        GLint matLutTex = -1;
        GLint simSize = -1;
        GLint invSimSize = -1;
        GLint time = -1;
        GLint ambientLight = -1;
        GLint sunIntensity = -1;
        GLint refractionEnabled = -1;
    } uComposite;

    GLuint progBloomPrefilter = 0;
    struct {
        GLint srcTex = -1;
        GLint texelSize = -1;
    } uBloomPrefilter;

    GLuint progBloomDown = 0;
    struct {
        GLint srcTex = -1;
        GLint texelSize = -1;
    } uBloomDown;

    GLuint progBloomUp = 0;
    struct {
        GLint srcTex = -1;
        GLint texelSize = -1;
    } uBloomUp;

    GLuint progToneMap = 0;
    struct {
        GLint litHDRTex = -1;
        GLint bloomTex = -1;
        GLint bloomIntensity = -1;
    } uToneMap;

    void destroyCanvasFbos() noexcept {
        destroyFboAndTex(gl, fboShadowRaw, texShadowRaw);
        destroyFboAndTex(gl, fboShadow, texShadow);
        if (fboScene != 0 && gl.DeleteFramebuffers) {
            gl.DeleteFramebuffers(1, &fboScene);
            fboScene = 0;
        }
        if (texScene != 0) {
            glDeleteTextures(1, &texScene);
            texScene = 0;
        }
        if (texRefractData != 0) {
            glDeleteTextures(1, &texRefractData);
            texRefractData = 0;
        }
        destroyFboAndTex(gl, fboLitHDR, texLitHDR);
        destroyFboAndTex(gl, fboBloomHalf, texBloomHalf);
        destroyFboAndTex(gl, fboBloomQuarter, texBloomQuarter);
        destroyFboAndTex(gl, fboBloomEighth, texBloomEighth);
        destroyFboAndTex(gl, fboFinal, texFinal);
        canvasW = 0;
        canvasH = 0;
        shadowW = 0;
        shadowH = 0;
        halfW = 0;
        halfH = 0;
        quarterW = 0;
        quarterH = 0;
        eighthW = 0;
        eighthH = 0;
    }

    bool ensureCanvasFbos(int targetW, int targetH) {
        const int cw = std::clamp(targetW, 1, maxTexSize);
        const int ch = std::clamp(targetH, 1, maxTexSize);
        if (cw == canvasW && ch == canvasH && fboFinal != 0) {
            return true;
        }

        destroyCanvasFbos();

        shadowW = std::max(1, std::min(cw, 640));
        shadowH = std::max(1, std::min(ch, 360));
        halfW = std::max(1, cw / 2);
        halfH = std::max(1, ch / 2);
        quarterW = std::max(1, cw / 4);
        quarterH = std::max(1, ch / 4);
        eighthW = std::max(1, cw / 8);
        eighthH = std::max(1, ch / 8);

        bool ok = true;
        ok = ok && createSingleFbo(gl, shadowW, shadowH, GL_RGBA16F, GL_HALF_FLOAT, GL_LINEAR, fboShadowRaw,
                                   texShadowRaw, "FBO_ShadowRaw");
        ok = ok && createSingleFbo(gl, shadowW, shadowH, GL_RGBA16F, GL_HALF_FLOAT, GL_LINEAR, fboShadow, texShadow,
                                   "FBO_Shadow");
        ok = ok && createSceneMrtFbo(gl, cw, ch, fboScene, texScene, texRefractData);
        ok =
            ok && createSingleFbo(gl, cw, ch, GL_RGBA16F, GL_HALF_FLOAT, GL_LINEAR, fboLitHDR, texLitHDR, "FBO_LitHDR");
        ok = ok && createSingleFbo(gl, halfW, halfH, GL_RGBA16F, GL_HALF_FLOAT, GL_LINEAR, fboBloomHalf, texBloomHalf,
                                   "FBO_BloomHalf");
        ok = ok && createSingleFbo(gl, quarterW, quarterH, GL_RGBA16F, GL_HALF_FLOAT, GL_LINEAR, fboBloomQuarter,
                                   texBloomQuarter, "FBO_BloomQuarter");
        ok = ok && createSingleFbo(gl, eighthW, eighthH, GL_RGBA16F, GL_HALF_FLOAT, GL_LINEAR, fboBloomEighth,
                                   texBloomEighth, "FBO_BloomEighth");
        ok = ok && createSingleFbo(gl, cw, ch, GL_RGBA8, GL_UNSIGNED_BYTE, GL_LINEAR, fboFinal, texFinal, "FBO_Final");

        if (!ok) {
            destroyCanvasFbos();
            return false;
        }
        canvasW = cw;
        canvasH = ch;
        return true;
    }

    bool uploadMaterialLut() {
        const auto &mats = getAllMaterials();
        const auto &optics = getAllMaterialOptics();
        std::array<float, 32 * 4 * 4> lut{};

        for (size_t i = 0; i < kMaterialCount; ++i) {
            const auto &m = mats[i];
            const auto &o = optics[i];

            const size_t row0 = (0 * 32 + i) * 4;
            lut[row0 + 0] = static_cast<float>(m.baseR) + static_cast<float>(m.baseG) * 256.0f +
                            static_cast<float>(static_cast<int>(m.phase)) * 65536.0f;
            lut[row0 + 1] = static_cast<float>(m.baseB) + static_cast<float>(m.colorVariance) * 256.0f;
            lut[row0 + 2] = o.ior;
            lut[row0 + 3] = o.refractStrength;

            const size_t row1 = (1 * 32 + i) * 4;
            lut[row1 + 0] = m.emissiveR;
            lut[row1 + 1] = m.emissiveG;
            lut[row1 + 2] = m.emissiveB;
            lut[row1 + 3] = o.detailScale;

            const size_t row2 = (2 * 32 + i) * 4;
            lut[row2 + 0] = o.sigmaR;
            lut[row2 + 1] = o.sigmaG;
            lut[row2 + 2] = o.sigmaB;
            lut[row2 + 3] = o.shadowOpacity;

            const size_t row3 = (3 * 32 + i) * 4;
            lut[row3 + 0] = o.translucency;
            lut[row3 + 1] = o.roughness;
            lut[row3 + 2] = o.metallic;
            lut[row3 + 3] = o.specularStrength;
        }

        GLint prevAlign = 4;
        GLint prevRowLen = 0;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prevRowLen);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

        texMatLut = createTexture2D(32, 4, GL_RGBA32F, GL_RGBA, GL_FLOAT, GL_NEAREST, lut.data());

        glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, prevRowLen);
        return texMatLut != 0;
    }

    bool compileAllShaders() {
        const char *vertChunks[] = {kFullscreenVertChunk};
        GLuint vs = compileShaderChunks(gl, GL_VERTEX_SHADER, vertChunks, 1, "fullscreen.vert");
        if (vs == 0) return false;

        auto cleanupVs = [&]() { gl.DeleteShader(vs); };

        const char *shadowChunks[] = {kFragHeaderSingleChunk, kGlslLutChunk, kGlslNoiseAndColorChunk,
                                      kPass1ShadowFragChunk, kPass1ShadowPart2Chunk};
        progShadow = linkProgramFromChunks(gl, vs, shadowChunks, 5, "pass1_shadow");
        if (progShadow == 0) {
            cleanupVs();
            return false;
        }

        const char *smoothChunks[] = {kFragHeaderSingleChunk, kPass1TentSmoothFragChunk};
        progShadowSmooth = linkProgramFromChunks(gl, vs, smoothChunks, 2, "pass1_tent_smooth");
        if (progShadowSmooth == 0) {
            cleanupVs();
            return false;
        }

        const char *sceneChunks[] = {kFragHeaderMrtChunk,       kGlslLutChunk,           kGlslNoiseAndColorChunk,
                                     kGlslLightingHelpersChunk, kPass2MaterialsChunk,    kPass2ShadingChunk,
                                     kPass2ShadingPart2Chunk,   kPass2ShadingPart3Chunk, kPass2MainChunk,
                                     kPass2MainPart2Chunk};
        progScene = linkProgramFromChunks(gl, vs, sceneChunks, 10, "pass2_scene_mrt");
        if (progScene == 0) {
            cleanupVs();
            return false;
        }

        const char *compChunks[] = {kFragHeaderSingleChunk,    kGlslLutChunk,
                                    kGlslNoiseAndColorChunk,   kPass3CompositeMainChunk,
                                    kPass3CompositePart2Chunk, kPass3CompositePart3Chunk};
        progComposite = linkProgramFromChunks(gl, vs, compChunks, 6, "pass3_composite");
        if (progComposite == 0) {
            cleanupVs();
            return false;
        }

        const char *prefChunks[] = {kFragHeaderSingleChunk, kPass4BloomPrefilterFragChunk};
        progBloomPrefilter = linkProgramFromChunks(gl, vs, prefChunks, 2, "pass4_bloom_prefilter");
        if (progBloomPrefilter == 0) {
            cleanupVs();
            return false;
        }

        const char *downChunks[] = {kFragHeaderSingleChunk, kPass4BloomDownsampleFragChunk};
        progBloomDown = linkProgramFromChunks(gl, vs, downChunks, 2, "pass4_bloom_downsample");
        if (progBloomDown == 0) {
            cleanupVs();
            return false;
        }

        const char *upChunks[] = {kFragHeaderSingleChunk, kPass4BloomUpsampleFragChunk};
        progBloomUp = linkProgramFromChunks(gl, vs, upChunks, 2, "pass4_bloom_upsample");
        if (progBloomUp == 0) {
            cleanupVs();
            return false;
        }

        const char *toneChunks[] = {kFragHeaderSingleChunk, kPass5ToneMapFragChunk};
        progToneMap = linkProgramFromChunks(gl, vs, toneChunks, 2, "pass5_tonemap");
        cleanupVs();
        if (progToneMap == 0) return false;

        cacheUniformLocations();
        return true;
    }

    void cacheUniformLocations() noexcept {
        uShadow.simStateTex = gl.GetUniformLocation(progShadow, "uSimStateTex");
        uShadow.simLightTex = gl.GetUniformLocation(progShadow, "uSimLightTex");
        uShadow.simKinematicsTex = gl.GetUniformLocation(progShadow, "uSimKinematicsTex");
        uShadow.matLutTex = gl.GetUniformLocation(progShadow, "uMatLutTex");
        uShadow.simSize = gl.GetUniformLocation(progShadow, "uSimSize");
        uShadow.invSimSize = gl.GetUniformLocation(progShadow, "uInvSimSize");
        uShadow.shadowsEnabled = gl.GetUniformLocation(progShadow, "uShadowsEnabled");
        uShadow.sunIntensity = gl.GetUniformLocation(progShadow, "uSunIntensity");

        uShadowSmooth.shadowRawTex = gl.GetUniformLocation(progShadowSmooth, "uShadowRawTex");
        uShadowSmooth.simKinematicsTex = gl.GetUniformLocation(progShadowSmooth, "uSimKinematicsTex");
        uShadowSmooth.shadowTexelSize = gl.GetUniformLocation(progShadowSmooth, "uShadowTexelSize");
        uShadowSmooth.shadowsEnabled = gl.GetUniformLocation(progShadowSmooth, "uShadowsEnabled");

        uScene.simStateTex = gl.GetUniformLocation(progScene, "uSimStateTex");
        uScene.simLightTex = gl.GetUniformLocation(progScene, "uSimLightTex");
        uScene.simKinematicsTex = gl.GetUniformLocation(progScene, "uSimKinematicsTex");
        uScene.matLutTex = gl.GetUniformLocation(progScene, "uMatLutTex");
        uScene.shadowTex = gl.GetUniformLocation(progScene, "uShadowTex");
        uScene.simSize = gl.GetUniformLocation(progScene, "uSimSize");
        uScene.invSimSize = gl.GetUniformLocation(progScene, "uInvSimSize");
        uScene.canvasSize = gl.GetUniformLocation(progScene, "uCanvasSize");
        uScene.time = gl.GetUniformLocation(progScene, "uTime");
        uScene.ambientLight = gl.GetUniformLocation(progScene, "uAmbientLight");
        uScene.sunIntensity = gl.GetUniformLocation(progScene, "uSunIntensity");

        uComposite.sceneTex = gl.GetUniformLocation(progComposite, "uSceneTex");
        uComposite.refractTex = gl.GetUniformLocation(progComposite, "uRefractTex");
        uComposite.shadowTex = gl.GetUniformLocation(progComposite, "uShadowTex");
        uComposite.simStateTex = gl.GetUniformLocation(progComposite, "uSimStateTex");
        uComposite.simLightTex = gl.GetUniformLocation(progComposite, "uSimLightTex");
        uComposite.simKinematicsTex = gl.GetUniformLocation(progComposite, "uSimKinematicsTex");
        uComposite.matLutTex = gl.GetUniformLocation(progComposite, "uMatLutTex");
        uComposite.simSize = gl.GetUniformLocation(progComposite, "uSimSize");
        uComposite.invSimSize = gl.GetUniformLocation(progComposite, "uInvSimSize");
        uComposite.time = gl.GetUniformLocation(progComposite, "uTime");
        uComposite.ambientLight = gl.GetUniformLocation(progComposite, "uAmbientLight");
        uComposite.sunIntensity = gl.GetUniformLocation(progComposite, "uSunIntensity");
        uComposite.refractionEnabled = gl.GetUniformLocation(progComposite, "uRefractionEnabled");

        uBloomPrefilter.srcTex = gl.GetUniformLocation(progBloomPrefilter, "uSrcTex");
        uBloomPrefilter.texelSize = gl.GetUniformLocation(progBloomPrefilter, "uTexelSize");

        uBloomDown.srcTex = gl.GetUniformLocation(progBloomDown, "uSrcTex");
        uBloomDown.texelSize = gl.GetUniformLocation(progBloomDown, "uTexelSize");

        uBloomUp.srcTex = gl.GetUniformLocation(progBloomUp, "uSrcTex");
        uBloomUp.texelSize = gl.GetUniformLocation(progBloomUp, "uTexelSize");

        uToneMap.litHDRTex = gl.GetUniformLocation(progToneMap, "uLitHDRTex");
        uToneMap.bloomTex = gl.GetUniformLocation(progToneMap, "uBloomTex");
        uToneMap.bloomIntensity = gl.GetUniformLocation(progToneMap, "uBloomIntensity");
    }

    bool uploadSimTexturesIfNeeded(const SimWorld &world, bool dataDirty) {
        const int W = world.width();
        const int H = world.height();
        if (W <= 0 || H <= 0) return false;

        const uint64_t tick = world.stats().tick;
        const bool sizeChanged = (W != simW || H != simH);
        const bool needUpload = !hasUploadedOnce || sizeChanged || dataDirty || (uploadedTick != tick);
        if (!needUpload) return true;

        const uint8_t *matPtr = world.materialData();
        const uint8_t *seedPtr = world.colorSeedData();
        const float *tempPtr = world.tempData();
        const float *lifePtr = world.lifeData();
        const float *chargePtr = world.chargeData();
        const float *pressPtr = world.pressureData();
        const float *vxPtr = world.velocityXData();
        const float *vyPtr = world.velocityYData();
        const float *airVxPtr = world.airVelocityXData();
        const float *airVyPtr = world.airVelocityYData();
        const float *lightRPtr = world.lightRData();
        const float *lightGPtr = world.lightGData();
        const float *lightBPtr = world.lightBData();

        if (!matPtr || !seedPtr || !tempPtr || !lifePtr || !chargePtr || !pressPtr || !vxPtr || !vyPtr || !airVxPtr ||
            !airVyPtr || !lightRPtr || !lightGPtr || !lightBPtr) {
            return false;
        }

        const size_t n = static_cast<size_t>(W) * static_cast<size_t>(H);
        statePackBuf.resize(n * 4);
        lightPackBuf.resize(n * 4);
        kinPackBuf.resize(n * 4);

        uint8_t *dstState = statePackBuf.data();
        uint8_t *dstLight = lightPackBuf.data();
        uint8_t *dstKin = kinPackBuf.data();

        for (size_t i = 0; i < n; ++i) {
            const uint8_t rawMat = matPtr[i];
            const uint8_t mIdx = (rawMat < kMaterialCount) ? rawMat : 0u;

            const float rawT = tempPtr[i];
            const float tClamped = (rawT > 150.0f) ? std::min(rawT, 5000.0f) : 150.0f;
            const float tNorm = (std::sqrt(tClamped) - kMinTempSqrt) * kInvTempSqrtSpan;

            const size_t o = i * 4;
            dstState[o + 0] = mIdx;
            dstState[o + 1] = seedPtr[i];
            dstState[o + 2] = floatToByte01(tNorm);
            dstState[o + 3] = floatToByte01(lifePtr[i] * kLifeScaleLUT[mIdx]);

            dstLight[o + 0] = floatToByte01(lightRPtr[i] * 0.4f);
            dstLight[o + 1] = floatToByte01(lightGPtr[i] * 0.4f);
            dstLight[o + 2] = floatToByte01(lightBPtr[i] * 0.4f);
            const float qVal = std::isfinite(chargePtr[i]) ? std::abs(chargePtr[i]) * 0.25f : 0.0f;
            dstLight[o + 3] = floatToByte01(qVal);

            const bool mov = (kMovableLUT[mIdx] != 0u);
            const float vx = mov ? vxPtr[i] : airVxPtr[i];
            const float vy = mov ? vyPtr[i] : airVyPtr[i];
            const float pAbs = std::isfinite(pressPtr[i]) ? std::abs(pressPtr[i]) : 0.0f;
            dstKin[o + 0] = encodeVelocityByte(vx);
            dstKin[o + 1] = encodeVelocityByte(vy);
            dstKin[o + 2] = floatToByte01((pAbs - 0.35f) * 0.22f);
            dstKin[o + 3] = kOccupancyLUT[mIdx];
        }

        GLint prevAlign = 4;
        GLint prevRowLen = 0;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prevRowLen);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

        gl.ActiveTexture(GL_TEXTURE0);
        if (sizeChanged || texSimState == 0 || texSimLight == 0 || texSimKinematics == 0) {
            if (texSimState) glDeleteTextures(1, &texSimState);
            if (texSimLight) glDeleteTextures(1, &texSimLight);
            if (texSimKinematics) glDeleteTextures(1, &texSimKinematics);
            texSimState = createTexture2D(W, H, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_NEAREST, statePackBuf.data());
            texSimLight = createTexture2D(W, H, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR, lightPackBuf.data());
            texSimKinematics = createTexture2D(W, H, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR, kinPackBuf.data());
            if (texSimState == 0 || texSimLight == 0 || texSimKinematics == 0) {
                if (texSimState) glDeleteTextures(1, &texSimState);
                if (texSimLight) glDeleteTextures(1, &texSimLight);
                if (texSimKinematics) glDeleteTextures(1, &texSimKinematics);
                texSimState = 0;
                texSimLight = 0;
                texSimKinematics = 0;
                simW = 0;
                simH = 0;
            } else {
                simW = W;
                simH = H;
            }
        } else {
            glBindTexture(GL_TEXTURE_2D, texSimState);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, statePackBuf.data());
            glBindTexture(GL_TEXTURE_2D, texSimLight);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, lightPackBuf.data());
            glBindTexture(GL_TEXTURE_2D, texSimKinematics);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, kinPackBuf.data());
        }
        glBindTexture(GL_TEXTURE_2D, 0);

        glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, prevRowLen);

        if (texSimState == 0 || texSimLight == 0 || texSimKinematics == 0) {
            return false;
        }
        uploadedTick = tick;
        hasUploadedOnce = true;
        return true;
    }

    void releaseAllGlResources() noexcept {
        if (SDL_GL_GetCurrentContext() == nullptr) {
            // GL context already destroyed; reset handles safely without invoking driver calls.
            ready = false;
            emptyVao = 0;
            texMatLut = 0;
            texSimState = 0;
            texSimLight = 0;
            texSimKinematics = 0;
            fboShadowRaw = texShadowRaw = 0;
            fboShadow = texShadow = 0;
            fboScene = texScene = texRefractData = 0;
            fboLitHDR = texLitHDR = 0;
            fboBloomHalf = texBloomHalf = 0;
            fboBloomQuarter = texBloomQuarter = 0;
            fboBloomEighth = texBloomEighth = 0;
            fboFinal = texFinal = 0;
            progShadow = progShadowSmooth = progScene = progComposite = 0;
            progBloomPrefilter = progBloomDown = progBloomUp = progToneMap = 0;
            gl.reset();
            return;
        }

        destroyCanvasFbos();

        if (texMatLut != 0) {
            glDeleteTextures(1, &texMatLut);
            texMatLut = 0;
        }
        if (texSimState != 0) {
            glDeleteTextures(1, &texSimState);
            texSimState = 0;
        }
        if (texSimLight != 0) {
            glDeleteTextures(1, &texSimLight);
            texSimLight = 0;
        }
        if (texSimKinematics != 0) {
            glDeleteTextures(1, &texSimKinematics);
            texSimKinematics = 0;
        }

        auto delProg = [&](GLuint &p) {
            if (p != 0 && gl.DeleteProgram) {
                gl.DeleteProgram(p);
                p = 0;
            }
        };
        delProg(progShadow);
        delProg(progShadowSmooth);
        delProg(progScene);
        delProg(progComposite);
        delProg(progBloomPrefilter);
        delProg(progBloomDown);
        delProg(progBloomUp);
        delProg(progToneMap);

        if (emptyVao != 0 && gl.DeleteVertexArrays) {
            gl.DeleteVertexArrays(1, &emptyVao);
            emptyVao = 0;
        }

        simW = 0;
        simH = 0;
        uploadedTick = 0;
        hasUploadedOnce = false;
        ready = false;
        gl.reset();
    }
};

GlShaderPipeline::GlShaderPipeline() : m_impl(std::make_unique<Impl>()) {}

GlShaderPipeline::~GlShaderPipeline() {
    shutdown();
}

bool GlShaderPipeline::initialize() {
    if (m_impl->ready) return true;

    if (SDL_GL_GetCurrentContext() == nullptr) {
        std::cerr << "[GlShaderPipeline] initialize() called without a current OpenGL context\n";
        return false;
    }

    // Drain any pre-existing GL error flags before probing version
    while (glGetError() != GL_NO_ERROR) {
    }

    GLint majorVer = 0;
    GLint minorVer = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &majorVer);
    glGetIntegerv(GL_MINOR_VERSION, &minorVer);
    if (glGetError() != GL_NO_ERROR || (majorVer * 10 + minorVer) < 33) {
        std::cerr << "[GlShaderPipeline] OpenGL 3.3 Core required (got " << majorVer << '.' << minorVer << ")\n";
        return false;
    }

    if (!m_impl->gl.loadAll()) {
        std::cerr << "[GlShaderPipeline] Failed to load required OpenGL 3.3 Core functions\n";
        m_impl->releaseAllGlResources();
        return false;
    }

    GLint maxSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    m_impl->maxTexSize = std::max(1024, maxSize);

    bool initOk = true;
    {
        ScopedGlState stateGuard(m_impl->gl);

        m_impl->gl.GenVertexArrays(1, &m_impl->emptyVao);
        if (m_impl->emptyVao == 0 || !consumeGlErrors("GenVertexArrays")) {
            std::cerr << "[GlShaderPipeline] Failed to create fullscreen VAO\n";
            initOk = false;
        } else if (!m_impl->uploadMaterialLut()) {
            std::cerr << "[GlShaderPipeline] Failed to upload MaterialOptics LUT texture\n";
            initOk = false;
        } else if (!m_impl->compileAllShaders() || !consumeGlErrors("compileAllShaders")) {
            initOk = false;
        } else if (!m_impl->ensureCanvasFbos(640, 360)) {
            // Validate full FBO chain (RGBA16F MRT + bloom pyramid + RGBA8 final) at initialization
            initOk = false;
        } else if (!consumeGlErrors("initialize")) {
            initOk = false;
        }
    }

    if (!initOk) {
        m_impl->releaseAllGlResources();
        return false;
    }

    m_impl->ready = true;
    return true;
}

void GlShaderPipeline::shutdown() {
    if (!m_impl) return;
    m_impl->releaseAllGlResources();
}

bool GlShaderPipeline::isReady() const noexcept {
    return m_impl && m_impl->ready;
}

uint64_t GlShaderPipeline::outputTextureId() const noexcept {
    if (!m_impl || !m_impl->ready) return 0;
    return static_cast<uint64_t>(m_impl->texFinal);
}

bool GlShaderPipeline::render(const SimWorld &world, const GraphicsSettings &settings, int canvasPixelW,
                              int canvasPixelH, float timeSec, bool dataDirty) {
    if (!m_impl || !m_impl->ready) return false;
    if (canvasPixelW <= 0 || canvasPixelH <= 0) return false;

    const GlProcs &gl = m_impl->gl;
    bool fboFailed = false;
    {
        ScopedGlState stateGuard(gl);

        if (!m_impl->uploadSimTexturesIfNeeded(world, dataDirty)) {
            return false;
        }

        if (!m_impl->ensureCanvasFbos(canvasPixelW, canvasPixelH)) {
            fboFailed = true;
        } else {
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_BLEND);

            gl.BindVertexArray(m_impl->emptyVao);

            const float fSimW = static_cast<float>(m_impl->simW);
            const float fSimH = static_cast<float>(m_impl->simH);
            const float invSimW = 1.0f / fSimW;
            const float invSimH = 1.0f / fSimH;
            const float fCanvasW = static_cast<float>(m_impl->canvasW);
            const float fCanvasH = static_cast<float>(m_impl->canvasH);
            const float ambient = std::clamp(settings.ambientLight, 0.05f, 1.0f);
            const float sun = std::clamp(settings.sunIntensity, 0.0f, 1.5f);
            const float bloomAmt = settings.bloomEnabled ? std::clamp(settings.bloomIntensity, 0.0f, 1.5f) : 0.0f;
            const int shadowsOn = settings.shadowsEnabled ? 1 : 0;
            const int refractOn = settings.refractionEnabled ? 1 : 0;

            // --- Pass 1a: Cast Shadows & 2D Local Light Raymarch ---
            gl.BindFramebuffer(GL_FRAMEBUFFER, m_impl->fboShadowRaw);
            glViewport(0, 0, m_impl->shadowW, m_impl->shadowH);
            gl.UseProgram(m_impl->progShadow);
            bindTexUnit(gl, 0, m_impl->texSimState);
            bindTexUnit(gl, 1, m_impl->texSimLight);
            bindTexUnit(gl, 2, m_impl->texSimKinematics);
            bindTexUnit(gl, 3, m_impl->texMatLut);
            gl.Uniform1i(m_impl->uShadow.simStateTex, 0);
            gl.Uniform1i(m_impl->uShadow.simLightTex, 1);
            gl.Uniform1i(m_impl->uShadow.simKinematicsTex, 2);
            gl.Uniform1i(m_impl->uShadow.matLutTex, 3);
            gl.Uniform2f(m_impl->uShadow.simSize, fSimW, fSimH);
            gl.Uniform2f(m_impl->uShadow.invSimSize, invSimW, invSimH);
            gl.Uniform1i(m_impl->uShadow.shadowsEnabled, shadowsOn);
            gl.Uniform1f(m_impl->uShadow.sunIntensity, sun);
            glDrawArrays(GL_TRIANGLES, 0, 3);

            // --- Pass 1b: Bilateral Tent Smooth on Shadow Buffer ---
            gl.BindFramebuffer(GL_FRAMEBUFFER, m_impl->fboShadow);
            glViewport(0, 0, m_impl->shadowW, m_impl->shadowH);
            gl.UseProgram(m_impl->progShadowSmooth);
            bindTexUnit(gl, 0, m_impl->texShadowRaw);
            bindTexUnit(gl, 1, m_impl->texSimKinematics);
            gl.Uniform1i(m_impl->uShadowSmooth.shadowRawTex, 0);
            gl.Uniform1i(m_impl->uShadowSmooth.simKinematicsTex, 1);
            gl.Uniform2f(m_impl->uShadowSmooth.shadowTexelSize, 1.0f / static_cast<float>(m_impl->shadowW),
                         1.0f / static_cast<float>(m_impl->shadowH));
            gl.Uniform1i(m_impl->uShadowSmooth.shadowsEnabled, shadowsOn);
            glDrawArrays(GL_TRIANGLES, 0, 3);

            // --- Pass 2: Scene & Refraction G-Buffer MRT ---
            gl.BindFramebuffer(GL_FRAMEBUFFER, m_impl->fboScene);
            glViewport(0, 0, m_impl->canvasW, m_impl->canvasH);
            gl.UseProgram(m_impl->progScene);
            bindTexUnit(gl, 0, m_impl->texSimState);
            bindTexUnit(gl, 1, m_impl->texSimLight);
            bindTexUnit(gl, 2, m_impl->texSimKinematics);
            bindTexUnit(gl, 3, m_impl->texMatLut);
            bindTexUnit(gl, 4, m_impl->texShadow);
            gl.Uniform1i(m_impl->uScene.simStateTex, 0);
            gl.Uniform1i(m_impl->uScene.simLightTex, 1);
            gl.Uniform1i(m_impl->uScene.simKinematicsTex, 2);
            gl.Uniform1i(m_impl->uScene.matLutTex, 3);
            gl.Uniform1i(m_impl->uScene.shadowTex, 4);
            gl.Uniform2f(m_impl->uScene.simSize, fSimW, fSimH);
            gl.Uniform2f(m_impl->uScene.invSimSize, invSimW, invSimH);
            gl.Uniform2f(m_impl->uScene.canvasSize, fCanvasW, fCanvasH);
            gl.Uniform1f(m_impl->uScene.time, timeSec);
            gl.Uniform1f(m_impl->uScene.ambientLight, ambient);
            gl.Uniform1f(m_impl->uScene.sunIntensity, sun);
            glDrawArrays(GL_TRIANGLES, 0, 3);

            // --- Pass 3: Optical Refraction, Beer-Lambert, Caustics, Fresnel & Gases ---
            gl.BindFramebuffer(GL_FRAMEBUFFER, m_impl->fboLitHDR);
            glViewport(0, 0, m_impl->canvasW, m_impl->canvasH);
            gl.UseProgram(m_impl->progComposite);
            bindTexUnit(gl, 0, m_impl->texScene);
            bindTexUnit(gl, 1, m_impl->texRefractData);
            bindTexUnit(gl, 2, m_impl->texShadow);
            bindTexUnit(gl, 3, m_impl->texSimState);
            bindTexUnit(gl, 4, m_impl->texSimLight);
            bindTexUnit(gl, 5, m_impl->texSimKinematics);
            bindTexUnit(gl, 6, m_impl->texMatLut);
            gl.Uniform1i(m_impl->uComposite.sceneTex, 0);
            gl.Uniform1i(m_impl->uComposite.refractTex, 1);
            gl.Uniform1i(m_impl->uComposite.shadowTex, 2);
            gl.Uniform1i(m_impl->uComposite.simStateTex, 3);
            gl.Uniform1i(m_impl->uComposite.simLightTex, 4);
            gl.Uniform1i(m_impl->uComposite.simKinematicsTex, 5);
            gl.Uniform1i(m_impl->uComposite.matLutTex, 6);
            gl.Uniform2f(m_impl->uComposite.simSize, fSimW, fSimH);
            gl.Uniform2f(m_impl->uComposite.invSimSize, invSimW, invSimH);
            gl.Uniform1f(m_impl->uComposite.time, timeSec);
            gl.Uniform1f(m_impl->uComposite.ambientLight, ambient);
            gl.Uniform1f(m_impl->uComposite.sunIntensity, sun);
            gl.Uniform1i(m_impl->uComposite.refractionEnabled, refractOn);
            glDrawArrays(GL_TRIANGLES, 0, 3);

            // --- Pass 4: HDR Bloom Pyramid ---
            if (bloomAmt > 0.001f) {
                // 4a: Prefilter + 13-tap downsample (LitHDR -> BloomHalf)
                gl.BindFramebuffer(GL_FRAMEBUFFER, m_impl->fboBloomHalf);
                glViewport(0, 0, m_impl->halfW, m_impl->halfH);
                gl.UseProgram(m_impl->progBloomPrefilter);
                bindTexUnit(gl, 0, m_impl->texLitHDR);
                gl.Uniform1i(m_impl->uBloomPrefilter.srcTex, 0);
                gl.Uniform2f(m_impl->uBloomPrefilter.texelSize, 1.0f / fCanvasW, 1.0f / fCanvasH);
                glDrawArrays(GL_TRIANGLES, 0, 3);

                // 4b: 13-tap downsample (BloomHalf -> BloomQuarter)
                gl.BindFramebuffer(GL_FRAMEBUFFER, m_impl->fboBloomQuarter);
                glViewport(0, 0, m_impl->quarterW, m_impl->quarterH);
                gl.UseProgram(m_impl->progBloomDown);
                bindTexUnit(gl, 0, m_impl->texBloomHalf);
                gl.Uniform1i(m_impl->uBloomDown.srcTex, 0);
                gl.Uniform2f(m_impl->uBloomDown.texelSize, 1.0f / static_cast<float>(m_impl->halfW),
                             1.0f / static_cast<float>(m_impl->halfH));
                glDrawArrays(GL_TRIANGLES, 0, 3);

                // 4c: 13-tap downsample (BloomQuarter -> BloomEighth)
                gl.BindFramebuffer(GL_FRAMEBUFFER, m_impl->fboBloomEighth);
                glViewport(0, 0, m_impl->eighthW, m_impl->eighthH);
                bindTexUnit(gl, 0, m_impl->texBloomQuarter);
                gl.Uniform2f(m_impl->uBloomDown.texelSize, 1.0f / static_cast<float>(m_impl->quarterW),
                             1.0f / static_cast<float>(m_impl->quarterH));
                glDrawArrays(GL_TRIANGLES, 0, 3);

                // 4d & 4e: 9-tap tent additive upsample (Eighth -> Quarter -> Half)
                glEnable(GL_BLEND);
                gl.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
                gl.BlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
                gl.UseProgram(m_impl->progBloomUp);
                gl.Uniform1i(m_impl->uBloomUp.srcTex, 0);

                gl.BindFramebuffer(GL_FRAMEBUFFER, m_impl->fboBloomQuarter);
                glViewport(0, 0, m_impl->quarterW, m_impl->quarterH);
                bindTexUnit(gl, 0, m_impl->texBloomEighth);
                gl.Uniform2f(m_impl->uBloomUp.texelSize, 1.0f / static_cast<float>(m_impl->eighthW),
                             1.0f / static_cast<float>(m_impl->eighthH));
                glDrawArrays(GL_TRIANGLES, 0, 3);

                gl.BindFramebuffer(GL_FRAMEBUFFER, m_impl->fboBloomHalf);
                glViewport(0, 0, m_impl->halfW, m_impl->halfH);
                bindTexUnit(gl, 0, m_impl->texBloomQuarter);
                gl.Uniform2f(m_impl->uBloomUp.texelSize, 1.0f / static_cast<float>(m_impl->quarterW),
                             1.0f / static_cast<float>(m_impl->quarterH));
                glDrawArrays(GL_TRIANGLES, 0, 3);

                glDisable(GL_BLEND);
            }

            // --- Pass 5: ACES Filmic Tone Mapping + sRGB + 1/255 TPDF Dither ---
            gl.BindFramebuffer(GL_FRAMEBUFFER, m_impl->fboFinal);
            glViewport(0, 0, m_impl->canvasW, m_impl->canvasH);
            gl.UseProgram(m_impl->progToneMap);
            bindTexUnit(gl, 0, m_impl->texLitHDR);
            bindTexUnit(gl, 1, m_impl->texBloomHalf);
            gl.Uniform1i(m_impl->uToneMap.litHDRTex, 0);
            gl.Uniform1i(m_impl->uToneMap.bloomTex, 1);
            gl.Uniform1f(m_impl->uToneMap.bloomIntensity, bloomAmt);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }

    if (fboFailed) {
        m_impl->releaseAllGlResources();
        return false;
    }
    return true;
}

} // namespace sim::app
