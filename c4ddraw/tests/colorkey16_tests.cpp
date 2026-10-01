#include <windows.h>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include "scalar-extracted.h"
#include "../features/colorkey16.h"
static BOOL candidate_has_sse2 = IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE);
static unsigned long candidate_fast_calls, candidate_fallback_calls;
__declspec(noinline) void blt_colorkey_candidate(
    unsigned char* dst,int dx,int dy,int w,int h,int dp,
    unsigned char* src,int sx,int sy,int sp,unsigned int low,unsigned int high,int bpp)
{
    if(c4_blt_colorkey16(dst,dx,dy,w,h,dp,src,sx,sy,sp,low,high,bpp,candidate_has_sse2)) {
        ++candidate_fast_calls;
        return;
    }
    ++candidate_fallback_calls;
    blt_colorkey_scalar(dst,dx,dy,w,h,dp,src,sx,sy,sp,low,high,bpp);
}

static unsigned checks=0, failures=0;
#define CHECK(c) do{ ++checks; if(!(c)){ ++failures; printf("FAIL line %d: %s\n",__LINE__,#c); }}while(0)
static const unsigned KEY=0xF81F;
static uint32_t rng=0x291BB79Du;
static uint32_t next_random(){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static void put_pixel(unsigned char* p,int bpp,uint32_t value){memcpy(p,&value,(size_t)bpp/8);}

struct GuardRows {
    unsigned char* allocation; unsigned char* pixels;
    size_t page, committed, stride; int rows;
    GuardRows(int row_bytes,int height,bool tail,int skew):allocation(0),pixels(0),rows(height){
        SYSTEM_INFO info;GetSystemInfo(&info);page=info.dwPageSize;
        committed=((size_t)row_bytes+skew+page-1)/page*page;
        stride=committed+page;
        allocation=(unsigned char*)VirtualAlloc(NULL,page+stride*rows,MEM_RESERVE,PAGE_NOACCESS);
        if(!allocation)throw std::runtime_error("reserve");
        for(int y=0;y<rows;++y){
            unsigned char* p=allocation+page+y*stride;
            if(!VirtualAlloc(p,committed,MEM_COMMIT,PAGE_READWRITE))throw std::runtime_error("commit");
            memset(p,0xA5,committed);
        }
        pixels=allocation+page+(tail?committed-row_bytes-skew:0)+skew;
    }
    ~GuardRows(){if(allocation)VirtualFree(allocation,0,MEM_RELEASE);}
    unsigned char* row(int y){return pixels+y*stride;}
    void readonly(){for(int y=0;y<rows;++y){DWORD old;VirtualProtect(allocation+page+y*stride,committed,PAGE_READONLY,&old);}}
    bool equal(const GuardRows& other)const{
        if(committed!=other.committed||rows!=other.rows)return false;
        for(int y=0;y<rows;++y)if(memcmp(allocation+page+y*stride,other.allocation+page+y*stride,committed))return false;
        return true;
    }
};
static void guard_tests(){
    const int widths[]={1,2,7,8,9,15,16,17,31,32,33,63,64,65,257,511,1599,1600};
    const int heights[]={1,3,32}; const int bits[]={8,16,32};
    for(int bi=0;bi<3;++bi)for(int range=0;range<2;++range)for(int tail=0;tail<2;++tail)
    for(int hi=0;hi<3;++hi)for(unsigned wi=0;wi<sizeof(widths)/sizeof(widths[0]);++wi){
        const int bpp=bits[bi],bytes=bpp/8,w=widths[wi],h=heights[hi];
        const int sx=1,dx=2,sy=1,dy=1,skew=(int)(wi&1);
        GuardRows source((w+sx)*bytes,h+sy,tail!=0,skew);
        GuardRows scalar((w+dx)*bytes,h+dy,tail!=0,skew);
        GuardRows simd((w+dx)*bytes,h+dy,tail!=0,skew);
        unsigned kl=bpp==8?0x81:KEY,kh=range?kl+7:kl;
        for(int y=0;y<h;++y)for(int x=0;x<w;++x){
            uint32_t value=next_random();
            if((x+y*3)%5)value=kl+((x+y)%3==0?(kh-kl):0);
            if(bpp==32)value|=(uint32_t)((x+y)&255)<<24;
            put_pixel(source.row(y+sy)+(x+sx)*bytes,bpp,value);
        }
        source.readonly();
        blt_colorkey_scalar(scalar.pixels,dx,dy,w,h,(int)scalar.stride,source.pixels,sx,sy,(int)source.stride,kl,kh,bpp);
        blt_colorkey_candidate(simd.pixels,dx,dy,w,h,(int)simd.stride,source.pixels,sx,sy,(int)source.stride,kl,kh,bpp);
        CHECK(scalar.equal(simd));
    }
    /* All-transparent blocks must avoid even reading inaccessible destination pixels. */
    GuardRows source(64,1,true,0);for(int x=0;x<32;++x)put_pixel(source.pixels+x*2,16,KEY);
    unsigned char* forbidden=(unsigned char*)VirtualAlloc(NULL,4096,MEM_RESERVE,PAGE_NOACCESS);
    CHECK(forbidden!=NULL);
    blt_colorkey_candidate(forbidden,0,0,32,1,64,source.pixels,0,0,64,KEY,KEY,16);
    VirtualFree(forbidden,0,MEM_RELEASE);
    CHECK(true);
}
static void random_fastpath_tests(){
    const int densities[]={0,1,5,10,50,100};
    for(unsigned case_index=0;case_index<720;++case_index){
        int w=8+(int)(next_random()%250),h=1+(int)(next_random()%64);
        int sx=(int)(next_random()%4),dx=(int)(next_random()%4),sy=(int)(next_random()%3),dy=(int)(next_random()%3);
        int sp=2*(sx+w+(int)(next_random()%24)),dp=2*(dx+w+(int)(next_random()%24));
        int skew=(int)(case_index%2),density=densities[case_index%6];
        std::vector<unsigned char> source(sp*(sy+h)+skew+32,0x45),scalar(dp*(dy+h)+skew+32,0xA9),simd=scalar;
        unsigned char* src=source.data()+skew;
        for(int y=0;y<h;++y)for(int x=0;x<w;++x){
            unsigned short value=(unsigned short)next_random();
            if(value==KEY)value=0;
            if((int)(next_random()%100)>=density)value=KEY;
            put_pixel(src+(y+sy)*sp+(x+sx)*2,16,value);
        }
        std::vector<unsigned char> original_source=source;
        unsigned long before=candidate_fast_calls;
        blt_colorkey_scalar(scalar.data()+skew,dx,dy,w,h,dp,src,sx,sy,sp,KEY,KEY,16);
        blt_colorkey_candidate(simd.data()+skew,dx,dy,w,h,dp,src,sx,sy,sp,KEY,KEY,16);
        CHECK(scalar==simd);CHECK(source==original_source);
        CHECK(candidate_fast_calls==before+(candidate_has_sse2?1:0));
    }
    /* The narrow GRID texture may be stored tightly or padded to64pixels. */
    const int pitches[]={124,128,720};
    for(int i=0;i<3;++i){
        const int sp=pitches[i],dp=3200,w=62,h=32;
        std::vector<unsigned char> source(sp*h),scalar(dp*h,0xC8),simd=scalar;
        for(int y=0;y<h;++y)for(int x=0;x<w;++x)put_pixel(source.data()+y*sp+x*2,16,((x+y)%31)?KEY:0x0800);
        unsigned long before=candidate_fast_calls;
        blt_colorkey_scalar(scalar.data(),0,0,w,h,dp,source.data(),0,0,sp,KEY,KEY,16);
        blt_colorkey_candidate(simd.data(),0,0,w,h,dp,source.data(),0,0,sp,KEY,KEY,16);
        CHECK(scalar==simd);CHECK(candidate_fast_calls==before+(candidate_has_sse2?1:0));
    }
}
static void invalid_requests_are_not_handled(){
    unsigned char source[128],destination[128],original[128];
    memset(source,0,sizeof(source));memset(destination,0x75,sizeof(destination));memcpy(original,destination,sizeof(original));
    CHECK(!c4_blt_colorkey16(destination,0,0,8,2,16,source,0,0,16,KEY,KEY,16,FALSE));
    CHECK(!c4_blt_colorkey16(NULL,0,0,8,2,16,source,0,0,16,KEY,KEY,16,TRUE));
    CHECK(!c4_blt_colorkey16(destination,0,0,8,2,16,NULL,0,0,16,KEY,KEY,16,TRUE));
    CHECK(!c4_blt_colorkey16(destination,-1,0,8,2,16,source,0,0,16,KEY,KEY,16,TRUE));
    CHECK(!c4_blt_colorkey16(destination,0,0,8,2,16,source,0,-1,16,KEY,KEY,16,TRUE));
    CHECK(!c4_blt_colorkey16(destination,0,0,8,2,15,source,0,0,16,KEY,KEY,16,TRUE));
    CHECK(!c4_blt_colorkey16(destination,0,0,8,2,16,source,1,0,16,KEY,KEY,16,TRUE));
    CHECK(!c4_blt_colorkey16(destination,0,0,0,2,16,source,0,0,16,KEY,KEY,16,TRUE));
    CHECK(!c4_blt_colorkey16(destination,0,0,8,0,16,source,0,0,16,KEY,KEY,16,TRUE));
    CHECK(!c4_blt_colorkey16((unsigned char*)((uintptr_t)-1-8),0,0,8,2,16,source,0,0,16,KEY,KEY,16,TRUE));
    CHECK(!c4_blt_colorkey16(destination,0,0,8,2,16,(unsigned char*)0x1000,0,0x7FFFFFFF,0x7FFFFFFE,KEY,KEY,16,TRUE));
    CHECK(!c4_blt_colorkey16(destination,0,0,8,2,16,source,0,0,16,KEY,KEY+1,16,TRUE));
    CHECK(!memcmp(destination,original,sizeof(original)));
}
static void fallback_tests(){
    const int pitch=160,height=12,w=32,h=5;
    for(int dy=0;dy<3;++dy)for(int dx=0;dx<9;++dx){
        std::vector<unsigned char> a(pitch*height),b;
        for(size_t i=0;i<a.size();i+=2)put_pixel(a.data()+i,16,(i%11)?KEY:next_random());
        b=a;
        const unsigned long before=candidate_fallback_calls;
        blt_colorkey_scalar(a.data(),dx,dy,w,h,pitch,a.data(),4,1,pitch,KEY,KEY,16);
        blt_colorkey_candidate(b.data(),dx,dy,w,h,pitch,b.data(),4,1,pitch,KEY,KEY,16);
        CHECK(a==b);CHECK(candidate_fallback_calls==before+1);
    }
    std::vector<unsigned char> source(640),a(640,0x63),b=a;
    for(size_t i=0;i<source.size();i+=2)put_pixel(source.data()+i,16,(i%5)?KEY:next_random());
    const BOOL supported=candidate_has_sse2;candidate_has_sse2=FALSE;
    unsigned long before=candidate_fallback_calls;
    blt_colorkey_scalar(a.data(),0,0,32,10,64,source.data(),0,0,64,KEY,KEY,16);
    blt_colorkey_candidate(b.data(),0,0,32,10,64,source.data(),0,0,64,KEY,KEY,16);
    CHECK(a==b);CHECK(candidate_fallback_calls==before+1);candidate_has_sse2=supported;
    /* Original 16-bit behavior truncates both full-width keys before comparing. */
    std::fill(a.begin(),a.end(),(unsigned char)0x63);b=a;
    blt_colorkey_scalar(a.data(),0,0,32,10,64,source.data(),0,0,64,KEY|0xCAFE0000u,KEY|0xBABE0000u,16);
    blt_colorkey_candidate(b.data(),0,0,32,10,64,source.data(),0,0,64,KEY|0xCAFE0000u,KEY|0xBABE0000u,16);
    CHECK(a==b);
}

using Blit=void (*)(unsigned char*,int,int,int,int,int,unsigned char*,int,int,int,unsigned int,unsigned int,int);
static volatile uint32_t sink;
static double frequency(){LARGE_INTEGER f;QueryPerformanceFrequency(&f);return (double)f.QuadPart;}
static double run_timing(Blit fn,unsigned char* dst,unsigned char* src,int w,int h,int dp,int sp,int iterations){
    LARGE_INTEGER start,end;QueryPerformanceCounter(&start);
    for(int i=0;i<iterations;++i)fn(dst,0,0,w,h,dp,src,0,0,sp,KEY,KEY,16);
    QueryPerformanceCounter(&end);sink+=dst[0]+dst[(h-1)*dp];
    return (double)(end.QuadPart-start.QuadPart)/frequency()*1e9/iterations;
}
static void benchmark(){
    const int sizes[][3]={{32,32,720},{62,32,124},{62,32,128},{64,32,720},{64,64,720},{1600,900,3200}};
    const int densities[]={0,1,5,10,100};
    puts("Synthetic diagonal grid; hot source/destination; single-thread QPC medians of seven alternating rounds.");
    puts("width,height,opaque_percent,src_pitch,dst_pitch,iterations,scalar_ns,candidate_ns,speedup");
    for(unsigned shape=0;shape<6;++shape)for(unsigned di=0;di<5;++di){
        int w=sizes[shape][0],h=sizes[shape][1],sp=sizes[shape][2],dp=3200,opaque=densities[di];
        std::vector<unsigned char> source(sp*h,0),dest(dp*h,0x29);
        unsigned actual_opaque=0;
        for(int y=0;y<h;++y)for(int x=0;x<w;++x){
            /* Sparse diagonal rows resemble a grid, without fabricating a capture. */
            bool visible=(((x+y*3)%100)<opaque);
            put_pixel(source.data()+y*sp+x*2,16,visible?(0x0400+((x+y)&255)):KEY);
            if(visible)++actual_opaque;
        }
        for(int i=0;i<20;++i){blt_colorkey_scalar(dest.data(),0,0,w,h,dp,source.data(),0,0,sp,KEY,KEY,16);blt_colorkey_candidate(dest.data(),0,0,w,h,dp,source.data(),0,0,sp,KEY,KEY,16);}
        int iterations=shape==5?16:5000;
        double pilot=run_timing(blt_colorkey_scalar,dest.data(),source.data(),w,h,dp,sp,iterations);
        iterations=(int)std::max(16.0,std::min(200000.0,50000000.0/pilot));
        std::vector<double> scalar,candidate;
        for(int round=0;round<7;++round){
            if(round&1){candidate.push_back(run_timing(blt_colorkey_candidate,dest.data(),source.data(),w,h,dp,sp,iterations));scalar.push_back(run_timing(blt_colorkey_scalar,dest.data(),source.data(),w,h,dp,sp,iterations));}
            else{scalar.push_back(run_timing(blt_colorkey_scalar,dest.data(),source.data(),w,h,dp,sp,iterations));candidate.push_back(run_timing(blt_colorkey_candidate,dest.data(),source.data(),w,h,dp,sp,iterations));}
        }
        std::sort(scalar.begin(),scalar.end());std::sort(candidate.begin(),candidate.end());
        printf("%d,%d,%.3f,%d,%d,%d,%.1f,%.1f,%.3f\n",w,h,100.0*actual_opaque/(w*h),sp,dp,iterations,scalar[3],candidate[3],scalar[3]/candidate[3]);
    }
}
struct CaptureRect { int x,y,w,h,sx,sy; };
static double capture_timing(Blit fn,std::vector<unsigned char>& dst,std::vector<unsigned char>& src,const std::vector<CaptureRect>& rects,int iterations){
    LARGE_INTEGER start,end;QueryPerformanceCounter(&start);
    for(int i=0;i<iterations;++i)for(size_t j=0;j<rects.size();++j){
        const CaptureRect& r=rects[j];
        fn(dst.data(),r.x,r.y,r.w,r.h,3200,src.data(),r.sx,r.sy,128,KEY,KEY,16);
    }
    QueryPerformanceCounter(&end);sink+=dst[10000];
    return (double)(end.QuadPart-start.QuadPart)/frequency()*1e9/iterations;
}
static void captured_grid(const char* sprite_path,const char* rect_path,bool timing){
    FILE* sprite=fopen(sprite_path,"rb");FILE* rect_file=fopen(rect_path,"r");
    CHECK(sprite!=NULL && rect_file!=NULL);
    if(!sprite || !rect_file){if(sprite)fclose(sprite);if(rect_file)fclose(rect_file);return;}
    std::vector<unsigned char> source(4096),scalar(3200*900),simd;
    CHECK(fread(source.data(),1,source.size(),sprite)==source.size());fclose(sprite);
    for(size_t i=0;i<scalar.size();++i)scalar[i]=(unsigned char)next_random();simd=scalar;
    std::vector<CaptureRect> rects;CaptureRect r;
    while(fscanf(rect_file,"%d %d %d %d %d %d",&r.x,&r.y,&r.w,&r.h,&r.sx,&r.sy)==6)rects.push_back(r);
    fclose(rect_file);CHECK(!rects.empty());
    for(size_t i=0;i<rects.size();++i){
        r=rects[i];
        blt_colorkey_scalar(scalar.data(),r.x,r.y,r.w,r.h,3200,source.data(),r.sx,r.sy,128,KEY,KEY,16);
        blt_colorkey_candidate(simd.data(),r.x,r.y,r.w,r.h,3200,source.data(),r.sx,r.sy,128,KEY,KEY,16);
    }
    CHECK(scalar==simd);
    unsigned opaque=0;for(int y=0;y<32;++y)for(int x=0;x<62;++x){unsigned short value;memcpy(&value,source.data()+y*128+x*2,2);opaque+=value!=KEY;}
    printf("Captured GRID:62x32,pitch128,opaque=%u/1984(%.3f%%),visiblepacket=%u blits; exact pixel equality=%s\n",opaque,100.0*opaque/1984,(unsigned)rects.size(),scalar==simd?"yes":"NO");
    if(!timing)return;
    int iterations=16;double pilot=capture_timing(blt_colorkey_scalar,scalar,source,rects,iterations);
    iterations=(int)std::max(8.0,std::min(500.0,50000000.0/pilot));
    std::vector<double> a,b,one_a,one_b;
    for(int i=0;i<7;++i){
        if(i&1){b.push_back(capture_timing(blt_colorkey_candidate,simd,source,rects,iterations));a.push_back(capture_timing(blt_colorkey_scalar,scalar,source,rects,iterations));}
        else{a.push_back(capture_timing(blt_colorkey_scalar,scalar,source,rects,iterations));b.push_back(capture_timing(blt_colorkey_candidate,simd,source,rects,iterations));}
        one_a.push_back(run_timing(blt_colorkey_scalar,scalar.data(),source.data(),62,32,3200,128,10000));
        one_b.push_back(run_timing(blt_colorkey_candidate,simd.data(),source.data(),62,32,3200,128,10000));
    }
    std::sort(a.begin(),a.end());std::sort(b.begin(),b.end());std::sort(one_a.begin(),one_a.end());std::sort(one_b.begin(),one_b.end());
    printf("Captured one full tile scalar_ns=%.1f candidate_ns=%.1f speedup=%.3f\n",one_a[3],one_b[3],one_a[3]/one_b[3]);
    printf("Captured visible packet scalar_ms=%.4f candidate_ms=%.4f speedup=%.3f iterations=%d\n",a[3]/1e6,b[3]/1e6,a[3]/b[3],iterations);
}
int main(int argc,char** argv){
    const bool run_benchmark=argc>1 && strcmp(argv[1],"--benchmark")==0;
    DWORD_PTR process_mask=0,system_mask=0;
    if(run_benchmark && GetProcessAffinityMask(GetCurrentProcess(),&process_mask,&system_mask)){
        DWORD_PTR pin=1;while((pin<<1) && (pin<<1)<=process_mask)pin<<=1;
        while(pin && !(pin&process_mask))pin>>=1;
        if(pin)printf("Benchmark thread affinity set: 0x%lx (previous0x%lx)\n",(unsigned long)pin,(unsigned long)SetThreadAffinityMask(GetCurrentThread(),pin));
    }
    char brand[49]={};int info[4];
    for(unsigned leaf=0;leaf<3;++leaf){__cpuid(info,(int)(0x80000002u+leaf));memcpy(brand+leaf*16,info,16);}
    printf("CPU: %s\n",brand);
    printf("SSE2 available: %s; Win32 pointer bits: %d; compiler: %d\n",candidate_has_sse2?"yes":"no",(int)(sizeof(void*)*8),_MSC_VER);
    guard_tests();fallback_tests();random_fastpath_tests();invalid_requests_are_not_handled();
    printf("Differential checks: %u passed, %u failed; fast calls %lu, fallback calls %lu\n",checks-failures,failures,candidate_fast_calls,candidate_fallback_calls);
    if(failures)return 1;
    if(!candidate_has_sse2){puts("No SSE2: performance comparison omitted.");return 0;}
    if(run_benchmark)benchmark();
    if(argc>=4)captured_grid(argv[2],argv[3],run_benchmark);
    printf("Final checks: %u passed, %u failed\n",checks-failures,failures);
    printf("Checksum sink: %lu\n",(unsigned long)sink);return failures?1:0;
}
