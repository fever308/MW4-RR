// Weapon depth (src/rr_weapon_depth.h): RR's depth as linear view depth, read from
// the depth subrect at Offset. World depth is Scale / (depth + DepthOffset), what the
// projection RR is given implies; the weapon slice (depth > 63/64, the game's own weapon
// camera) keeps its depth order in front of the near plane instead of a flat card.
// RR reads the copy instead; the game's depth is never written.
cbuffer Control:register(b0){uint Width,Height,OffsetX,OffsetY;float Scale,DepthOffset,Sky,Knee;}
Texture2D<float> Depth:register(t0);
RWTexture2D<float> Linear:register(u0);
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=Width||id.y>=Height)return;
 float depth=Depth.Load(int3(id.xy+uint2(OffsetX,OffsetY),0)),z;
 // NR test: hardware reverse-Z form. World depth is copied unchanged; the weapon slice gets
 // the reverse-Z depth of the linear weapon depth, (1 + offset)(1 + t / Knee) - offset >= 1.
 if(depth>63.0/64.0)z=(1+DepthOffset)*(1+min((depth-63.0/64.0)*64,1)/Knee)-DepthOffset;
 else z=depth;
 Linear[id.xy]=z;
}
