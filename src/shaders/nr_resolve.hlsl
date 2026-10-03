// Brings the model's answer back onto the frame.
//
// Everything is compared in the diffuse-white-relative linear space. The model's verdict is taken
// as a luminance ratio against what it was actually shown -- the proxy's own round trip, not the
// original -- so highlights the curve compressed, or that hit its cap, are not mistaken for the
// model darkening them. That ratio is clamped to the guard and applied on the game's hue (at
// g_colour 0) or on the model's (at 1). Blend rather than replace: g_transfer 0 gives back exactly
// the original. The result is re-encoded into the source's encoding.
//
// Debug views write into the frame directly (they pass through the game's post-processing, so they
// are for judging the pass, not for pixel-exact readings): 1 = what the model is shown, 2 = its raw
// answer, 3 = what it changed, amplified twenty times -- flat black means nothing, 4 = the motion
// vectors the model is given.
#include "nr_common.hlsli"

Texture2D<float3> Orig : register(t0);
Texture2D<float3> Model : register(t1);
RWTexture2D<float3> Out : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_width || id.y >= g_height)
        return;

    const float3 src = Orig[id.xy];
    const float3 o = max(SourceToRel(src), 0.0);
    const float3 shown = EncodeProxy(o);
    const float3 answer = Model[id.xy];

    if (g_debug == 1)
    {
        Out[id.xy] = LinearToSource(SrgbToLinear(shown));
        return;
    }
    if (g_debug == 2)
    {
        Out[id.xy] = LinearToSource(SrgbToLinear(clamp(answer, 0.0, 1.0)));
        return;
    }
    if (g_debug == 4)
    {
        // Motion vectors as the model gets them, in pixels per frame: red = horizontal,
        // green = vertical, log scale up to 16 px. Blue is the depth the model gets, as a check
        // that the guides are read at all.
        uint mw, mh;
        Model.GetDimensions(mw, mh);
        const uint2 mid = uint2(id.x * mw / g_width, id.y * mh / g_height);
        const float2 px = Model[mid].xy * float2(g_max_ratio, g_colour);
        uint dw, dh;
        Orig.GetDimensions(dw, dh);
        const float d = Orig[uint2(id.x * dw / g_width, id.y * dh / g_height)].x;
        // Blue: raw depth, dimmed so motion stays readable on top of it.
        // Log scale, as Launchpad's own view: a tenth of a pixel already shows.
        const float2 v = saturate(log(1.0 + abs(px) * 4.0) / log(1.0 + 4.0 * 16.0));
        Out[id.xy] = LinearToSource(SrgbToLinear(float3(v, d * 0.5)));
        return;
    }
    if (g_debug == 3)
    {
        Out[id.xy] = LinearToSource(abs(DecodeProxy(answer) - DecodeProxy(shown)) * 20.0);
        return;
    }

    const float3 p = DecodeProxy(shown);   // the original as the model saw it
    float3 m = DecodeProxy(answer);
    const float lo = Luma(o);
    const float lp = Luma(p);
    const float lm = Luma(m);

    float3 result = o;
    if (lp > 1e-6 && lo > 1e-6 && lm > 1e-6)
    {
        const float ratio = clamp(lm / lp, 1.0 / g_max_ratio, g_max_ratio);
        const float target = lo * ratio;
        const float3 on_game_hue = o * ratio;
        const float3 on_model_hue = m * (target / lm);
        result = lerp(on_game_hue, on_model_hue, g_colour);
    }
    // else: black stays black, and a model answer of zero is not trusted -- the original stands.

    // NaN guard: any non-finite lane falls back to the original.
    result = select(result == result, result, o);

    // Pixels the transfer leaves alone are written back bit-exact.
    if (g_transfer <= 0.0)
    {
        Out[id.xy] = src;
        return;
    }
    Out[id.xy] = RelToSource(lerp(o, result, g_transfer));
}
