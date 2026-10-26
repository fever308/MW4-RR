// Transparency albedo blend (NVIDIA RR guide 3.4.1). One thread per render pixel.
// Coverage is the owned 4x MSAA transmittance T (cleared to 1, multiplied by each
// captured composite's dest-blend factor). Parity mapping matches the glass unpack.
// The CPU mirror is rrblend::Reference in src/rr_transparency_blend_policy.h.
cbuffer Control : register(b0) {
    uint Admitted, Width, Height, Flags; // Flags bit0 blend sky (status 2), bit1 blend roughness,
                                         // bit2 3x3 tent filter, bit3 fade the SSS guide
    float4 Diffuse;                      // rgb = diffuse target
    float4 Params;                       // x specular target, y roughness target, z strength, w floor
    uint4 Reserved;                      // unused
    uint4 Zero;                          // always 0: source for clearing Stats
};
Texture2DMS<float4, 4> Coverage : register(t0);
Texture2D<uint> Status : register(t1);
RWTexture2D<float4> NormalRoughness : register(u0);
RWTexture2D<float4> DiffuseAlbedo : register(u1);
RWTexture2D<float4> SpecularAlbedo : register(u2);
RWByteAddressBuffer Stats : register(u3); // [0] ran [4] eligible [8] covered [12] opacity*64
RWTexture2D<float> SssGuide : register(u4); // a null view unless Flags bit3
// The least magnitude a faded SSS guide keeps (rrblend::SssFloor): the next frame's
// SSS-masked lighting reads a nonzero guide as an SSS pixel.
static const float kSssFloor = 6.103515625e-05;
groupshared uint g_eligible, g_covered, g_opacity;
static const float3 kLuma = float3(0.299, 0.587, 0.114);
// Opacity of render pixel q. Saturated per tap: NaN reads as clear, and a value
// outside [0, 1] cannot leak through the weighted sum. The subtraction is precise
// so that it carries no fast-math (no-NaN) flag into the saturate that maps NaN to 0.
float3 Opacity(int2 q) {
    precise float3 o = 1.0 - Coverage.Load(q >> 1, (q.x & 1) | ((q.y & 1) << 1)).rgb;
    return saturate(o);
}
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    // Submission-time gate. Admitted lives in an upload-heap CBV that the CPU writes, and every
    // wave loads it on its own, so both barriers stay unconditional: a word rewritten while the
    // dispatch is in flight must not leave part of a group waiting at a barrier the rest skipped.
    // Unadmitted, no thread reads or writes a resource (inside is false, Stats stays untouched).
    const bool admitted = Admitted == 1u;
    if (gi == 0) {
        g_eligible = 0;
        g_covered = 0;
        g_opacity = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    const bool inside = admitted && id.x < Width && id.y < Height;
    const uint status = inside ? Status[id.xy] : 0u;
    const bool eligible = status == 1u || status == 3u || (status == 2u && (Flags & 1u));
    float3 c = 0;
    if (eligible) {
        const int2 p = int2(id.xy);
        float3 o;
        if (Flags & 4u) {
            // 1-2-1 x 1-2-1 tent over p + [-1, 1]^2, normalized over the taps inside the extent.
            o = 0;
            float total = 0.0;
            [unroll] for (int dy = -1; dy <= 1; ++dy) [unroll] for (int dx = -1; dx <= 1; ++dx) {
                const int2 q = p + int2(dx, dy);
                if (q.x >= 0 && q.y >= 0 && q.x < int(Width) && q.y < int(Height)) {
                    const float w = (dx != 0 ? 1.0 : 2.0) * (dy != 0 ? 1.0 : 2.0);
                    o += w * Opacity(q);
                    total += w;
                }
            }
            o /= total;
        } else {
            o = Opacity(p);
        }
        c = saturate(saturate((o - Params.w) / (1.0 - Params.w)) * Params.z);
    }
    const bool covered = eligible && max(c.r, max(c.g, c.b)) > 0.0;
    if (covered) {
        float4 d = DiffuseAlbedo[id.xy];
        float4 s = SpecularAlbedo[id.xy];
        d.rgb = lerp(d.rgb, Diffuse.rgb, c);
        s.rgb = lerp(s.rgb, Params.xxx, c);
        if (Flags & 2u) {
            float4 n = NormalRoughness[id.xy];
            n.w = lerp(n.w, Params.y, dot(c, kLuma));
            NormalRoughness[id.xy] = n;
        }
        DiffuseAlbedo[id.xy] = d;
        SpecularAlbedo[id.xy] = s;
        // The colour shows the background's SSS only through the layer's transmittance,
        // about 1 - c: fade the guide by c, weighted as NVIDIA's SSS guide luminance.
        if (Flags & 8u) {
            const float g = SssGuide[id.xy];
            if (g != 0.0) {
                const float w = dot(c, float3(0.25, 0.5, 0.25));
                const float faded = max(abs(g) * (1.0 - w), min(abs(g), kSssFloor));
                SssGuide[id.xy] = g < 0.0 ? -faded : faded;
            }
        }
    }
    if (eligible)
        InterlockedAdd(g_eligible, 1u);
    if (covered) {
        InterlockedAdd(g_covered, 1u);
        // At most 64 per pixel: the uint32 sum is exact up to 2^26 covered pixels (8192x8192),
        // well above 4K (3840x2160).
        InterlockedAdd(g_opacity, uint(round(dot(c, kLuma) * 64.0)));
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) {
        if (admitted && id.x == 0 && id.y == 0)
            Stats.Store(0, 1u);
        if (g_eligible) Stats.InterlockedAdd(4, g_eligible);
        if (g_covered) Stats.InterlockedAdd(8, g_covered);
        if (g_opacity) Stats.InterlockedAdd(12, g_opacity);
    }
}
