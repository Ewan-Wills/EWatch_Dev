#include "hostgfx.h"
#include <zlib.h>
#include <vector>
static void be32(std::vector<uint8_t>&v,uint32_t x){for(int i=3;i>=0;i--)v.push_back(x>>(i*8));}
static void chunk(FILE*f,const char*t,const std::vector<uint8_t>&d){std::vector<uint8_t>b;be32(b,d.size());fwrite(b.data(),1,4,f);std::vector<uint8_t>c(t,t+4);c.insert(c.end(),d.begin(),d.end());fwrite(c.data(),1,c.size(),f);uint32_t crc=crc32(0,c.data(),c.size());b.clear();be32(b,crc);fwrite(b.data(),1,4,f);}
void HostGFX::writePNG(const char*p){FILE*f=fopen(p,"wb");const uint8_t sig[8]={137,80,78,71,13,10,26,10};fwrite(sig,1,8,f);std::vector<uint8_t>h;be32(h,240);be32(h,280);h.push_back(8);h.push_back(2);h.push_back(0);h.push_back(0);h.push_back(0);chunk(f,"IHDR",h);
std::vector<uint8_t>raw;for(int y=0;y<280;y++){raw.push_back(0);for(int x=0;x<240;x++){uint16_t c=fb[y*240+x];raw.push_back(((c>>11)&31)*255/31);raw.push_back(((c>>5)&63)*255/63);raw.push_back((c&31)*255/31);}}
uLongf n=compressBound(raw.size());std::vector<uint8_t>z(n);compress(z.data(),&n,raw.data(),raw.size());z.resize(n);chunk(f,"IDAT",z);chunk(f,"IEND",{});fclose(f);}
