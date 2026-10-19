// Standalone GPU regression check; compile explicitly as Objective-C++ (PASSES.md).
// Including the implementation keeps this diagnostic out of the game executable.
#include "passes/assao_pass.mm"
#include <cstdio>
#include <vector>
int main() { @autoreleasepool {
    auto device=MTLCreateSystemDefaultDevice();auto queue=[device newCommandQueue];
    if(!device)return 2;
    auto desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:128 height:128 mipmapped:NO];
    desc.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;desc.storageMode=MTLStorageModeShared;
    auto color=[device newTextureWithDescriptor:desc];
    desc.pixelFormat=MTLPixelFormatR32Float;auto depth=[device newTextureWithDescriptor:desc];
    const float projection[16]={1,0,0,0,0,1,0,0,0,0,1.001f,1,0,0,-1.001f,0};
    std::vector<unsigned char> white(128*128*4,255), output(white.size());
    std::vector<float> depths(128*128);
    for(int quality=-1;quality<=2;++quality)for(int step=0;step<=1;++step) {
        for(int y=0;y<128;++y)for(int x=0;x<128;++x){
            float z=step&&x>45&&x<85&&y>45&&y<85?9.f:10.f;
            depths[y*128+x]=1.001f-1.001f/z;
        }
        [color replaceRegion:MTLRegionMake2D(0,0,128,128) mipmapLevel:0 withBytes:white.data() bytesPerRow:512];
        [depth replaceRegion:MTLRegionMake2D(0,0,128,128) mipmapLevel:0 withBytes:depths.data() bytesPerRow:512];
        Graphics::ExtAoFrame frame{quality,2,4.f,1.f,1.5f,.06f,.98f,.5f,25.f,100.f};
        auto commands=[queue commandBuffer];
        bool encoded=VideoMetal::Assao::Draw(device,commands,color,depth,projection,frame);
        [commands commit];[commands waitUntilCompleted];
        [color getBytes:output.data() bytesPerRow:512 fromRegion:MTLRegionMake2D(0,0,128,128) mipmapLevel:0];
        unsigned dark=0,alphaChanged=0;
        for(size_t i=0;i<output.size();i+=4){if(output[i]<250)++dark;if(output[i+3]!=255)++alphaChanged;}
        std::fprintf(stderr,"ASSAO quality=%d step=%d encoded=%d status=%lu dark=%u alphaChanged=%u error=%s\n",
                     quality,step,encoded,(unsigned long)commands.status,dark,alphaChanged,commands.error.localizedDescription.UTF8String);
        if(!encoded||commands.status!=MTLCommandBufferStatusCompleted||alphaChanged||(step?!dark:dark))return 1;
        if(step){
            char path[96];std::snprintf(path,sizeof(path),"build/mac-passes/assao_q%d.rgba",quality);
            FILE* file=std::fopen(path,"wb");if(!file)return 2;
            std::fwrite(output.data(),1,output.size(),file);std::fclose(file);
        }
    }
    return 0;
} }
