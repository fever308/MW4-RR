// Owned MRT sample parity, not a conventional averaged MSAA resolve.
cbuffer Control:register(b0){uint Width,Height,OutputW,OutputH;uint Serial,ResetHistory,Accepted,SssAccepted;uint Replacement,ColorX,ColorY,Flags;}
Texture2DMS<float,4> Producer:register(t0);
RWTexture2D<float> Guide:register(u0);
RWByteAddressBuffer Stats:register(u1);
RWByteAddressBuffer History:register(u2);
// RR_SSS_COVERAGE_OUT (src/rr_sss_coverage.h): the producer ran with the marker stencil
// initializer, which writes 65504 (rrssscov::Marker) where the Subsurface Scattering draw's
// stencil test rejects. Those samples give the guide 0, exactly as the baseline
// initializer's 0 does, and record "not drawn" (0) in u3; samples the draw wrote record 1;
// untouched or unusable samples record 2 (unknown). The guide and stats are otherwise the
// baseline's.
#ifndef RR_SSS_COVERAGE_OUT
#define RR_SSS_COVERAGE_OUT 0
#endif
#if RR_SSS_COVERAGE_OUT
RWTexture2D<uint> Coverage:register(u3);
#endif
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=Width||id.y>=Height)return;
 float value=0;
#if RR_SSS_COVERAGE_OUT
 uint drawn=2;
#endif
 // The source access must remain dominated by this branch in compiled DXIL.
 [branch]if(SssAccepted!=0){
  value=Producer.Load(int2(id.xy>>1),(id.x&1)+2*(id.y&1));
#if RR_SSS_COVERAGE_OUT
  if(value==65504.0f){value=0;drawn=0;}
  else
#endif
  if(!isfinite(value)||value==-65504.0f||abs(value)>=65504.0f){
   value=0;uint old;
   // Tolerant (Flags bit0): this pixel reads as no SSS and is counted. The
   // gate rejects the frame only when such samples are widespread.
   if((Flags&1)!=0)Stats.InterlockedAdd(12,1,old);
   else{Stats.InterlockedOr(0,1,old);History.InterlockedOr(0,1,old);}
  }
#if RR_SSS_COVERAGE_OUT
  else drawn=1;
#endif
 }
 Guide[id.xy]=value;
#if RR_SSS_COVERAGE_OUT
 Coverage[id.xy]=drawn;
#endif
}
