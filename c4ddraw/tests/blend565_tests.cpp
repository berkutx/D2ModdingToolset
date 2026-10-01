#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include "../features/blend565.h"
#include "../features/blend565_install.h"
#ifdef C4_BLEND_NATIVE_ORACLE
#include "blend565-native-bytes.h"
#endif

using c4blend565::Point;
using c4blend565::Size;
using c4blend565::Operation;
static unsigned checks, failures, handled_calls, fallback_calls;
static uint32_t rng=0x251CD836u;
static bool cpu_sse2=IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE)!=FALSE;
#define CHECK(c) do { ++checks; if(!(c)) { ++failures; printf("FAIL line%d: %s\n",__LINE__,#c); } } while(0)
static uint32_t random32(){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static uint16_t read16(const void* p){uint16_t v;memcpy(&v,p,2);return v;}
static void put16(void* p,uint16_t v){memcpy(p,&v,2);}

/* Independent per-channel scalar specification. Pair reads intentionally preserve
 * the native full32-bit key sentinel behavior even when key is outside16-bit range. */
static uint16_t model_pixel(Operation op,uint16_t source,uint16_t destination){
    const unsigned shift[]={11,5,0},mask[]={31,63,31};unsigned out=0;
    for(int channel=0;channel<3;++channel){
        unsigned s=(source>>shift[channel])&mask[channel],d=(destination>>shift[channel])&mask[channel],value;
        if(op==c4blend565::Half)value=s/2+d/2;
        else if(op==c4blend565::Add)value=std::min(mask[channel],s+d);
        else value=d>s?d-s:0;
        out|=value<<shift[channel];
    }
    return (uint16_t)out;
}
static void model(Operation op,const void* source,int sp,const Point* ss,void* destination,int dp,const Point* ds,const Size* size,int opacity,uint32_t key){
    if(op==c4blend565::Half && ((uint8_t)opacity==0||(uint8_t)opacity==255))return;
    const unsigned char* s=(const unsigned char*)source+ss->y*sp+ss->x*2;
    unsigned char* d=(unsigned char*)destination+ds->y*dp+ds->x*2;
    for(int y=0;y<size->height;++y){
        int x=0;
        if(op==c4blend565::Half){
            for(;x+2<=size->width;x+=2){
                uint32_t pair;memcpy(&pair,s+x*2,4);
                if(pair==(key|(key<<16)))continue;
                uint16_t a=(uint16_t)pair,b=(uint16_t)(pair>>16);
                uint16_t da=read16(d+x*2),db=read16(d+x*2+2);
                if((uint32_t)a!=key)put16(d+x*2,model_pixel(op,a,da));
                if((uint32_t)b!=key)put16(d+x*2+2,model_pixel(op,b,db));
            }
        }
        for(;x<size->width;++x){
            uint16_t value=read16(s+x*2);
            if((op==c4blend565::Half && (uint32_t)value==key)||(op!=c4blend565::Half && value==0))continue;
            put16(d+x*2,model_pixel(op,value,read16(d+x*2)));
        }
        if(y+1<size->height){s+=sp;d+=dp;}
    }
}
#ifdef C4_BLEND_NATIVE_ORACLE
using Native7=void (__stdcall*)(const void*,int,const Point*,void*,int,const Point*,const Size*);
using Native9=void (__stdcall*)(const void*,int,const Point*,void*,int,const Point*,const Size*,int,uint32_t);
static Native9 native_alpha;static Native7 native_add,native_sub;
static void* load_oracle(const unsigned char* bytes,size_t length){
    void* p=VirtualAlloc(NULL,length,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!p)throw std::runtime_error("oracle allocation failed");memcpy(p,bytes,length);DWORD old;
    if(!VirtualProtect(p,length,PAGE_EXECUTE_READ,&old))throw std::runtime_error("oracle protection failed");
    FlushInstructionCache(GetCurrentProcess(),p,length);return p;
}
#endif
static void oracle(Operation op,const void* s,int sp,const Point* ss,void* d,int dp,const Point* ds,const Size* size,int opacity,uint32_t key){
#ifdef C4_BLEND_NATIVE_ORACLE
    if(op==c4blend565::Half)native_alpha(s,sp,ss,d,dp,ds,size,opacity,key);
    else if(op==c4blend565::Add)native_add(s,sp,ss,d,dp,ds,size);
    else native_sub(s,sp,ss,d,dp,ds,size);
#else
    model(op,s,sp,ss,d,dp,ds,size,opacity,key);
#endif
}
static bool apply_with_fallback(Operation op,const void* s,int sp,const Point* ss,void* d,int dp,const Point* ds,const Size* size,int opacity,uint32_t key){
    bool result=c4blend565::apply(op,s,sp,ss,d,dp,ds,size,opacity,key,cpu_sse2);
    if(result)++handled_calls;else{++fallback_calls;oracle(op,s,sp,ss,d,dp,ds,size,opacity,key);}
    return result;
}
static void compare_case(Operation op,int width,int height,int opacity,uint32_t key,int pattern){
    Point ss={(int)(random32()%4),(int)(random32()%3)},ds={(int)(random32()%4),(int)(random32()%3)};Size size={width,height};
    int sp=2*(ss.x+width+(int)(random32()%16)+1),dp=2*(ds.x+width+(int)(random32()%16)+1),skew=pattern&1;
    std::vector<unsigned char> source(sp*(ss.y+height)+32+skew),expected(dp*(ds.y+height)+32+skew),actual,spec;
    for(size_t i=0;i<source.size();++i)source[i]=(unsigned char)random32();
    for(size_t i=0;i<expected.size();++i)expected[i]=(unsigned char)random32();
    uint32_t packed=key|(key<<16);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x){
        unsigned selector=(unsigned)(x+y+pattern)%11;uint16_t value=read16(source.data()+skew+(ss.y+y)*sp+(ss.x+x)*2);
        if(selector==0)value=0;if(selector==1)value=0xFFFF;if(selector==2)value=(uint16_t)key;
        put16(source.data()+skew+(ss.y+y)*sp+(ss.x+x)*2,value);
    }
    if(width>=2)for(int y=0;y<height;++y)memcpy(source.data()+skew+(ss.y+y)*sp+ss.x*2,&packed,4);
    actual=expected;spec=expected;std::vector<unsigned char> before_source=source;
    model(op,source.data()+skew,sp,&ss,spec.data()+skew,dp,&ds,&size,opacity,key);
    oracle(op,source.data()+skew,sp,&ss,expected.data()+skew,dp,&ds,&size,opacity,key);
    bool handled=apply_with_fallback(op,source.data()+skew,sp,&ss,actual.data()+skew,dp,&ds,&size,opacity,key);
    CHECK(actual==expected);CHECK(spec==expected);CHECK(source==before_source);
    const bool noop=!width||!height||(op==c4blend565::Half&&((uint8_t)opacity==0||(uint8_t)opacity==255));
    CHECK(handled==(noop||(cpu_sse2&&width>=8)));
    if(actual!=expected||spec!=expected){printf("Mismatch op%d width%d height%d opacity%d key%08x\n",(int)op,width,height,opacity,key);throw std::runtime_error("pixel mismatch");}
}
static void differential(){
    const int widths[]={1,2,3,7,8,9,15,16,17,31,32,33,62,63,64,65,257,1600};
    const int alpha[]={0,1,2,64,127,128,129,200,254,255,256,257,511,-1};
    const uint32_t keys[]={0,1,0xF81F,0xFFFF,0xFFFFFFFF,0x1FFFF,0x12345678,0xFFFF0000};
    for(unsigned wi=0;wi<18;++wi)for(unsigned ki=0;ki<8;++ki)for(unsigned ai=0;ai<14;++ai)
        compare_case(c4blend565::Half,widths[wi],1+(int)(random32()%17),alpha[ai],keys[ki],(int)(wi+ki+ai));
    for(int mode=1;mode<3;++mode)for(unsigned wi=0;wi<18;++wi)for(int pattern=0;pattern<30;++pattern)
        compare_case((Operation)mode,widths[wi],1+(int)(random32()%33),128,0,pattern);
    for(int mode=0;mode<3;++mode){compare_case((Operation)mode,0,3,128,0xF81F,0);compare_case((Operation)mode,17,0,128,0xF81F,0);}
    uint16_t white[3]={0xFFFF,0xFFFF,0xFFFF},destination[3]={0xFFFF,0xFFFF,0xFFFF};Point p={};Size size={3,1};
    apply_with_fallback(c4blend565::Half,white,6,&p,destination,6,&p,&size,128,0xFFFFFFFF);
    CHECK(destination[0]==0xFFFF&&destination[1]==0xFFFF&&destination[2]==0xF7DE);
}
struct GuardRows{unsigned char* allocation;unsigned char* pixels;int pitch,rows;size_t page;
    GuardRows(int bytes,int height,bool tail,int skew):rows(height){
        SYSTEM_INFO info;GetSystemInfo(&info);page=info.dwPageSize;pitch=(int)page*2;
        allocation=(unsigned char*)VirtualAlloc(NULL,page+(size_t)pitch*height,MEM_RESERVE,PAGE_NOACCESS);if(!allocation)throw std::runtime_error("guard reserve");
        for(int y=0;y<height;++y){void* p=allocation+page+y*pitch;if(!VirtualAlloc(p,page,MEM_COMMIT,PAGE_READWRITE))throw std::runtime_error("guard commit");memset(p,0x45,page);}
        pixels=allocation+page+(tail?page-bytes-skew:0)+skew;
    }
    ~GuardRows(){VirtualFree(allocation,0,MEM_RELEASE);}
    void readonly(){for(int y=0;y<rows;++y){DWORD old;VirtualProtect(allocation+page+y*pitch,page,PAGE_READONLY,&old);}}
};
static void guards(){
    const int widths[]={1,2,7,8,9,15,16,17,31,32,33,62,63,64,65};Point p={};
    for(int mode=0;mode<3;++mode)for(int end=0;end<2;++end)for(int skew=0;skew<2;++skew)for(unsigned wi=0;wi<15;++wi){
        Size size={widths[wi],3};GuardRows source(size.width*2,3,end!=0,skew),expected(size.width*2,3,end!=0,skew),actual(size.width*2,3,end!=0,skew);
        for(int y=0;y<3;++y)for(int x=0;x<size.width;++x)put16(source.pixels+y*source.pitch+x*2,(x%5)?(uint16_t)random32():0xF81F);
        source.readonly();oracle((Operation)mode,source.pixels,source.pitch,&p,expected.pixels,expected.pitch,&p,&size,128,0xF81F);
        apply_with_fallback((Operation)mode,source.pixels,source.pitch,&p,actual.pixels,actual.pitch,&p,&size,128,0xF81F);
        bool same=true;for(int y=0;y<3;++y)if(memcmp(expected.allocation+expected.page+y*expected.pitch,actual.allocation+actual.page+y*actual.pitch,expected.page))same=false;CHECK(same);
    }
}
static void validation(){
    unsigned char source[1024],destination[1024],original[1024];memset(source,0x32,sizeof(source));memset(destination,0x75,sizeof(destination));memcpy(original,destination,sizeof(original));
    Point p={};Size z={8,2};
#define REJECT(op,s,sp,ss,d,dp,ds,size,opacity,key,cpu) do {CHECK(!c4blend565::apply(op,s,sp,ss,d,dp,ds,size,opacity,key,cpu));CHECK(!memcmp(destination,original,sizeof(original)));} while(0)
    REJECT((Operation)99,source,16,&p,destination,16,&p,&z,128,0,true);
    for(int mode=0;mode<3;++mode){Operation op=(Operation)mode;
        REJECT(op,source,16,&p,destination,16,&p,&z,128,0,false);
        REJECT(op,NULL,16,&p,destination,16,&p,&z,128,0,true);
        REJECT(op,source,16,NULL,destination,16,&p,&z,128,0,true);
        REJECT(op,source,16,&p,NULL,16,&p,&z,128,0,true);
        REJECT(op,source,16,&p,destination,16,NULL,&z,128,0,true);
        REJECT(op,source,16,&p,destination,16,&p,NULL,128,0,true);
        Point negative={-1,0},bad_y={0,-1},past_row={1,0},far_x={INT_MAX/16+1,0},far_y={0,INT_MAX};
        Size negative_w={-1,2},negative_h={8,-1},narrow={7,2},large={8,INT_MAX};
        REJECT(op,source,16,&negative,destination,16,&p,&z,128,0,true);
        REJECT(op,source,16,&p,destination,16,&bad_y,&z,128,0,true);
        REJECT(op,source,16,&past_row,destination,16,&p,&z,128,0,true);
        REJECT(op,source,16,&p,destination,16,&past_row,&z,128,0,true);
        REJECT(op,source,15,&p,destination,16,&p,&z,128,0,true);
        REJECT(op,source,16,&p,destination,-16,&p,&z,128,0,true);
        REJECT(op,source,0,&p,destination,16,&p,&z,128,0,true);
        REJECT(op,source,16,&p,destination,16,&p,&negative_w,128,0,true);
        REJECT(op,source,16,&p,destination,16,&p,&negative_h,128,0,true);
        REJECT(op,source,16,&p,destination,16,&p,&narrow,128,0,true);
        REJECT(op,source,INT_MAX-1,&far_x,destination,16,&p,&z,128,0,true);
        REJECT(op,(void*)0x1000,INT_MAX-1,&far_y,destination,16,&p,&z,128,0,true);
        REJECT(op,(void*)((uintptr_t)-1-8),16,&p,destination,16,&p,&z,128,0,true);
        REJECT(op,source,INT_MAX-1,&p,destination,16,&p,&large,128,0,true);
        REJECT(op,destination,16,&p,destination,16,&p,&z,128,0,true);
        REJECT(op,destination+2,16,&p,destination,16,&p,&z,128,0,true);
        REJECT(op,destination,16,&p,destination+2,16,&p,&z,128,0,true);
        Size empty_w={0,2},empty_h={8,0};
        CHECK(c4blend565::apply(op,NULL,0,NULL,NULL,0,NULL,&empty_w,128,0,false));
        CHECK(c4blend565::apply(op,NULL,0,NULL,NULL,0,NULL,&empty_h,128,0,false));
    }
    CHECK(c4blend565::apply(c4blend565::Half,NULL,0,NULL,NULL,0,NULL,NULL,0,0,false));
    CHECK(c4blend565::apply(c4blend565::Half,NULL,0,NULL,NULL,0,NULL,NULL,255,0,false));
    CHECK(c4blend565::apply(c4blend565::Half,NULL,0,NULL,NULL,0,NULL,NULL,256,0,false));
    CHECK(c4blend565::apply(c4blend565::Half,NULL,0,NULL,NULL,0,NULL,NULL,-1,0,false));
    CHECK(!memcmp(destination,original,sizeof(original)));
#undef REJECT
    /* Wrapper fallback preserves the original sequential self-blit behavior. */
    for(int mode=0;mode<3;++mode)for(int dy=0;dy<3;++dy)for(int dx=0;dx<9;++dx){
        std::vector<unsigned char> expected(160*12),actual;
        for(size_t i=0;i<expected.size();++i)expected[i]=(unsigned char)random32();actual=expected;
        Point ss={4,1},ds={dx,dy};Size size={32,5};
        oracle((Operation)mode,expected.data(),160,&ss,expected.data(),160,&ds,&size,128,0xF81F);
        CHECK(!apply_with_fallback((Operation)mode,actual.data(),160,&ss,actual.data(),160,&ds,&size,128,0xF81F));CHECK(actual==expected);
    }
}
static void store16(std::vector<uint8_t>& data,size_t offset,uint16_t value){memcpy(data.data()+offset,&value,2);}
static void store32(std::vector<uint8_t>& data,size_t offset,uint32_t value){memcpy(data.data()+offset,&value,4);}
static std::vector<uint8_t> fake_pe(){
    std::vector<uint8_t> data(1024);const size_t pe=0x80,optional=pe+24,sections=optional+224;
    store16(data,0,0x5A4D);store32(data,60,(uint32_t)pe);store32(data,pe,0x4550);
    store16(data,pe+4,0x14C);store16(data,pe+6,3);store16(data,pe+20,224);store16(data,pe+22,0x0102);
    store16(data,optional,0x10B);store32(data,optional+16,0x26D6E0);store32(data,optional+28,0x400000);
    store32(data,optional+32,0x1000);store32(data,optional+36,0x200);store32(data,optional+56,0x485000);store32(data,optional+60,1024);
    const char* names[]={".text",".rdata",".data"};const uint32_t rvas[]={0x1000,0x2CE000,0x38E000},sizes[]={0x2CC2EC,0xBFFE8,0xAD234},flags[]={0x60000020,0x40000040,0xC0000040};
    for(unsigned i=0;i<3;++i){size_t at=sections+i*40;memcpy(data.data()+at,names[i],strlen(names[i]));store32(data,at+8,sizes[i]);store32(data,at+12,rvas[i]);store32(data,at+36,flags[i]);}
    return data;
}
static std::vector<uint8_t> fake_version(){
    std::vector<uint8_t> data(92);store16(data,0,92);store16(data,2,52);const char key[]="VS_VERSION_INFO";
    for(size_t i=0;i<sizeof(key);++i)store16(data,6+i*2,(uint16_t)key[i]);
    store32(data,40,0xFEEF04BD);store32(data,44,0x10000);store32(data,48,0x07D3000C);store32(data,52,0x000B0001);return data;
}
static void installation_gates(){
    namespace install=c4blend565install;install::PeLayout layout={};std::vector<uint8_t> good=fake_pe(),bad;
    CHECK(install::validPe(good.data(),good.size(),0x400000,layout)&&layout.imageSize==0x485000);
    CHECK(!install::validPe(good.data(),good.size(),0x500000,layout)&&layout.imageSize==0);
    CHECK(!install::validPe(NULL,good.size(),0x400000,layout));
    const size_t cuts[]={0,2,63,64,0x80,0x97,0x100,0x178,0x1EF,1023};
    for(unsigned i=0;i<sizeof(cuts)/sizeof(cuts[0]);++i)CHECK(!install::validPe(good.data(),cuts[i],0x400000,layout));
    const size_t offsets[]={0,60,0x80,0x84,0x86,0x94,0x98,0xA8,0xB4,0xB8,0xBC,0xD0,0xD4,0x178,0x180,0x184,0x19C};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);++i){bad=good;bad[offsets[i]]^=1;CHECK(!install::validPe(bad.data(),bad.size(),0x400000,layout));}
    bad=good;store16(bad,0x84,0x8664);CHECK(!install::validPe(bad.data(),bad.size(),0x400000,layout));
    bad=good;store16(bad,0x96,0x0103);CHECK(install::validPe(bad.data(),bad.size(),0x400000,layout));
    bad=good;store16(bad,0x96,0x2102);CHECK(!install::validPe(bad.data(),bad.size(),0x400000,layout));
    bad=good;store32(bad,60,0xFFFFFFFC);CHECK(!install::validPe(bad.data(),bad.size(),0x400000,layout));
    bad=good;store32(bad,0x178+40+12,0x1000);CHECK(!install::validPe(bad.data(),bad.size(),0x400000,layout));
    bad=good;store32(bad,0x178+80+8,0xFFFFFFFF);CHECK(!install::validPe(bad.data(),bad.size(),0x400000,layout));
    // An icon/resource-only edit can change timestamp, resource sections and image size.
    bad=good;store32(bad,0x88,0x12345678);store16(bad,0x86,5);store32(bad,0xD0,0x450000);
    const size_t rsrc=0x178+3*40,reloc=rsrc+40;memcpy(bad.data()+rsrc,".rsrc",5);store32(bad,rsrc+8,0x3000);store32(bad,rsrc+12,0x440000);store32(bad,rsrc+36,0x40000040);
    memcpy(bad.data()+reloc,".reloc",6);store32(bad,reloc+8,0x2000);store32(bad,reloc+12,0x443000);store32(bad,reloc+36,0x42000040);
    CHECK(install::validPe(bad.data(),bad.size(),0x400000,layout)&&layout.imageSize==0x450000);
    good=fake_version();CHECK(install::validVersion(good.data(),good.size()));CHECK(!install::validVersion(NULL,good.size()));
    for(size_t n=0;n<good.size();++n)CHECK(!install::validVersion(good.data(),n));
    const size_t version_offsets[]={0,2,4,6,8,32,34,40,44,48,52};
    for(unsigned i=0;i<sizeof(version_offsets)/sizeof(version_offsets[0]);++i){bad=good;bad[version_offsets[i]]^=1;CHECK(!install::validVersion(bad.data(),bad.size()));}
    // Version strings after the fixed root do not affect the verified executable version.
    good.resize(128,0x45);store16(good,0,128);CHECK(install::validVersion(good.data(),good.size()));
    uint8_t digest[32];char hex[65];
    const char* messages[]={"","abc","abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"};
    const char* hashes[]={"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"};
    for(unsigned i=0;i<3;++i){install::sha256((const uint8_t*)messages[i],strlen(messages[i]),digest);for(unsigned j=0;j<32;++j)sprintf(hex+j*2,"%02x",digest[j]);CHECK(!strcmp(hex,hashes[i]));}
    std::vector<uint8_t> zeros(316);
    CHECK(!install::validCode(3,zeros.data(),zeros.size()));
    for(unsigned i=0;i<3;++i){CHECK(!install::validCode(i,NULL,install::kLengths[i]));CHECK(!install::validCode(i,zeros.data(),install::kLengths[i]-1));CHECK(!install::validCode(i,zeros.data(),install::kLengths[i]));}
