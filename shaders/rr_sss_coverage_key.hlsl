// Subsurface Scattering coverage experiment (src/rr_sss_coverage.h): this frame's surface
// key, from the prepass PackedSurface right after the prepass, for a later frame's
// reprojection. High 16 bits: 0 for sky or unusable depth; float16 hardware depth for the
// world; bit 15 plus float16 of the weapon camera's own depth on the weapon slice. Low 16
// bits: PackedSurface.y's 8+8 bit octahedral normal. Thread 0 also zeroes this frame's
// lighting counters (u1), which the lighting pass then adds to.
cbuffer Control:register(b0){uint Width,Height;}
Texture2D<uint4> PackedSurface:register(t0);
RWTexture2D<uint> Key:register(u0);
RWByteAddressBuffer Counters:register(u1);
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if(all(id==0)){
  [unroll]for(uint i=0;i<64;i+=16)Counters.Store4(i,uint4(0,0,0,0));
 }
 if(id.x>=Width||id.y>=Height)return;
 uint4 s=PackedSurface.Load(int3(id.xy,0));
 float d=asfloat(s.x);
 uint high=0;
 // Integer exponent test: FXC/DXC may fold isfinite away.
 if((s.x&0x7f800000u)!=0x7f800000u&&d>0&&d<=1){
  if(d>63.0/64.0)high=0x8000u|(f32tof16(min((d-63.0/64.0)*64,1.0))&0x7fffu);
  else high=f32tof16(d)&0x7fffu;
 }
 Key[id.xy]=(high<<16)|(s.y&0xffffu);
}
