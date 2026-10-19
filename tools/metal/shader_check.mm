#include "app/options.h"
#include "VideoMetalData.h"
#include <fstream>
#include <iterator>
#include <cstdio>
int main(int argc,char** argv) { OptionsInit(); @autoreleasepool {
 auto device=MTLCreateSystemDefaultDevice();unsigned ok=0,bad=0;
 for(int i=1;i<argc;++i) { @autoreleasepool {
 std::ifstream in(argv[i],std::ios::binary);
 std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in),{}};
 auto shader=VideoMetal::Translate(bytes,device);
 if(shader.function)++ok;else {++bad;fprintf(stderr,"FAIL %s\n",argv[i]);}
 } }
 printf("Native arm64 DXBC conversion + Metal load: %u passed, %u failed\n",ok,bad);
 return bad?1:0;
} }
