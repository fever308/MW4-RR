#include "../src/rr_resolution_limits.h"
// Same native root, heaps and tables. Only proven b1/b3 GPUVAs are temporarily
// changed by the caller and restored after all strips; unknown b2 is untouched.
cbuffer RateFirst:register(b1){uint4 RateHeader;uint4 RateWords0[4095];}
cbuffer RateSecond:register(b3){uint4 RateWords1[4096];}
#ifdef RR_PACKED_FLOAT
Texture2D<float4> nativeLighting:register(t0);
Texture2D<float> nativeWeights:register(t1);
RWTexture2D<float4> color:register(u0);
RWTexture2D<float> weights:register(u1);
#else
Texture2D<uint4> nativeLighting:register(t0);
Texture2D<uint> nativeWeights:register(t1);
RWTexture2D<uint4> color:register(u0);
RWTexture2D<uint> weights:register(u1);
#endif

uint PixelCode(uint2 p){
 uint byteOffset=16+(p.y*RateHeader.x+p.x)*2;
 uint word;
 if(byteOffset<65536){uint i=(byteOffset-16)/4;word=RateWords0[i/4][i%4];}
 else{uint i=(byteOffset-65536)/4;word=RateWords1[i/4][i%4];}
 return (word>>((byteOffset&2)*8))&65535;
}
// RR_NATIVE_LOBE is a lobe mask (1 = first, 2 = second, 3 = both) taken from
// native temporal output (t0, the native spatial input) instead of raw
// current-frame lighting. The game's SSS blur spreads sparse, boosted raw
// diffuse samples into blotches. Its weight bits come along too, matching the
// temporal-only passthrough comparison.
// RR_NATIVE_FILL keeps a lobe's fresh sample, without the inverse-probability
// boost, wherever a ray landed this frame; only unsampled pixels take native.
#ifndef RR_NATIVE_LOBE
#define RR_NATIVE_LOBE 0
#endif
#ifndef RR_NATIVE_FILL
#define RR_NATIVE_FILL 0
#endif
// RR_SSS_MASK applies that selection only where the previous frame's SSS guide
// (t2) is nonzero; every other pixel keeps raw boosted lighting. The game's
// guide is exactly 0 off SSS pixels. Only the private root binds t2.
#ifndef RR_SSS_MASK
#define RR_SSS_MASK 0
#endif
#if RR_SSS_MASK
Texture2D<float> sssGuide:register(t2);
#endif
// RR_SSS_COVERAGE (src/rr_sss_coverage.h; needs RR_SSS_MASK): the "SSS pixel" decision also
// comes from an earlier frame's Subsurface Scattering pass coverage (t10), reprojected with
// this pixel's depth and both frames' cameras (b4) and used only where the surface stored
// for that frame (t9) matches this one (t8). The RR guide is not read for it. CoverageFlags
// bit 0 applies it; otherwise the baseline guide test above is applied unchanged and the
// coverage decision is only counted (u2).
#ifndef RR_SSS_COVERAGE
#define RR_SSS_COVERAGE 0
#endif
#if RR_SSS_COVERAGE&&!RR_SSS_MASK
#error RR_SSS_COVERAGE refines the SSS-masked selection and needs RR_SSS_MASK
#endif
#if RR_SSS_COVERAGE
#include "rr_guide_math.hlsli"
cbuffer CoverageControl:register(b4){
 uint CoverageWidth,CoverageHeight,CoverageFlags,CoverageAge;
 float CoverageAX,CoverageBX,CoverageAY,CoverageBY;
 float CurrentScale,CurrentOffset,PreviousScale,PreviousOffset;
 float4 Reproject0,Reproject1,Reproject2,Reproject3; // current view position -> previous clip
 float DepthTolerance,NormalCos,CoveragePad0,CoveragePad1;
}
Texture2D<uint> currentKey:register(t8);
Texture2D<uint> previousKey:register(t9);
Texture2D<uint> previousCoverage:register(t10);
RWByteAddressBuffer coverageCounters:register(u2);
// rrssscov::Pixel
static const uint PixelCovered=0,PixelNotCovered=1,PixelSky=2,PixelOffScreen=3,
 PixelDisoccluded=4,PixelNormal=5,PixelUnknown=6;
