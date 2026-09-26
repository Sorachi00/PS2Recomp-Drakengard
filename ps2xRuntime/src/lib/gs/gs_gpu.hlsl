// GS compute pipeline. Compile with IEEE strictness: guest interpolation and
// integer blending must not be replaced with host fixed-function blending.
cbuffer State : register(b0) { uint4 words[48]; }
#define P(i) words[(i)/4][(i)%4]
RWByteAddressBuffer Vram : register(u0);
RWByteAddressBuffer Output : register(u1);
ByteAddressBuffer Upload : register(t0);

uint storageBits(uint p) { return p==20 ? 4 : p==19 ? 8 : (p==2||p==10||p==50||p==58) ? 16 : 32; }
uint bits(uint p) { return (p==1||p==49) ? 24 : p==27 ? 8 : (p==36||p==44) ? 4 : storageBits(p); }
uint2 extent(uint p) { return p==20 ? uint2(128,128) : p==19 ? uint2(128,64) : storageBits(p)==16 ? uint2(64,64) : uint2(64,32); }
uint address(uint p, uint base, uint bw, uint x, uint y)
{
    uint2 e=extent(p); uint page=(y/e.y)*(bw*64/e.x)+x/e.x;
    uint block, column; x%=e.x; y%=e.y;
    if (p==20) { block=BlockTableP4[(y/16)*4+x/32]; column=ColumnTable4[(y%16)*32+x%32]; }
    else if(p==19) { block=BlockTableP8[(y/16)*8+x/16]; column=ColumnTable8[(y%16)*16+x%16]; }
    else if(storageBits(p)==16) {
        uint i=(y/8)*4+x/16;
        block=p==10 ? BlockTableC16S[i] : p==50 ? BlockTableZ16[i] : p==58 ? BlockTableZ16S[i] : BlockTableC16[i];
        column=ColumnTable16[(y%8)*16+x%16];
    } else { uint i=(y/8)*8+x/8; block=(p==48||p==49) ? BlockTableZ32[i] : BlockTableC32[i]; column=ColumnTable32[(y%8)*8+x%8]; }
    return ((base*2048+page*65536+block*2048+column*storageBits(p)) + (p==27||p==36 ? 24 : p==44 ? 28 : 0)) & 0x1ffffff;
}
uint readPixel(uint p,uint base,uint bw,uint x,uint y)
{
    uint a=address(p,base,bw,x,y); uint n=bits(p);
    return (Vram.Load((a/32)*4) >> (a%32)) & (n==32 ? 0xffffffff : (1u<<n)-1);
}
void writePixelRaw(uint p,uint base,uint bw,uint x,uint y,uint value)
{
    uint a=address(p,base,bw,x,y), n=bits(p), at=(a/32)*4;
    if(n==32) { Vram.Store(at,value); return; }
    uint mask=((1u<<n)-1)<<(a%32), old=Vram.Load(at), found;
    [allow_uav_condition] for(;;) {
        Vram.InterlockedCompareExchange(at,old,(old&~mask)|((value<<(a%32))&mask),found);
        if(found==old) break; old=found;
    }
}
uint4 unpack(uint c) { return uint4(c,c>>8,c>>16,c>>24)&255; }
uint pack(uint4 c) { return c.x|(c.y<<8)|(c.z<<16)|(c.w<<24); }
uint decode16(uint c) { return pack(uint4((c&31)<<3,((c>>5)&31)<<3,((c>>10)&31)<<3,((c>>15)&1)<<7)); }
uint encode16(uint c) { uint4 v=unpack(c); return (v.x>>3)|((v.y>>3)<<5)|((v.z>>3)<<10)|((v.w>>7)<<15); }
uint texa(uint p,uint v) {
    uint a=v>>24; bool zero=(v&0xffffff)==0;
    if(p==1) a=P(35)&&zero ? 0 : P(34);
    if(p==2||p==10) a=(a&128) ? P(36) : P(35)&&zero ? 0 : P(34);
    return (v&0xffffff)|(a<<24);
}
int wrapCoord(int c,int size,uint mode,uint lo,uint hi) {
    if(mode==0) return uint(c)&uint(size-1);
    if(mode==1) return clamp(c,0,size-1);
    if(mode==2) return min(max(c,int(lo)),int(hi));
    return (uint(c)&lo)|hi;
}
uint samplePoint(int x,int y) {
    x=wrapCoord(x,P(23),P(37),P(39),P(40)); y=wrapCoord(y,P(24),P(38),P(41),P(42));
    uint p=P(22), v=readPixel(p,P(20),P(21),x,y);
    if(p==0||p==1||p==48||p==49) return texa(p,v);
    if(storageBits(p)==16) return texa(p,decode16(v));
    if(p==19||p==20||p==27||p==36||p==44) {
        bool four=p==20||p==36||p==44, pal16=P(28)==2||P(28)==10;
        uint mask=pal16 ? 511 : 255;
        uint index=four ? v&15 : v;
        if(P(29)==0) {
            uint base=(P(30)&(pal16 ? 31 : 15))<<4;
            index=(index+base)&mask;
            index=(index&~24u)|((index&8)<<1)|((index&16)>>1);
        }
        uint c=readPixel(P(28),P(27),max(1u,P(31)),P(32)+(index&15),P(33)+(index>>4));
        return texa(P(28),pal16 ? decode16(c) : c);
    }
    return 0xffff00ff;
}
float qValue(float q) { return abs(q)>1e-8 ? q : 1; }
// Shader float division may use an approximate reciprocal. Refine it before
// rounding to float so integer texel/colour boundaries match the CPU reference.
float divide(float a,float b) {
    precise double inverse=double(1.0/b);
    inverse=inverse*(2.0-double(b)*inverse);
    return float(double(a)*inverse);
}
uint4 sampleTexture(float s,float t,float q,uint u,uint v) {
    precise float2 uv=(P(1)&16) ? float2(u,v)/16.0 : float2(divide(s,qValue(q)),divide(t,qValue(q)))*float2(P(23),P(24));
    if(!(P(1)&32)) return unpack(samplePoint(int(uv.x),int(uv.y)));
    uv-=.5;
    int2 ij=int2(floor(uv)); precise float2 f=uv-float2(ij);
    uint c00=samplePoint(ij.x,ij.y), c10=f.x==0 ? c00 : samplePoint(ij.x+1,ij.y);
    uint c01=f.y==0 ? c00 : samplePoint(ij.x,ij.y+1), c11=f.x==0 ? c01 : f.y==0 ? c10 : samplePoint(ij.x+1,ij.y+1);
    precise float4 top=float4(unpack(c00))+(float4(unpack(c10))-float4(unpack(c00)))*f.x;
    precise float4 bottom=float4(unpack(c01))+(float4(unpack(c11))-float4(unpack(c01)))*f.x;
    precise float4 color=top+(bottom-top)*f.y;
    uint4 integer=uint4(color); return min(255u,integer+uint4(color-float4(integer)>=.5));
}
uint4 combine(uint4 v,uint4 t) {
    uint3 rgb=t.xyz; uint a=P(25) ? t.w : v.w;
    if(P(26)==0) { rgb=min(255u,(t.xyz*v.xyz)>>7); a=P(25) ? min(255u,(t.w*v.w)>>7) : v.w; }
    if(P(26)==2||P(26)==3) { rgb=min(255u,((t.xyz*v.xyz)>>7)+v.w); if(P(26)==2) a=P(25) ? min(255u,t.w+v.w) : v.w; }
    return uint4(rgb,a);
}
bool alphaPass(uint a) {
    if(!(P(9)&1)) return true; uint mode=(P(9)>>1)&7, ref=(P(9)>>4)&255;
    return mode==1 || (mode==2&&a<ref)||(mode==3&&a<=ref)||(mode==4&&a==ref)||(mode==5&&a>=ref)||(mode==6&&a>ref)||(mode==7&&a!=ref);
}
int3 pickRGB(uint s,int3 src,int3 dst) { return s==0 ? src : s==1 ? dst : int3(0,0,0); }
void shade(int2 xy,uint z,uint4 c,uint fog) {
    if(xy.x<int(P(16))||xy.x>int(P(17))||xy.y<int(P(18))||xy.y>int(P(19))) return;
    if(P(1)&4) c.xyz=((fog*c.xyz)>>8)+(((255-fog)*unpack(P(13)).xyz)>>8);
    bool rgb=true, alpha=true, depth=true;
    if(!alphaPass(c.w)) {
        uint fail=(P(9)>>12)&3;
        if(fail==0) return;
        rgb=fail!=2; alpha=rgb&&!(fail==3&&P(4)==0); depth=fail==2;
    }
    uint raw=readPixel(P(4),P(2),P(3),xy.x,xy.y);
    if(P(9)&16384) { uint bit=P(4)==0 ? 31 : 15; if((P(4)==0||P(4)==2||P(4)==10)&&((raw>>bit)&1)!=((P(9)>>15)&1)) return; }
    uint zm=(P(9)>>17)&3;
    if(zm==0) return;
    if(zm>=2) { uint old=readPixel(P(7),P(6),P(3),xy.x,xy.y); if(zm==2 ? z<old : z<=old) return; }
    if(rgb||alpha) {
        uint dst=storageBits(P(4))==16 ? decode16(raw) : P(4)==1 ? raw|0x80000000 : raw;
        if((P(1)&8)&&!((P(1)&64)&&!(c.w&128))) {
            uint4 d=unpack(dst); uint cs=(P(10)>>4)&3; int factor=cs==0 ? c.w : cs==1 ? d.w : P(11);
            int3 a=pickRGB(P(10)&3,c.xyz,d.xyz), b=pickRGB((P(10)>>2)&3,c.xyz,d.xyz), e=pickRGB((P(10)>>6)&3,c.xyz,d.xyz);
            c.xyz=uint3(clamp(((a-b)*factor>>7)+e,0,255));
        }
        if(alpha&&P(12)&&P(4)!=1) c.w|=128;
        uint value=pack(c); value=(value&~P(5))|(dst&P(5));
        if(!alpha&&P(4)==0) value=(value&0xffffff)|(dst&0xff000000);
        if(storageBits(P(4))==16) value=encode16(value);
        writePixelRaw(P(4),P(2),P(3),xy.x,xy.y,value);
    }
    if(depth&&!P(8)) writePixelRaw(P(7),P(6),P(3),xy.x,xy.y,z);
}
float vf(uint vertex,uint field) { return asfloat(P(64+vertex*16+field)); }
uint vi(uint vertex,uint field) { return P(64+vertex*16+field); }
double vz(uint vertex) { return asdouble(vi(vertex,2),vi(vertex,3)); }
float2 pos(uint v) { return float2(vf(v,0),vf(v,1))-float2(P(14),P(15)); }
void rasterPixel(uint index) {
    int2 xy=int2(P(44)+index%P(46),P(45)+index/P(46));
    uint4 color; uint z,fog;
    if(P(0)==6) {
        int2 offset=int2(P(14),P(15));
        int2 a=int2(vf(0,0),vf(0,1))-offset, b=int2(vf(1,0),vf(1,1))-offset;
        int2 lo=min(a,b), span=max(1,abs(b-a));
        z=uint(vz(1)); color=unpack(vi(1,4)); fog=vi(1,10);
        if(P(1)&2) {
            precise float2 t=float2(divide(float(xy.x-lo.x)+.5,float(span.x)),divide(float(xy.y-lo.y)+.5,float(span.y)));
            precise float2 uv0,uv1;
            if(P(1)&16) { uv0=float2(vi(0,8)>>4,vi(0,9)>>4); uv1=float2(vi(1,8)>>4,vi(1,9)>>4); }
            else { uv0=float2(divide(vf(0,6),qValue(vf(0,5))),divide(vf(0,7),qValue(vf(0,5))))*float2(P(23),P(24)); uv1=float2(divide(vf(1,6),qValue(vf(1,5))),divide(vf(1,7),qValue(vf(1,5))))*float2(P(23),P(24)); }
            precise float2 uv=uv0+(uv1-uv0)*t;
            uint2 fixedUv=uint2(clamp(int2(uv*16+.5),0,65535));
            color=combine(color,sampleTexture(divide(uv.x,P(23)),divide(uv.y,P(24)),1,fixedUv.x,fixedUv.y));
        }
    } else if(P(0)==0) { z=uint(vz(0)); color=unpack(vi(0,4)); fog=vi(0,10); }
    else {
        precise float2 a=pos(0), b=pos(1), c=pos(2), p=float2(xy)+.5;
        precise float denominator=(b.y-c.y)*(a.x-c.x)+(c.x-b.x)*(a.y-c.y);
        if(abs(denominator)<.001) return;
        float winding=denominator<0 ? -1 : 1; precise float inv=divide(1,abs(denominator));
        precise float w0=(((b.y-c.y)*(p.x-c.x)+(c.x-b.x)*(p.y-c.y))*winding)*inv;
        precise float w1=(((c.y-a.y)*(p.x-c.x)+(a.x-c.x)*(p.y-c.y))*winding)*inv;
        precise float w2=1-w0-w1;
        if(w0<-.0001||w1<-.0001||w2<-.0001) return;
        z=uint(vz(0)*double(w0)+vz(1)*double(w1)+vz(2)*double(w2)+.5);
        precise float4 interpolated=float4(unpack(vi(0,4)))*w0+float4(unpack(vi(1,4)))*w1+float4(unpack(vi(2,4)))*w2;
        color=(P(1)&1) ? uint4(clamp(int4(interpolated),0,255)) : unpack(vi(2,4));
        fog=uint(clamp(int(vi(0,10)*w0+vi(1,10)*w1+vi(2,10)*w2),0,255));
        if(P(1)&2) {
            precise float s=vf(0,6)*w0+vf(1,6)*w1+vf(2,6)*w2;
            precise float t=vf(0,7)*w0+vf(1,7)*w1+vf(2,7)*w2;
            precise float q=vf(0,5)*w0+vf(1,5)*w1+vf(2,5)*w2;
            uint u=uint(vi(0,8)*w0+vi(1,8)*w1+vi(2,8)*w2)&65535;
            uint v=uint(vi(0,9)*w0+vi(1,9)*w1+vi(2,9)*w2)&65535;
            color=combine(color,sampleTexture(s,t,q,u,v));
        }
    }
    shade(xy,z,color,fog);
}
void rasterLine() {
    int2 offset=int2(P(14),P(15));
    int2 a=int2(vf(0,0),vf(0,1))-offset, b=int2(vf(1,0),vf(1,1))-offset;
    int2 d=int2(abs(b.x-a.x),-abs(b.y-a.y));
    int2 step=int2(a.x<b.x ? 1:-1,a.y<b.y ? 1:-1); int err=d.x+d.y; uint i=0;
    float total=max(1,max(abs(b.x-a.x),abs(b.y-a.y)));
    [loop] for(;;) {
        precise float t=divide(float(i),total);
        precise float4 c=float4(unpack(vi(0,4)))+(float4(unpack(vi(1,4)))-float4(unpack(vi(0,4))))*t;
        uint4 color=(P(1)&1) ? uint4(clamp(int4(c),0,255)) : unpack(vi(1,4));
        uint z=uint(vz(0)+(vz(1)-vz(0))*double(t));
        uint fog=uint(clamp(int(float(vi(0,10))+(float(vi(1,10))-float(vi(0,10)))*t),0,255));
        shade(a,z,color,fog); if(all(a==b)) break;
        int e2=2*err; if(e2>=d.y){err+=d.y;a.x+=step.x;} if(e2<=d.x){err+=d.x;a.y+=step.y;} ++i;
    }
}
[numthreads(64,1,1)] void Raster(uint3 tid:SV_DispatchThreadID) {
    if(P(0)==1||P(0)==2) { if(tid.x==0) rasterLine(); return; }
    uint start=tid.x,count=1,pageX=0,pageY=0,pageWidth=1;
    if(P(48)==2) {
        if(tid.x>=P(49)*P(50))return;
        uint x0=(P(44)/64+tid.x%P(49))*64,y0=(P(45)/32+tid.x/P(49))*32;
        // Colour and Z alias only inside this GS page. Preserve the original
        // pixel order there while unrelated pages execute concurrently.
        pageX=max(x0,P(44));pageY=max(y0,P(45));
        pageWidth=min(x0+64,P(44)+P(46))-pageX;
        count=pageWidth*(min(y0+32,P(45)+P(47))-pageY);
    }
    else if(P(48)) { if(tid.x!=0)return;start=P(49);count=P(50); }
    else if(tid.x>=P(46)*P(47)) return;
    [loop] for(uint i=0;i<count;++i) {
        uint index=P(48)==2 ? (pageY+i/pageWidth-P(45))*P(46)+pageX+i%pageWidth-P(44) : start+i;
        rasterPixel(index);
    }
}
// Transfer fields: op=0 upload,1 local copy,2 packed readback,3 single read,
// 4 single write,5 clear. Packed upload/readback conversion stays on GPU.
void transferPixel(uint i) {
    uint op=P(0), width=P(1), x=i%width,y=i/width;
    if(op==1) { if(P(14)&2) x=width-x-1; if(P(14)&1) y=P(2)-y-1; }
    uint v=0;
    if(op==0) {
        uint bit=(i-P(19))*bits(P(8)); uint off=(bit/32)*4, shift=bit%32;
        v=Upload.Load(off)>>shift; if(shift+bits(P(8))>32) v|=Upload.Load(off+4)<<(32-shift);
    } else if(op==4||op==5) v=P(16);
    else v=readPixel(P(5),P(3),P(4),P(6)+x,P(7)+y);
    if(op==2) {
        uint n=bits(P(5)), bit=i*n, off=(bit/32)*4, shift=bit%32, dummy;
        v&=n==32 ? 0xffffffff : (1u<<n)-1;
        Output.InterlockedOr(off,v<<shift,dummy);
        if(shift+n>32) Output.InterlockedOr(off+4,v>>(32-shift),dummy);
    } else if(op==3) Output.Store(0,v);
    else {
        if(op==5) { uint old=readPixel(P(8),P(9),P(10),P(11)+x,P(12)+y); v=(v&~P(17))|(old&P(17)); }
        writePixelRaw(P(8),P(9),P(10),P(11)+x,P(12)+y,v);
    }
}
[numthreads(64,1,1)] void Transfer(uint3 tid:SV_DispatchThreadID) {
    if(P(18)) { if(tid.x==0) { [loop] for(uint i=P(15);i<P(15)+P(13);++i) transferPixel(i); } }
    else if(tid.x<P(13)) transferPixel(P(15)+tid.x);
}
// Each presentation source is converted directly from GPU VRAM. Non-black
// counts implement the existing source-selection behaviour without CPU pixels.
uint displayColor(uint base,uint bw,uint p,uint x,uint y) {
    uint c=readPixel(p,base,bw,x,y);
    if(p==1) return c;
    if(p==2||p==10) {
        uint4 v=unpack(decode16(c)); v.xyz|=v.xyz>>5; return pack(v);
    }
    return c;
}
[numthreads(8,8,1)] void Convert(uint3 tid:SV_DispatchThreadID) {
    uint x=tid.x,y=tid.y;if(x>=P(3)||y>=P(4)) return;
    uint v=displayColor(P(0),P(1),P(2),x+P(5),y+P(6));
    Output.Store(64+(P(7)*640*512+y*640+x)*4,v);
    // Only presence matters. Avoid contending on one global counter for every
    // visible pixel (hundreds of thousands of serialized atomics per frame).
    if((v&0xffffff)&&Output.Load(P(7)*4)==0) { uint dummy; Output.InterlockedOr(P(7)*4,1,dummy); }
}
[numthreads(8,8,1)] void Compose(uint3 tid:SV_DispatchThreadID) {
    uint x=tid.x,y=tid.y; if(x>=P(0)||y>=P(1)) return;
    if(P(2)&1) y=(P(2)&2) ? y/2 : min(P(1)-1,(y&~1u)+P(3));
    uint4 c=unpack(P(12));
    if(!P(4)) c=unpack(Output.Load(64+(P(5)*640*512+y*640+x)*4));
    else {
        if(!(P(11)&128)&&x<P(8)&&y<P(9)) c=unpack(Output.Load(64+(P(6)*640*512+y*640+x)*4));
        if(x<P(7)&&y<P(10)) {
            uint4 s=unpack(Output.Load(64+(P(5)*640*512+y*640+x)*4));
            int factor=(P(11)&32) ? (P(11)>>8)&255 : min(255u,s.w*2);
            c.xyz=uint3(clamp(int3(c.xyz)+(int3(s.xyz)-int3(c.xyz))*factor/255,0,255));
        }
    }
    c.w=255; Output.Store(64+(4*640*512+tid.y*640+x)*4,pack(c));
}
