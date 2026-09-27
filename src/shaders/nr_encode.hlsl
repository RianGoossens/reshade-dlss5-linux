// Builds the proxy the model is shown: the source decoded to diffuse-white-relative linear, then
// the display-referred curve from nr_common.
#include "nr_common.hlsli"

Texture2D<float3> Src : register(t0);
Texture2D<float3> Unused : register(t1);
RWTexture2D<float3> Dst : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_width || id.y >= g_height)
        return;
    Dst[id.xy] = EncodeProxy(SourceToRel(Src[id.xy]));
}