#ifdef C4_BLEND_NATIVE_ORACLE
    CHECK(install::validPe(native_pe_header_bytes,sizeof(native_pe_header_bytes),install::kImageBase,layout));
    CHECK(install::validVersion(native_version_bytes,sizeof(native_version_bytes)));
    const uint8_t* code[]={native_alpha_bytes,native_add_bytes,native_sub_bytes};
    for(unsigned i=0;i<3;++i){
        CHECK(install::validCode(i,code[i],install::kLengths[i]));
        // Mutations away from the prologue must also disable installation.
        const size_t positions[]={0,17,install::kLengths[i]/2,install::kLengths[i]-1};
        for(unsigned j=0;j<4;++j){std::vector<uint8_t> changed(code[i],code[i]+install::kLengths[i]);changed[positions[j]]^=1;CHECK(!install::validCode(i,changed.data(),changed.size()));}
    }
#endif
}
struct FakePatch {
    uint32_t slots[3];bool begin_ok,published,writable;int begin_calls,end_calls,publish_calls,cas_calls;
    int inject_index,end_failures;bool inject_during_preflight,change_owned_before_rollback;
    FakePatch():begin_ok(true),published(false),writable(false),begin_calls(0),end_calls(0),publish_calls(0),cas_calls(0),inject_index(-1),end_failures(0),inject_during_preflight(false),change_owned_before_rollback(false){memcpy(slots,c4blend565install::kEntries,sizeof(slots));}
};
static bool patch_begin(void* opaque,uintptr_t* cookie){FakePatch& f=*(FakePatch*)opaque;++f.begin_calls;if(!f.begin_ok)return false;f.writable=true;*cookie=0x1234;return true;}
static bool patch_end(void* opaque,uintptr_t cookie){FakePatch& f=*(FakePatch*)opaque;++f.end_calls;CHECK(cookie==0x1234);CHECK(f.writable);if(f.end_failures){--f.end_failures;return false;}f.writable=false;return true;}
static uint32_t patch_cas(void* opaque,unsigned index,uint32_t expected,uint32_t desired){
    FakePatch& f=*(FakePatch*)opaque;++f.cas_calls;CHECK(index<3&&f.writable);
    bool publishing=expected==c4blend565install::kEntries[index]&&desired!=expected;
    if(publishing)CHECK(f.published);
    if((int)index==f.inject_index && ((f.inject_during_preflight&&expected==desired)||(!f.inject_during_preflight&&publishing))){
        f.slots[index]=0x00FE0000+index;f.inject_index=-1;
        if(f.change_owned_before_rollback && index>0)f.slots[0]=0x00FB0000;
    }
    uint32_t old=f.slots[index];if(old==expected)f.slots[index]=desired;return old;
}
static void patch_publish(void* opaque){FakePatch& f=*(FakePatch*)opaque;CHECK(f.writable);++f.publish_calls;f.published=true;}
static c4blend565install::PatchOps patch_ops(FakePatch& f){c4blend565install::PatchOps ops={&f,patch_begin,patch_end,patch_cas,patch_publish};return ops;}
static void patch_transactions(){
    namespace install=c4blend565install;const uint32_t hooks[]={0x01001000,0x01002000,0x01003000};
    {FakePatch f;CHECK(install::patchSlots(patch_ops(f),hooks)==install::PatchResult::Installed);CHECK(!memcmp(f.slots,hooks,sizeof(hooks)));CHECK(f.begin_calls==1&&f.end_calls==1&&f.publish_calls==1&&!f.writable);}
    {FakePatch f;f.begin_ok=false;CHECK(install::patchSlots(patch_ops(f),hooks)==install::PatchResult::ProtectFailed);CHECK(!memcmp(f.slots,install::kEntries,sizeof(f.slots)));CHECK(f.cas_calls==0&&f.publish_calls==0&&f.end_calls==0);}
    for(int phase=0;phase<2;++phase)for(int index=0;index<3;++index){
        FakePatch f;f.inject_index=index;f.inject_during_preflight=phase==0;
        CHECK(install::patchSlots(patch_ops(f),hooks)==install::PatchResult::Collision);
        for(int slot=0;slot<3;++slot)CHECK(f.slots[slot]==(slot==index?0x00FE0000u+index:install::kEntries[slot]));
        CHECK(f.end_calls==1&&!f.writable);CHECK(f.publish_calls==(phase?1:0));
    }
    {FakePatch f;f.inject_index=2;f.change_owned_before_rollback=true;CHECK(install::patchSlots(patch_ops(f),hooks)==install::PatchResult::Collision);CHECK(f.slots[0]==0x00FB0000&&f.slots[1]==install::kEntries[1]&&f.slots[2]==0x00FE0002);}
    {FakePatch f;f.end_failures=1;CHECK(install::patchSlots(patch_ops(f),hooks)==install::PatchResult::RestoreFailed);CHECK(!memcmp(f.slots,install::kEntries,sizeof(f.slots)));CHECK(f.end_calls==2&&!f.writable);}
    {FakePatch f;f.end_failures=2;CHECK(install::patchSlots(patch_ops(f),hooks)==install::PatchResult::RestoreFailed);CHECK(!memcmp(f.slots,install::kEntries,sizeof(f.slots)));CHECK(f.end_calls==2&&f.writable);}
    {FakePatch f;f.inject_index=1;f.end_failures=1;CHECK(install::patchSlots(patch_ops(f),hooks)==install::PatchResult::RestoreFailed);CHECK(f.slots[0]==install::kEntries[0]&&f.slots[1]==0x00FE0001&&f.slots[2]==install::kEntries[2]);}
    {FakePatch f;install::PatchOps ops=patch_ops(f);ops.compareExchange=NULL;CHECK(install::patchSlots(ops,hooks)==install::PatchResult::ProtectFailed);CHECK(!f.begin_calls&&!f.cas_calls&&!f.publish_calls);}
    {FakePatch f;const uint32_t invalid[]={hooks[0],0,hooks[2]};CHECK(install::patchSlots(patch_ops(f),invalid)==install::PatchResult::ProtectFailed);CHECK(!f.begin_calls&&!f.cas_calls&&!f.publish_calls);}
}

int main(){
    static_assert(sizeof(Point)==8&&sizeof(Size)==8,"Native ABI layout");
#ifdef C4_BLEND_NATIVE_ORACLE
    native_alpha=(Native9)load_oracle(native_alpha_bytes,sizeof(native_alpha_bytes));native_add=(Native7)load_oracle(native_add_bytes,sizeof(native_add_bytes));native_sub=(Native7)load_oracle(native_sub_bytes,sizeof(native_sub_bytes));
    puts("Oracle: local native machine code plus independent per-channel scalar model.");
#else
    puts("Oracle: independent per-channel scalar model (no game asset or external dependency).");
#endif
    differential();guards();validation();installation_gates();patch_transactions();
    printf("Blend565 checks:%u passed,%u failed;helper handled%u,fallback%u;SSE2%s\n",checks-failures,failures,handled_calls,fallback_calls,cpu_sse2?"yes":"no");
    return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
