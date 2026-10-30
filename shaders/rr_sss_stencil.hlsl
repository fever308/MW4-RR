// No game descriptor/root argument access. Zero only stencil-rejected samples.
float4 vs(uint id:SV_VertexID):SV_Position {
    float2 p=id==0?float2(-1,-1):id==1?float2(-1,3):float2(3,-1);
    return float4(p,0,1);
}
[earlydepthstencil] float ps():SV_Target0 { return 0; }
// Subsurface Scattering coverage experiment (src/rr_sss_coverage.h, rrssscov::Marker): the
// same initializer marking stencil-rejected samples as "not drawn" for the resolve, which
// still gives the RR guide 0 there.
[earlydepthstencil] float ps_marker():SV_Target0 { return 65504.0; }
