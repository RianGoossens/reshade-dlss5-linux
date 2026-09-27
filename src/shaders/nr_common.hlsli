// The color bridge.
//
// The source (the game's DLSS output) is first decoded from its encoding to linear BT.709, then
// normalised so 1.0 is the diffuse white -- mirroring the RenoDX DLSS5 addon's Encoding and Diffuse
// White (nits) controls. The model is then shown a display-referred picture of that: exact below a
// knee, highlights rolled off smoothly above it, sRGB-encoded. SDR content at its own diffuse white
// therefore reaches the model almost untouched.
//
// The legacy "measured" curve is kept as an option: the white point is then the frame's smoothed
// log-average luminance and the curve is Reinhard, as in the original fork.
//
// Encode and resolve run in the same frame with the same constants, so the round trip is lossless
// by construction below the curve's cap.

#define ENC_LINEAR   1  // linear BT.709 (native DLSS SR/AA/RR output)
#define ENC_SRGB     2  // sRGB-encoded BT.709 (SDR UNORM targets)
#define ENC_PQ       3  // BT.2100 PQ (ST 2084, BT.2020 primaries)
#define ENC_SCRGB    4  // linear scRGB (1.0 = 80 nits)
#define ENC_SCRGB_NL 5  // sRGB-curve-encoded scRGB, sign-preserving

#define CURVE_KNEE     0
#define CURVE_REINHARD 1

cbuffer Constants : register(b0)
{
    float g_transfer;     // how far the frame moves toward the model's answer; 0 = bypass
    float g_max_ratio;    // luminance guard: the model may move a pixel at most this far, either way
    float g_colour;       // 0 = keep the game's own hue, only brightness carries the model's verdict
    float g_white_point;  // source-linear value of diffuse white (or the measured white point)
    uint g_debug;         // 0 off, 1 proxy, 2 model answer, 3 difference x20
    uint g_width;
    uint g_height;
    uint g_encoding;      // ENC_*
    uint g_curve;         // CURVE_*
};

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float3 SrgbToLinear(float3 c)
{
    const float3 a = abs(c);
    const float3 l = select(a <= 0.04045, a / 12.92, pow((a + 0.055) / 1.055, 2.4));
    return sign(c) * l;
}

float3 LinearToSrgb(float3 c)
{
    const float3 a = abs(c);
    const float3 e = select(a <= 0.0031308, a * 12.92, 1.055 * pow(a, 1.0 / 2.4) - 0.055);
    return sign(c) * e;
}

static const float PQ_M1 = 0.1593017578125;
static const float PQ_M2 = 78.84375;
static const float PQ_C1 = 0.8359375;
static const float PQ_C2 = 18.8515625;
static const float PQ_C3 = 18.6875;

float3 PqToNits(float3 e)
{
    const float3 p = pow(saturate(e), 1.0 / PQ_M2);
    return 10000.0 * pow(max(p - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * p), 1.0 / PQ_M1);
}

float3 NitsToPq(float3 n)
{
    const float3 y = pow(saturate(n / 10000.0), PQ_M1);
    return pow((PQ_C1 + PQ_C2 * y) / (1.0 + PQ_C3 * y), PQ_M2);
}

float3 Bt2020To709(float3 c)
{
    return float3(dot(c, float3( 1.6605, -0.5876, -0.0728)),
                  dot(c, float3(-0.1246,  1.1329, -0.0083)),
                  dot(c, float3(-0.0182, -0.1006,  1.1187)));
}

float3 Bt709To2020(float3 c)
{
    return float3(dot(c, float3(0.6274, 0.3293, 0.0433)),
                  dot(c, float3(0.0691, 0.9195, 0.0114)),
                  dot(c, float3(0.0164, 0.0880, 0.8956)));
}

// Source encoding -> linear BT.709 in the source's own scale (PQ in nits).
float3 SourceToLinear(float3 v)
{
    switch (g_encoding)
    {
        case ENC_SRGB:     return SrgbToLinear(v);
        case ENC_PQ:       return Bt2020To709(PqToNits(v));
        case ENC_SCRGB_NL: return SrgbToLinear(v);
        default:           return v;  // linear BT.709, scRGB
    }
}

float3 LinearToSource(float3 l)
{
    switch (g_encoding)
    {
        case ENC_SRGB:     return LinearToSrgb(max(l, 0.0));
        case ENC_PQ:       return NitsToPq(max(Bt709To2020(l), 0.0));
        case ENC_SCRGB_NL: return LinearToSrgb(l);
        default:           return l;
    }
}

// Source -> relative linear BT.709, 1.0 = diffuse white.
float3 SourceToRel(float3 v) { return SourceToLinear(v) / max(g_white_point, 1e-4); }
float3 RelToSource(float3 r) { return LinearToSource(r * max(g_white_point, 1e-4)); }

// --- the curve the model sees ---------------------------------------------

static const float KNEE = 0.8;       // below this (relative to diffuse white) the proxy is exact
static const float CURVE_CAP = 0.9995;

float3 KneeForward(float3 x)
{
    const float s = 1.0 - KNEE;
    return select(x <= KNEE, x, KNEE + s * (1.0 - exp(-(x - KNEE) / s)));
}

float3 KneeInverse(float3 y)
{
    const float s = 1.0 - KNEE;
    y = min(y, CURVE_CAP);
    return select(y <= KNEE, y, KNEE - s * log(max(1.0 - (y - KNEE) / s, 1e-6)));
}

// Relative linear -> the display-referred picture the model is shown.
float3 EncodeProxy(float3 rel)
{
    rel = max(rel, 0.0);
    if (g_curve == CURVE_REINHARD)
        return pow(rel / (1.0 + rel), 1.0 / 2.2);
    return LinearToSrgb(KneeForward(rel));
}

// The model's display-referred answer -> relative linear.
float3 DecodeProxy(float3 display_color)
{
    const float3 p = clamp(display_color, 0.0, 1.0);
    if (g_curve == CURVE_REINHARD)
    {
        // 0.999 caps the inverse at ~1000x rather than infinity when the model writes pure white.
        const float3 t = pow(min(p, 0.999), 2.2);
        return t / (1.0 - t);
    }
    return KneeInverse(SrgbToLinear(p));
}