struct CoverageSurface{bool present,weapon;float depth;float3 normal;};
CoverageSurface DecodeKey(uint key){
 CoverageSurface s;uint high=key>>16;
 s.present=(high&0x7fffu)!=0;s.weapon=(high&0x8000u)!=0;
 s.depth=f16tof32(high&0x7fffu);s.normal=RRDecodeSurfaceNormal(key&0xffffu);
 return s;
}
// rrssscov::Decide, same operations in the same order.
uint CoverageDecide(uint2 p){
 CoverageSurface s=DecodeKey(currentKey.Load(int3(p,0)));
 if(!s.present)return PixelSky;
 float3 q;
 if(s.weapon)q=float3(float2(p)+0.5,s.depth);
 else{
  float z=CurrentScale/(s.depth+CurrentOffset);
  float nx=(p.x+0.5)/float(CoverageWidth)*2-1,ny=1-(p.y+0.5)/float(CoverageHeight)*2;
  float3 v=float3(z*(nx*CoverageAX+CoverageBX),z*(ny*CoverageAY+CoverageBY),z);
  float4 c=v.x*Reproject0+v.y*Reproject1+v.z*Reproject2+Reproject3;
  if(!(c.w>0))return PixelOffScreen;
  q=float3((c.x/c.w*0.5+0.5)*CoverageWidth,(0.5-c.y/c.w*0.5)*CoverageHeight,c.w);
 }
 if(!(q.x>=0&&q.y>=0&&q.x<float(CoverageWidth)&&q.y<float(CoverageHeight)))return PixelOffScreen;
 int2 b=int2(floor(q.xy-0.5));
 float lo=1e38,hi=0,best=-2;bool matched=false;
 [unroll]for(int dy=0;dy<2;++dy)[unroll]for(int dx=0;dx<2;++dx){
  int2 t=b+int2(dx,dy);
  if(any(t<0)||t.x>=int(CoverageWidth)||t.y>=int(CoverageHeight))continue;
  CoverageSurface u=DecodeKey(previousKey.Load(int3(t,0)));
  if(!u.present||u.weapon!=s.weapon)continue;
  float d=u.weapon?u.depth:PreviousScale/(u.depth+PreviousOffset);
  lo=min(lo,d);hi=max(hi,d);best=max(best,dot(s.normal,u.normal));matched=true;
 }
 if(!matched||!(q.z>=lo*(1-DepthTolerance)&&q.z<=hi*(1+DepthTolerance)))return PixelDisoccluded;
 if(best<NormalCos)return PixelNormal;
 uint v=previousCoverage.Load(int3(uint2(q.xy),0));
 return v==1?PixelCovered:v==0?PixelNotCovered:PixelUnknown;
}
void CoverageCount(uint slot,bool value){
 uint n=WaveActiveCountBits(value);
 if(WaveIsFirstLane()&&n!=0){uint old;coverageCounters.InterlockedAdd(slot*4,n,old);}
}
// Whether the SSS-masked source takes native lobe 2 here (rrssscov::Native), and the counts
// of rrssscov::Counter.
bool CoverageSelect(uint2 p,bool baseline){
 uint d=CoverageDecide(p);
 bool coverage=d==PixelCovered;
 bool native=(CoverageFlags&1)!=0?coverage:baseline;
 CoverageCount(0,true);CoverageCount(1,baseline);CoverageCount(2,coverage);
 CoverageCount(3,baseline!=coverage);CoverageCount(4,native);CoverageCount(5,d==PixelSky);
 CoverageCount(6,d==PixelOffScreen);CoverageCount(7,d==PixelDisoccluded);
 CoverageCount(8,d==PixelNormal);CoverageCount(9,d==PixelUnknown);
 CoverageCount(10,(currentKey.Load(int3(p,0))&0x80000000u)!=0);
 return native;
}
#endif
float4 UnpackLobe(uint2 packed){
 return float4(f16tof32(packed.x&65535),f16tof32(packed.x>>16),f16tof32(packed.y&65535),f16tof32(packed.y>>16));
}
uint2 ScaleLobe(uint2 packed,bool sampled,uint inverse,out bool valid){
 float4 v=UnpackLobe(packed);
 valid=sampled&&inverse!=0&&all(v>=0)&&all(v<=65504);
 if(!valid)return 0;
 float f=float(inverse);v=min(v*float4(f,f,f,f*f),65504);
 uint4 h=f32tof16(v);return uint2(h.x|(h.y<<16),h.z|(h.w<<16));
}
uint2 SelectLobe(uint2 raw,bool sampled,uint2 native,out bool fresh){
#if RR_NATIVE_FILL
 float4 v=float4(f16tof32(raw.x&65535),f16tof32(raw.x>>16),f16tof32(raw.y&65535),f16tof32(raw.y>>16));
 fresh=sampled&&all(v>=0)&&all(v<=65504);
#else
 fresh=false;
#endif
 return fresh?raw:native;
}
// RR_FILL_SPATIAL (SSS-masked lobe 2 fill gaps only): an unsampled pixel takes native
// temporal only where its native lobe-2 field ((nw>>8)&4095) is at least RR_FILL_HISTORY
// (4096: never). Otherwise it takes the mean of this frame's fresh, unboosted, finite
// lobe-2 samples within 2 px, inside this 16x16 group (0 if there are none). Neighbours
// come from groupshared copies taken before any thread writes, so there is no race with
// the in-place output or with other groups and strips. Fresh pixels stay unboosted.
#ifndef RR_FILL_SPATIAL
#define RR_FILL_SPATIAL 0
#endif
#ifndef RR_FILL_HISTORY
#define RR_FILL_HISTORY 0
#endif
#if RR_FILL_SPATIAL&&!(RR_SSS_MASK&&RR_NATIVE_FILL&&RR_NATIVE_LOBE==2&&RR_FILL_HISTORY>=1)
#error RR_FILL_SPATIAL refines SSS-masked lobe 2 fill gaps and needs RR_FILL_HISTORY >= 1
#endif
#if !RR_FILL_SPATIAL&&RR_FILL_HISTORY
#error RR_FILL_HISTORY needs RR_FILL_SPATIAL
#endif
// RR_FILL_PREFER_NATIVE (as RR_FILL_SPATIAL): a pixel whose native lobe-2 field is at least
// RR_FILL_HISTORY takes native temporal even where a fresh ray landed, so a settled pixel never
// shows a single noisy sample; fresh unboosted samples are used only where there is no history.
#ifndef RR_FILL_PREFER_NATIVE
#define RR_FILL_PREFER_NATIVE 0
#endif
#if RR_FILL_PREFER_NATIVE&&!RR_FILL_SPATIAL
#error RR_FILL_PREFER_NATIVE needs RR_FILL_SPATIAL
#endif
// RR_FILL_EDGE (as RR_FILL_SPATIAL): the no-history fill weights samples by surface
// (src/rr_edge_fill.h) instead of the group mean. Neighbours come from the immutable raw
// snapshot the output was copied from (t3/t4), so support crosses group and strip
// boundaries without racing the in-place output. Surface inputs are this frame's: the
// prepass PackedSurface the native spatial denoiser binds (t5) and the guide conversion's
// normals (t6) and Status (t7), all at the lighting extent. Only the private root binds them.
#ifndef RR_FILL_EDGE
#define RR_FILL_EDGE 0
#endif
#if RR_FILL_EDGE&&!RR_FILL_SPATIAL
#error RR_FILL_EDGE needs RR_FILL_SPATIAL
#endif
#if RR_FILL_EDGE
#include "../src/rr_edge_fill.h"
#ifdef RR_PACKED_FLOAT
Texture2D<float4> rawLighting:register(t3);
Texture2D<float> rawWeights:register(t4);
#else
Texture2D<uint4> rawLighting:register(t3);
Texture2D<uint> rawWeights:register(t4);
#endif
Texture2D<uint4> packedSurface:register(t5);
Texture2D<float4> surfaceNormal:register(t6);
Texture2D<uint> surfaceStatus:register(t7);
EdgeSurface LoadSurface(uint2 q){
 const uint4 s=packedSurface.Load(int3(q,0));const float3 n=surfaceNormal.Load(int3(q,0)).xyz;
 EdgeSurface r;r.depth=asfloat(s.x);r.word=s.w;r.nx=n.x;r.ny=n.y;r.nz=n.z;
 r.status=surfaceStatus.Load(int3(q,0));return r;
}
uint2 EdgeFill(uint2 p,uint w,uint h,out uint outcome){
 const EdgeSurface c=LoadSurface(p);
 outcome=RR_EDGE_OUTCOME_UNCLASSIFIED;
 if(!EdgeClassified(c))return 0;
 float4 sum=0;float total=0;
 [unroll]for(int dy=-RR_EDGE_RADIUS;dy<=RR_EDGE_RADIUS;++dy)
 [unroll]for(int dx=-RR_EDGE_RADIUS;dx<=RR_EDGE_RADIUS;++dx){
  const int2 q=int2(p)+int2(dx,dy);
  if((dx==0&&dy==0)||!EdgeInside(q.x,q.y,w,h))continue;
#ifdef RR_PACKED_FLOAT
  const uint2 raw=asuint(rawLighting.Load(int3(q,0))).zw;const uint rw=asuint(rawWeights.Load(int3(q,0)));
#else
  const uint2 raw=rawLighting.Load(int3(q,0)).zw;const uint rw=rawWeights.Load(int3(q,0));
#endif
  const float4 v=UnpackLobe(raw);
  if(!EdgeFresh(rw,v))continue;
  EdgeAccumulate(sum,total,v,EdgeWeight(c,LoadSurface(uint2(q)),dx,dy));
 }
 float4 mean=0;
 if(!EdgeResolve(sum,total,mean)){outcome=RR_EDGE_OUTCOME_NO_NEIGHBOUR;return 0;}
 outcome=RR_EDGE_OUTCOME_EDGE;
 const uint4 m=f32tof16(mean);return uint2(m.x|(m.y<<16),m.z|(m.w<<16));
}
#endif
#if RR_FILL_SPATIAL
groupshared uint3 fillTile[256]; // packed lobe 2, fresh flag
uint2 GroupMean(uint2 gt,out uint n){
 float4 sum=0;n=0;
 [unroll]for(int dy=-2;dy<=2;++dy)[unroll]for(int dx=-2;dx<=2;++dx){
  int2 q=int2(gt)+int2(dx,dy);
  if(any(q<0)||any(q>15))continue;
  uint3 s=fillTile[q.y*16+q.x];
  if(s.z!=0){sum+=UnpackLobe(s.xy);++n;}
 }
 if(n==0)return 0;
 uint4 m=f32tof16(min(sum/float(n),65504));return uint2(m.x|(m.y<<16),m.z|(m.w<<16));
}
#endif
[numthreads(16,16,1)]
#if RR_FILL_SPATIAL
void main(uint3 id:SV_DispatchThreadID,uint3 gt:SV_GroupThreadID,uint gi:SV_GroupIndex){
 uint w,h;color.GetDimensions(w,h);uint2 p=uint2(id.x,id.y+RateHeader.z);
 // Every thread reaches the barrier; out-of-range threads read nothing (no rate-table or
 // texture access) and return after it. The header test below is uniform per dispatch.
 const bool inside=id.x<w&&id.y<RateHeader.w&&p.y<h;
#else
void main(uint3 id:SV_DispatchThreadID){
 uint w,h;color.GetDimensions(w,h);uint2 p=uint2(id.x,id.y+RateHeader.z);
 if(id.x>=w||id.y>=RateHeader.w||p.y>=h)return;
#endif
 // A malformed header cannot be interpreted as a native weighting profile.
 uint rows=w>0?min(32u,(131072u-16u)/(w*2u)):0;
 if(w==0||w>RR_TEXTURE2D_LIMIT||(w&1)!=0||RateHeader.x!=w||RateHeader.y!=h||RateHeader.w==0||RateHeader.w>rows)return;
 #if RR_FILL_SPATIAL
 uint4 c=0;uint wt=0;
 if(inside){
#ifdef RR_PACKED_FLOAT
  c=asuint(color[p]);wt=asuint(weights[p]);
#else
  c=color[p];wt=weights[p];
#endif
 }
 {const float4 v=UnpackLobe(c.zw);
  fillTile[gi]=uint3(c.zw,inside&&((wt>>8)&4095)!=0&&all(v>=0)&&all(v<=65504)?1u:0u);}
 GroupMemoryBarrierWithGroupSync();
 if(!inside)return;
 uint code=PixelCode(id.xy);
#else
 uint code=PixelCode(id.xy);
#ifdef RR_PACKED_FLOAT
 uint4 c=asuint(color[p]);uint wt=asuint(weights[p]);
#else
 uint4 c=color[p];uint wt=weights[p];
#endif
#endif
#ifdef RR_PACKED_FLOAT
 uint4 nc=asuint(nativeLighting.Load(int3(p,0)));uint nw=asuint(nativeWeights.Load(int3(p,0)));
#else
 uint4 nc=nativeLighting.Load(int3(p,0));uint nw=nativeWeights.Load(int3(p,0));
#endif
 bool a,b;
 uint2 ca=ScaleLobe(c.xy,(wt>>20)!=0,code&255,a);
 uint2 cb=ScaleLobe(c.zw,((wt>>8)&4095)!=0,code>>8,b);
 uint outWeights=(wt&((a?0xfff00000:0)|(b?0x000fff00:0)))|(nw&255);
#if RR_SSS_MASK
 uint2 rawA=ca,rawB=cb;uint rawWeights=outWeights;
#endif
#if RR_NATIVE_LOBE&1
 bool fa;ca=SelectLobe(c.xy,(wt>>20)!=0,nc.xy,fa);
 outWeights=(outWeights&~0xfff00000u)|((fa?wt:nw)&0xfff00000u);
#endif
#if RR_NATIVE_LOBE&2
 bool fb;cb=SelectLobe(c.zw,((wt>>8)&4095)!=0,nc.zw,fb);
 outWeights=(outWeights&~0x000fff00u)|((fb?wt:nw)&0x000fff00u);
#if RR_FILL_SPATIAL
 uint found=1;
 if(!fb&&((nw>>8)&4095)<RR_FILL_HISTORY){
#if RR_FILL_EDGE
  uint outcome;const uint2 e=EdgeFill(p,w,h,outcome);
  cb=outcome==RR_EDGE_OUTCOME_EDGE?e:outcome==RR_EDGE_OUTCOME_NO_NEIGHBOUR?uint2(0,0):GroupMean(gt.xy,found);
#else
  cb=GroupMean(gt.xy,found);
#endif
 }
#if RR_FILL_PREFER_NATIVE
 if(fb&&((nw>>8)&4095)>=RR_FILL_HISTORY){
  cb=nc.zw;outWeights=(outWeights&~0x000fff00u)|(nw&0x000fff00u);}
#endif
#endif
#endif
#if RR_SSS_MASK
 // An out-of-range load returns 0, so a mismatched guide leaves raw lighting.
 bool sssNative=!(sssGuide.Load(int3(p,0))==0);
#if RR_SSS_COVERAGE
 sssNative=CoverageSelect(p,sssNative);
#endif
 if(!sssNative){ca=rawA;cb=rawB;outWeights=rawWeights;}
#endif
#ifdef RR_PACKED_FLOAT
 color[p]=asfloat(uint4(ca,cb));
 weights[p]=asfloat(outWeights);
#else
 color[p]=uint4(ca,cb);
 weights[p]=outWeights;
#endif
}
