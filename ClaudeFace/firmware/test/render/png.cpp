#include "hostgfx.h"
#include <zlib.h>
#include <vector>
static void be32(std::vector<uint8_t>&v,uint32_t x){for(int i=3;i>=0;i--)v.push_back((uint8_t)(x>>(i*8)));}
static void chunk(FILE*f,const char*t,const std::vector<uint8_t>&d){std::vector<uint8_t>b;be32(b,(uint32_t)d.size());fwrite(b.data(),1,4,f);std::vector<uint8_t>c(t,t+4);c.insert(c.end(),d.begin(),d.end());fwrite(c.data(),1,c.size(),f);uint32_t crc=(uint32_t)crc32(0,c.data(),(uInt)c.size());b.clear();be32(b,crc);fwrite(b.data(),1,4,f);}
void writePNG565(const char*p,const uint16_t*px,int w,int h){FILE*f=fopen(p,"wb");if(!f)return;const uint8_t sig[8]={137,80,78,71,13,10,26,10};fwrite(sig,1,8,f);std::vector<uint8_t>hd;be32(hd,w);be32(hd,h);hd.push_back(8);hd.push_back(2);hd.push_back(0);hd.push_back(0);hd.push_back(0);chunk(f,"IHDR",hd);
std::vector<uint8_t>raw;for(int y=0;y<h;y++){raw.push_back(0);for(int x=0;x<w;x++){uint16_t c=px[(size_t)y*w+x];raw.push_back((uint8_t)(((c>>11)&31)*255/31));raw.push_back((uint8_t)(((c>>5)&63)*255/63));raw.push_back((uint8_t)((c&31)*255/31));}}
uLongf n=compressBound(raw.size());std::vector<uint8_t>z(n);compress(z.data(),&n,raw.data(),raw.size());z.resize(n);chunk(f,"IDAT",z);chunk(f,"IEND",std::vector<uint8_t>());fclose(f);}
void HostGFX::writePNG(const char*p) const { writePNG565(p, fb.data(), fbW, fbH); }
