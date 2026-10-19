#include "app/options.h"
#include "platform/posix/mac_display_defaults.h"
#include "VideoMetalData.h"
#include "display_geometry.h"
#include "passes/tree_passes.h"
#include "passes/assao_pass.h"
#include "video/Device.h"
#include "platform/common/cursor_image.h"
#include "video/Buffers.h"
#include "video/ImageBuffer.h"
#include "video/ShaderProgram.h"
#include "video/VertexLayout.h"
#include "video/RenderState.h"
#include "video/Context.h"
#include "io/Serializer.h"
#include "system/MemoryAllocator.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <memory>
#include <atomic>
#include <dispatch/dispatch.h>
extern "C" void OpenBAConfigureBackgroundWindow(SDL_Window*);
// Serial single-sample baseline: translated DXBC and native Metal draw encoding.
namespace VideoMetal {
static id<MTLDevice> gpu;
static id<MTLCommandQueue> queue;
static CAMetalLayer* layer;
static SDL_MetalView view;
static id<CAMetalDrawable> drawable;

static id<MTLCommandBuffer> commands;
static id<MTLRenderCommandEncoder> activeEncoder;
static id<MTLTexture> encoderColor,encoderDepth;
static id<MTLRenderPipelineState> activePipeline;
static void EndEncoder(){if(activeEncoder)[activeEncoder endEncoding];activeEncoder=nil;encoderColor=nil;encoderDepth=nil;activePipeline=nil;}
static id<MTLTexture> mainDepth, mainColor, targetColor, targetDepth;
static std::unordered_set<ImageBufferData*> colorAliases,depthAliases;
static id<MTLTexture> ImageTexture(const Video::ImageBuffer* image) {
 return image && image->_data ? static_cast<ImageBufferData*>(image->_data)->texture : nil;
}
// Retained bindings are applied when the draw encoder is created.
struct Bindings {
 id<MTLBuffer> vertex[31] = {}, index = nil, constants[2][31] = {};
 Shader* vs = nullptr; Shader* ps = nullptr;
 bool scissor = false;
 bool vertexDynamic[31]={};
 NSUInteger vertexOffsets[31] = {}, strides[31] = {};
 id<MTLTexture> images[31] = {};
 id<MTLSamplerState> samplers[31] = {};
 MTLVertexDescriptor* layout = nil;
 id<MTLDepthStencilState> depth = nil;
 MTLRenderPipelineColorAttachmentDescriptor* blend = nil;
 MTLCullMode cull = MTLCullModeBack;
 MTLTriangleFillMode fill = MTLTriangleFillModeFill;
 float bias = 0, slopeBias = 0;
 unsigned stencilReference = 0;
};
static Bindings bound;
// Per-frame stall accumulators (ms), written as extra columns of standalone_metal_frames.csv when OPENBA_METAL_STATS=1.
static double statSem=0,statDrawable=0,statPipeline=0,statBuffer=0;static unsigned statPipelinesNew=0;
static inline double StatNow(){return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static dispatch_semaphore_t inFlight;
static uint64_t frameGeneration=0;
static unsigned frameSlot=0;
struct UploadArena { std::vector<id<MTLBuffer>> pages; size_t page=0, cursor=0; };
static UploadArena uploads[3];
// Completion tickets cover encoded, submitted and executing draws. They never
// retain Metal objects; the command buffer itself retains its encoded resources.
using Completion = std::shared_ptr<std::atomic<bool>>;
static Completion currentCompletion;
static id<MTLCommandBuffer> NewCommands() {
 auto cb=[queue commandBuffer];
 auto done=std::make_shared<std::atomic<bool>>(false);currentCompletion=done;
 [cb addCompletedHandler:^(id<MTLCommandBuffer>){done->store(true,std::memory_order_release);}];
 return cb;
}
struct BufferUse { __weak id<MTLBuffer> buffer; Completion completion; };
static std::unordered_map<void*,BufferUse> bufferUses;
static bool BufferBusy(id<MTLBuffer> buffer) {
 auto it=bufferUses.find((__bridge void*)buffer);
 if(it==bufferUses.end())return false;
 auto resource=it->second.buffer;
 return resource==buffer && !it->second.completion->load(std::memory_order_acquire);
}
static void UseBuffer(id<MTLBuffer> buffer) {
 if(buffer)bufferUses[(__bridge void*)buffer]=BufferUse{buffer,currentCompletion};
}
static uint64_t renameCount=0,renameBytes=0,inPlaceWrites=0;
struct BufferAllocation { __weak id<MTLBuffer> buffer; const char* kind; };
static std::vector<BufferAllocation> bufferAllocations;
static id<MTLBuffer> NewBuffer(NSUInteger size,const char* kind) {
 auto b=[gpu newBufferWithLength:size options:MTLResourceStorageModeShared];
 if(Opt().metalStats&&b)bufferAllocations.push_back({b,kind});
 return b;
}
static std::vector<__weak id<MTLTexture>> textureAllocations;
static id<MTLTexture> NewTextureResource(MTLTextureDescriptor* descriptor) {
 auto t=[gpu newTextureWithDescriptor:descriptor];
 if(Opt().metalStats&&t)textureAllocations.push_back(t);
 return t;
}
static void ReportBuffers(){
 size_t textureCount=0,textureBytes=0;
 for(auto it=textureAllocations.begin();it!=textureAllocations.end();){
  auto t=*it;if(!t){it=textureAllocations.erase(it);continue;}
  ++textureCount;textureBytes+=t.allocatedSize;++it;
 }
 fprintf(stderr,"[MetalTextures] count=%zu bytes=%zu\n",textureCount,textureBytes);
 std::unordered_map<std::string,std::pair<size_t,size_t>> kinds;
 for(auto it=bufferAllocations.begin();it!=bufferAllocations.end();){
  auto b=it->buffer;if(!b){it=bufferAllocations.erase(it);continue;}
  auto& k=kinds[it->kind];++k.first;k.second+=b.length;++it;
 }
 for(auto& k:kinds)fprintf(stderr,"[MetalBuffers] kind=%s count=%zu bytes=%zu\n",k.first.c_str(),k.second.first,k.second.second);
}

// Loading runs outside Draw/BeginCommands pools. Metal's transient autoreleased
// references must drain here too, or even released terrain copies survive load.
static void RenameBuffer(id<MTLBuffer> __strong& buffer,bool preserve) { @autoreleasepool {
 if(!BufferBusy(buffer)){++inPlaceWrites;return;}
 ++renameCount;renameBytes+=buffer.length;
 auto previous=buffer;
 double t0=StatNow();buffer=NewBuffer(previous.length,"rename");
 if(preserve)memcpy(buffer.contents,previous.contents,previous.length);statBuffer+=StatNow()-t0;
 for(auto& vertex:bound.vertex)if(vertex==previous)vertex=buffer;
 if(bound.index==previous)bound.index=buffer;
} }
static NSUInteger constantOffsets[2][31]={};
static NSUInteger constantSizes[2][31]={};
static id<MTLBuffer> Upload(const void* src,size_t size,NSUInteger& offset) {
 auto& a=uploads[frameSlot];size_t aligned=(size+255)&~size_t(255);
 if(a.cursor+aligned>8*1024*1024){++a.page;a.cursor=0;}
 if(a.page>=a.pages.size()){double t0=StatNow();a.pages.push_back(NewBuffer(std::max(size_t(8*1024*1024),aligned),"constant-upload"));statBuffer+=StatNow()-t0;}
 auto buffer=a.pages[a.page];offset=a.cursor;memcpy(static_cast<uint8_t*>(buffer.contents)+offset,src,size);a.cursor+=aligned;return buffer;
}
static std::unordered_map<std::string,id<MTLRenderPipelineState>> pipelines;
static id<MTLBuffer> zeroConstants;
struct ExpandedInput {id<MTLBuffer> source=nil, expanded=nil;};
static std::unordered_map<std::string,ExpandedInput> expandedInputs;
static void InvalidateExpanded(id<MTLBuffer> source) {
 for(auto it=expandedInputs.begin();it!=expandedInputs.end();)
  if(it->second.source==source)it=expandedInputs.erase(it);else ++it;
}
static uint64_t totalDraws=0,indexedDraws=0,instancedDraws=0,shadowDraws=0;
template<class T> static void Key(std::string& key,const T& value) {key.append(reinterpret_cast<const char*>(&value),sizeof(value));}
#include "pipeline_cache.h"
static id<MTLRenderPipelineState> Pipeline() {
 if(!bound.vs||!bound.vs->function||(!targetColor&&!targetDepth))return nil;
 std::string key;
 auto vf=(__bridge void*)bound.vs->function;auto pf=(__bridge void*)(bound.ps?bound.ps->function:nil);
 auto layout=(__bridge void*)bound.layout;auto blendKey=(__bridge void*)bound.blend;
 bool a2c=bound.ps&&TreePasses::IsCoverage(bound.ps)&&!(bound.blend&&bound.blend.blendingEnabled);
 Key(key,vf);Key(key,pf);Key(key,layout);Key(key,blendKey);Key(key,a2c);
 for(auto stride:bound.strides)Key(key,stride);
 unsigned samples=targetColor?unsigned(targetColor.sampleCount):unsigned(targetDepth.sampleCount);
 auto colorFormat=targetColor?targetColor.pixelFormat:MTLPixelFormatInvalid;
 auto depthFormat=targetDepth?targetDepth.pixelFormat:MTLPixelFormatInvalid;
 Key(key,colorFormat);Key(key,depthFormat);Key(key,samples);
 auto found=pipelines.find(key);if(found!=pipelines.end())return found->second;
 auto desc=[MTLRenderPipelineDescriptor new];desc.vertexFunction=bound.vs->function;desc.fragmentFunction=bound.ps?bound.ps->function:nil;
 desc.vertexDescriptor=[bound.layout copy];
 for(unsigned i=0;i<23;++i)if(bound.strides[i])desc.vertexDescriptor.layouts[i+8].stride=bound.strides[i];
 for(MTLVertexAttribute* input in bound.vs->function.vertexAttributes){
  auto a=desc.vertexDescriptor.attributes[input.attributeIndex];
  if(input.attributeType==MTLDataTypeInt4 && a.format==MTLVertexFormatUChar4){
   a.format=MTLVertexFormatInt4;a.bufferIndex=7;a.offset=0;desc.vertexDescriptor.layouts[7].stride=16;
  }
 }
 auto blend=bound.blend;
 if(blend)desc.colorAttachments[0]=[blend copy];
 if(a2c&&samples>1){
  // FOLIAGE_AA 6.3: coverage comes from o0.w; the blend writes colour*1 and alpha 0 (retail's mov o0.w,0).
  auto c=desc.colorAttachments[0];c.blendingEnabled=YES;c.sourceRGBBlendFactor=MTLBlendFactorOne;c.destinationRGBBlendFactor=MTLBlendFactorZero;c.rgbBlendOperation=MTLBlendOperationAdd;
  c.sourceAlphaBlendFactor=MTLBlendFactorZero;c.destinationAlphaBlendFactor=MTLBlendFactorZero;c.alphaBlendOperation=MTLBlendOperationAdd;c.writeMask=MTLColorWriteMaskAll;
  desc.alphaToCoverageEnabled=YES;
 }
 desc.colorAttachments[0].pixelFormat=targetColor?targetColor.pixelFormat:MTLPixelFormatInvalid;
 desc.depthAttachmentPixelFormat=targetDepth?targetDepth.pixelFormat:MTLPixelFormatInvalid;
 desc.stencilAttachmentPixelFormat=desc.depthAttachmentPixelFormat;
 desc.rasterSampleCount=samples;
 auto state=CachedPipeline(desc,bound.vs,bound.ps,true);
 pipelines.emplace(key,state);if(state&&pipelines.size()<=15)fprintf(stderr,"[Metal] pipeline %zu created\n",pipelines.size());return state;
}
// DXMT's SM50 tables contain GPU addresses/resource IDs, indexed in qwords by
// SM50GetArgumentsInfo. The public reflection's legacy 29/30 bind indices are
// stale: setup_binding_table2 uses SM50_BINDING_INDEX_* (0 and 1).
static void Resources(id<MTLRenderCommandEncoder> enc,Shader& shader,bool vertex) {
 MTLRenderStages stages=vertex?MTLRenderStageVertex:MTLRenderStageFragment;
 if(!zeroConstants)zeroConstants=NewBuffer(65536,"zero-constant");
 std::vector<uint64_t> cb(std::max(1u,shader.reflection.NumConstantBuffers),0);
 for(auto& a:shader.constants){auto b=a.SM50BindingSlot<31?bound.constants[vertex?1:0][a.SM50BindingSlot]:nil;if(!b)b=zeroConstants;
 if(a.StructurePtrOffset<cb.size())cb[a.StructurePtrOffset]=b.gpuAddress+(b==zeroConstants?0:constantOffsets[vertex?1:0][a.SM50BindingSlot]);[enc useResource:b usage:MTLResourceUsageRead stages:stages];}
 if(shader.reflection.NumConstantBuffers){if(vertex)[enc setVertexBytes:cb.data() length:cb.size()*8 atIndex:SM50_BINDING_INDEX_CONSTANT_BUFFER];else [enc setFragmentBytes:cb.data() length:cb.size()*8 atIndex:SM50_BINDING_INDEX_CONSTANT_BUFFER];}
 std::vector<uint64_t> args(std::max(1u,shader.reflection.ArgumentTableQwords),0);
 for(auto& a:shader.arguments){auto slot=a.SM50BindingSlot;auto off=a.StructurePtrOffset;if(slot>=31||off>=args.size())continue;
 if(a.Type==SM50BindingType::Sampler){auto sampler=bound.samplers[slot];args[off]=sampler.gpuResourceID._impl;if(off+1<args.size())args[off+1]=args[off];}
 else if(a.Type==SM50BindingType::SRV&&(a.Flags&MTL_SM50_SHADER_ARGUMENT_TEXTURE)){auto tex=bound.images[slot];if(tex){args[off]=tex.gpuResourceID._impl;if(off+1<args.size())args[off+1]=uint64_t(tex.arrayLength)<<32;[enc useResource:tex usage:MTLResourceUsageRead stages:stages];}}
 }
 if(shader.reflection.NumArguments){if(vertex)[enc setVertexBytes:args.data() length:args.size()*8 atIndex:SM50_BINDING_INDEX_ARGUMENT_TABLE];else [enc setFragmentBytes:args.data() length:args.size()*8 atIndex:SM50_BINDING_INDEX_ARGUMENT_TABLE];}
}
static void Draw(Video::Context* ctx,int primitive,int count,int instances,bool indexed,int start,int baseVertex) { @autoreleasepool {
 if(!commands||count<=0||instances<=0)return;
 auto targetSamples=targetColor?targetColor.sampleCount:(targetDepth?targetDepth.sampleCount:1);
 TreePasses::BlendGuard treeBlend(bound.ps,bound.blend.blendingEnabled||(targetSamples<=1&&TreePasses::IsCoverage(bound.ps)));
 auto pipeline=Pipeline();if(!pipeline)return;
 if(activeEncoder && (encoderColor!=targetColor || encoderDepth!=targetDepth))EndEncoder();
 if(!activeEncoder){
 auto pass=[MTLRenderPassDescriptor renderPassDescriptor];auto a=pass.colorAttachments[0];a.texture=targetColor;a.loadAction=MTLLoadActionLoad;a.storeAction=MTLStoreActionStore;
 if(targetDepth){pass.depthAttachment.texture=targetDepth;pass.depthAttachment.loadAction=MTLLoadActionLoad;pass.depthAttachment.storeAction=MTLStoreActionStore;pass.stencilAttachment.texture=targetDepth;pass.stencilAttachment.loadAction=MTLLoadActionLoad;pass.stencilAttachment.storeAction=MTLStoreActionStore;}
 activeEncoder=[commands renderCommandEncoderWithDescriptor:pass];encoderColor=targetColor;encoderDepth=targetDepth;
 }
 auto enc=activeEncoder;
 if(activePipeline!=pipeline){[enc setRenderPipelineState:pipeline];activePipeline=pipeline;}
 [enc setDepthStencilState:bound.depth];[enc setCullMode:bound.cull];[enc setFrontFacingWinding:MTLWindingClockwise];[enc setTriangleFillMode:bound.fill];[enc setDepthBias:bound.bias slopeScale:bound.slopeBias clamp:0];[enc setStencilReferenceValue:bound.stencilReference];
 auto vp=ctx->_viewport;[enc setViewport:MTLViewport{double(vp.x),double(vp.y),double(vp.width),double(vp.height),0,1}];
 if(bound.scissor){auto extent=targetColor?targetColor:targetDepth;auto r=ctx->_scissor;NSUInteger x=std::clamp(r.x,0,int(extent.width)),y=std::clamp(r.y,0,int(extent.height));NSUInteger right=std::clamp(r.x+std::max(0,r.width),int(x),int(extent.width)),bottom=std::clamp(r.y+std::max(0,r.height),int(y),int(extent.height));[enc setScissorRect:MTLScissorRect{x,y,right-x,bottom-y}];}
 else {auto extent=targetColor?targetColor:targetDepth;[enc setScissorRect:MTLScissorRect{0,0,extent.width,extent.height}];}
 for(unsigned i=0;i<23;++i)if(bound.vertex[i]){UseBuffer(bound.vertex[i]);[enc setVertexBuffer:bound.vertex[i] offset:bound.vertexOffsets[i] atIndex:i+8];}
 for(MTLVertexAttribute* input in bound.vs->function.vertexAttributes) {
  auto a=bound.layout.attributes[input.attributeIndex];
  if(input.attributeType!=MTLDataTypeInt4 || a.format!=MTLVertexFormatUChar4)continue;
  unsigned slot=unsigned(a.bufferIndex)-8;auto source=bound.vertex[slot];
  size_t stride=bound.strides[slot];if(!source||!stride)continue;
  size_t begin=bound.vertexOffsets[slot], n=(source.length-begin)/stride;
  std::string key;auto address=source.gpuAddress;Key(key,address);Key(key,begin);Key(key,stride);Key(key,a.offset);
  auto cached=expandedInputs.find(key);
  if(!bound.vertexDynamic[slot]&&cached!=expandedInputs.end()){
   [enc setVertexBuffer:cached->second.expanded offset:0 atIndex:7];continue;
  }
  auto expanded=NewBuffer(std::max(size_t(16),n*16),"expanded-vertex");
  auto dst=static_cast<int32_t*>(expanded.contents);auto src=static_cast<const uint8_t*>(source.contents)+begin;
  for(size_t v=0;v<n;++v)for(unsigned k=0;k<4;++k)dst[v*4+k]=src[v*stride+a.offset+k];
  if(!bound.vertexDynamic[slot])expandedInputs.emplace(key,ExpandedInput{source,expanded});
  [enc setVertexBuffer:expanded offset:0 atIndex:7];
 }
 Resources(enc,*bound.vs,true);if(bound.ps)Resources(enc,*bound.ps,false);
 MTLPrimitiveType type=primitive==2?MTLPrimitiveTypeLine:MTLPrimitiveTypeTriangle;
 if(indexed){UseBuffer(bound.index);NSUInteger offset=NSUInteger(ctx->_indexOffset)+NSUInteger(start)*2;
 if(bound.index&&offset+NSUInteger(count)*2<=bound.index.length)[enc drawIndexedPrimitives:type indexCount:count indexType:MTLIndexTypeUInt16 indexBuffer:bound.index indexBufferOffset:offset instanceCount:instances baseVertex:baseVertex baseInstance:0];}
 else [enc drawPrimitives:type vertexStart:start vertexCount:count instanceCount:instances];
 if(!targetColor&&targetDepth)++shadowDraws;
 ++totalDraws;if(indexed)++indexedDraws;if(instances>1)++instancedDraws;
} }
static unsigned clearColor = 0xff202830;
static id<MTLTexture> NewTexture(int w,int h,MTLPixelFormat f,MTLTextureUsage usage) {
 auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:std::max(1,w) height:std::max(1,h) mipmapped:NO];
 d.usage=usage; d.storageMode=MTLStorageModeShared;
 return NewTextureResource(d);
}
static unsigned msaaSamples=1;
static unsigned SupportedSamples(unsigned want) {
 for(unsigned n:{8u,4u,2u})if(n<=want&&[gpu supportsTextureSampleCount:n])return n;
 return 1;
}
// VideoMultisample is an INDEX in the retail option list: sample count = index + 1
// (0 none, 1 = 2x, 3 = 4x, 7 = 8x), exactly as the DX11 backend's ResolveMultisampleMode.
static unsigned DesiredSamples() {
 auto cfg=Video::Configuration::_singleton;int index=cfg?int(cfg->multisample):0;
 return SupportedSamples(unsigned(std::clamp(index,0,31)+1));
}
static id<MTLTexture> NewMainColor(int w,int h,unsigned samples) {
 if(samples<=1)return NewTexture(w,h,MTLPixelFormatBGRA8Unorm,MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead);
 auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:std::max(1,w) height:std::max(1,h) mipmapped:NO];
 d.textureType=MTLTextureType2DMultisample;d.sampleCount=samples;d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
 return NewTextureResource(d);
}
static id<MTLTexture> NewMainDepth(int w,int h,unsigned samples) {
 auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:std::max(1,w) height:std::max(1,h) mipmapped:NO];
 if(samples>1){d.textureType=MTLTextureType2DMultisample;d.sampleCount=samples;}
 d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;d.storageMode=MTLStorageModePrivate;
 return NewTextureResource(d);
}
// The engine's default render buffer owns the main targets; re-point it on a sample-count change
// (Options Apply). In-flight command buffers retain the old textures.
static void RebuildMainTargets(Video::Context* context,unsigned samples,int newW=0,int newH=0) {
 if(!mainColor)return;
 int w=newW>0?newW:int(mainColor.width),h=newH>0?newH:int(mainColor.height);
 EndEncoder();msaaSamples=samples;
 mainColor=NewMainColor(w,h,samples);mainDepth=NewMainDepth(w,h,samples);
 // FrameCopyImageBuffer (water, screenshots, thumbnails) owns another alias;
 // updating just the default RenderBuffer leaves it reading the old frame.
 for(auto image:colorAliases)image->texture=mainColor;
 for(auto image:depthAliases)image->texture=mainDepth;
 if(auto rb=context->_defaultRenderBuffer){
  if(rb->colorBuffer&&rb->colorBuffer->_data)static_cast<ImageBufferData*>(rb->colorBuffer->_data)->texture=mainColor;
  if(rb->depthBuffer&&rb->depthBuffer->_data)static_cast<ImageBufferData*>(rb->depthBuffer->_data)->texture=mainDepth;
  if(newW>0){   // a size change: the buffers' own recorded size must follow (RenderBuffer::Width reads it)
   if(rb->colorBuffer){auto b=const_cast<Video::ImageBuffer*>(rb->colorBuffer);b->_width=w;b->_height=h;}
   if(rb->depthBuffer){auto b=const_cast<Video::ImageBuffer*>(rb->depthBuffer);b->_width=w;b->_height=h;}
  }
 }
 fprintf(stderr,"[Metal] multisample %ux (%dx%d)\n",samples,w,h);
}
void Present() { @autoreleasepool {
 if (!commands) return;
 EndEncoder();
 {double drwT0=StatNow();drawable=[layer nextDrawable];statDrawable+=StatNow()-drwT0;}
 if(!drawable){auto semaphore=inFlight;[commands addCompletedHandler:^(id<MTLCommandBuffer>){dispatch_semaphore_signal(semaphore);}];[commands commit];commands=nil;return;}
 auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
 pass.colorAttachments[0].texture=drawable.texture;
 pass.colorAttachments[0].loadAction=MTLLoadActionClear;
 pass.colorAttachments[0].storeAction=MTLStoreActionStore;
 pass.colorAttachments[0].clearColor=MTLClearColorMake(((clearColor>>16)&255)/255.,((clearColor>>8)&255)/255.,(clearColor&255)/255.,((clearColor>>24)&255)/255.);
 static bool info=false;if(!info){info=true;fprintf(stderr,"[Metal] present color=%lux%lu x%lu drawable=%lux%lu\n",(unsigned long)mainColor.width,(unsigned long)mainColor.height,(unsigned long)mainColor.sampleCount,(unsigned long)drawable.texture.width,(unsigned long)drawable.texture.height);}
 if(mainColor && mainColor.sampleCount>1 && mainColor.width==drawable.texture.width && mainColor.height==drawable.texture.height) {
 auto resolve=[MTLRenderPassDescriptor renderPassDescriptor];auto a=resolve.colorAttachments[0];a.texture=mainColor;a.resolveTexture=drawable.texture;a.loadAction=MTLLoadActionLoad;a.storeAction=MTLStoreActionMultisampleResolve;
 [[commands renderCommandEncoderWithDescriptor:resolve] endEncoding];
 } else if(mainColor && mainColor.width==drawable.texture.width && mainColor.height==drawable.texture.height) {
 auto blit=[commands blitCommandEncoder];
 [blit copyFromTexture:mainColor sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(mainColor.width,mainColor.height,1) toTexture:drawable.texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];[blit endEncoding];
 } else {auto enc=[commands renderCommandEncoderWithDescriptor:pass]; [enc endEncoding];}
 [commands presentDrawable:drawable];
 auto semaphore=inFlight;
 [commands addCompletedHandler:^(id<MTLCommandBuffer> cb){
  if(cb.error)fprintf(stderr,"[Metal] command error: %s\n",cb.error.localizedDescription.UTF8String);
  dispatch_semaphore_signal(semaphore);
 }];
 [commands commit];

 if(commands.error)fprintf(stderr,"[Metal] command error: %s\n",commands.error.localizedDescription.UTF8String);
 static unsigned frames=0;
 if(Opt().metalStats){
  using Clock=std::chrono::steady_clock;static auto first=Clock::now(),last=first;
  auto now=Clock::now();static FILE* timing=std::fopen("standalone_metal_frames.csv","w");
  if(timing){if(!frames)std::fprintf(timing,"frame,seconds,frame_ms,sem_ms,drawable_ms,pipeline_ms,new_pipelines,newbuf_ms\n");
   std::fprintf(timing,"%u,%.6f,%.6f,%.4f,%.4f,%.4f,%u,%.4f\n",frames,std::chrono::duration<double>(now-first).count(),std::chrono::duration<double,std::milli>(now-last).count(),statSem,statDrawable,statPipeline,statPipelinesNew,statBuffer);std::fflush(timing);}
  last=now;statSem=statDrawable=statPipeline=statBuffer=0;statPipelinesNew=0;
 }
 if (++frames==1 || frames%300==0) fprintf(stderr,"[Metal] frame=%u status=%lu pipelines=%zu draws=%llu indexed=%llu instanced=%llu\n",frames,(unsigned long)commands.status,pipelines.size(),(unsigned long long)totalDraws,(unsigned long long)indexedDraws,(unsigned long long)instancedDraws);
 commands=nil; drawable=nil;
} }
bool DeviceLive(){return gpu!=nil;}
unsigned CurrentSamples(){return msaaSamples;}
bool DepthReadable(){return mainDepth!=nil&&(mainDepth.usage&MTLTextureUsageShaderRead);}
extern "C" void OpenBAApplyInitialWindowGeometry(SDL_Window*,int,int,int,int,int,int);
extern "C" void OpenBAQueryScreenForModes(int*,int*,int*,int*);
static int g_lastFullscreen=-1;
bool Enumerate(Video::Device* dev,const char* name) {
 using D=Video::Device; using A=System::MemoryAllocator;
 auto old=dev->interfaceData; int n=dev->interfaceCount;
 auto list=static_cast<D::GraphicsInterface*>(A::MemAlloc(sizeof(D::GraphicsInterface)*(n+1),4));
 for(int i=0;i<n;++i) { new(list+i) D::GraphicsInterface(old[i]); old[i].~GraphicsInterface(); }
 A::MemFree(old); new(list+n) D::GraphicsInterface();
 auto& g=list[n]; g.name.Set(name);
 g.adapterData=static_cast<D::Adapter*>(A::MemAlloc(sizeof(D::Adapter),4));
 new(g.adapterData) D::Adapter(); g.adapterCount=g.adapterCapacity=1;
 auto& a=*g.adapterData; a.name.Set("Apple Metal"); a.dxgiAdapterIndex=a.dxgiOutputIndex=0; a.maxFilterLevel=4;
 a.multisampleData=static_cast<uint8_t*>(A::MemAlloc(8,4)); memset(a.multisampleData,0,8); a.multisampleData[0]=1; a.multisampleCount=a.multisampleCap=8;
 if(auto probe=MTLCreateSystemDefaultDevice())for(unsigned n:{2u,4u,8u})if([probe supportsTextureSampleCount:n])a.multisampleData[n-1]=1;
 D::RefreshRate rate{60,1};
 {int mw=0,mh=0,nw=0,nh=0;OpenBAQueryScreenForModes(&mw,&mh,&nw,&nh);int32_t sizes[32][2];int count=Platform::Window::BuildResolutionList(mw,mh,nw,nh,sizes,32);   // engine PIXELS, filtered to the usable screen
  for(int i=0;i<count;++i){ D::DisplayMode mode{}; mode.width=sizes[i][0];mode.height=sizes[i][1];a.FindOrAddMode(mode,rate); }}
 dev->interfaceData=list;dev->interfaceCount=dev->interfaceCapacity=n+1;
 return true;
}

bool DeviceData::EnumerateInterfaces() {
return Enumerate(_owner,"Metal");
}
bool DeviceData::CreateDevice() {
@autoreleasepool {
 gpu=MTLCreateSystemDefaultDevice(); if(!gpu) return false;
 queue=[gpu newCommandQueue];inFlight=dispatch_semaphore_create(3); auto w=static_cast<SDL_Window*>(_owner->windowHandle);
 if(!w) return false;
 if(Opt().background){_owner->backBufferWidth=640;_owner->backBufferHeight=360;}
 if(Opt().windowWidth>0)_owner->backBufferWidth=Opt().windowWidth;
 if(Opt().windowHeight>0)_owner->backBufferHeight=Opt().windowHeight;
 if(auto*c=Video::Configuration::_singleton; !c->fullscreen && !Opt().background && Opt().windowWidth<=0 && Opt().windowHeight<=0)
  OpenBAApplyInitialWindowGeometry(w,c->windowX,c->windowY,c->windowWidth,c->windowHeight,c->width,c->height);   // docs/macos/INPUT_WINDOW.md "Opening size"
 else if(_owner->backBufferWidth<=1||_owner->backBufferHeight<=1){auto*c=Video::Configuration::_singleton;OpenBAApplyInitialWindowGeometry(w,-1,-1,-1,-1,c?c->width:0,c?c->height:0);}   // saved fullscreen with no usable size opened a 1x1 window
 else SDL_SetWindowSize(w,std::max(1,_owner->backBufferWidth),std::max(1,_owner->backBufferHeight));
 SDL_SyncWindow(w);
 SDL_GetWindowSizeInPixels(w,&_owner->backBufferWidth,&_owner->backBufferHeight);
 if(auto*c=Video::Configuration::_singleton){g_lastFullscreen=c->fullscreen?1:0;
  // The window opened at a pixel size the Options list may not show (first run: VideoWidth is read as POINTS): when it is a listed mode, make the configuration say so, so the dialog and the engine agree.
  if(!c->fullscreen&&!Opt().background)for(int i=0;i<_owner->modeCount;++i){auto m=_owner->modeData+i;if(m->width==_owner->backBufferWidth&&m->height==_owner->backBufferHeight){c->width=m->width;c->height=m->height;c->windowWidth=m->width;c->windowHeight=m->height;_owner->displayModeIndex=i;break;}}}
 MacDisplay::SetPixelDensity(SDL_GetWindowPixelDensity(w));
 view=SDL_Metal_CreateView(w); if(!view) {fprintf(stderr,"[Metal] view failed: %s\n",SDL_GetError());return false;}
 // A small unobtrusive/occluded window lets macOS App Nap + memory compression freeze the whole process for seconds
 // (soak 2026-10-19: RSS fell to 7 MB at 0 % CPU during the 8.6 s "hitch"); a game is user-initiated, latency-critical work.
 static id appActivity=[[NSProcessInfo processInfo] beginActivityWithOptions:(NSActivityUserInitiatedAllowingIdleSystemSleep|NSActivityLatencyCritical) reason:@"OpenBA real-time rendering"];(void)appActivity;
 layer=(__bridge CAMetalLayer*)SDL_Metal_GetLayer(view); layer.device=gpu;layer.pixelFormat=MTLPixelFormatBGRA8Unorm;layer.framebufferOnly=NO;layer.displaySyncEnabled=!Opt().stress;
 SDL_ShowWindow(w); OpenBAConfigureBackgroundWindow(w); _owner->settingsChanged=0;
 fprintf(stderr,"[Metal] device %s created\n",gpu.name.UTF8String);return true;
}
}
void DeviceData::Shutdown() {
EndEncoder();
bound=Bindings{};pipelines.clear();warmPipelines.clear();pipelineRecords=nil;pipelineFunctions=nil;expandedInputs.clear();bufferUses.clear();zeroConstants=nil;
if(inFlight){
 for(int i=0;i<3;++i)dispatch_semaphore_wait(inFlight,DISPATCH_TIME_FOREVER);
 // libdispatch requires the semaphore's original count at destruction.
 for(int i=0;i<3;++i)dispatch_semaphore_signal(inFlight);
 inFlight=nil;
}
for(auto& a:uploads)a=UploadArena{};
commands=nil;drawable=nil; mainDepth=nil;mainColor=nil;targetColor=nil;targetDepth=nil;layer=nil;if(view) SDL_Metal_DestroyView(view);view=nullptr;queue=nil;gpu=nil;
}
void DeviceData::Slot4(bool loading) {  }
float DeviceData::Slot5() { return 0; }
bool DeviceData::Slot6() {
return false;
}
bool DeviceData::Slot7() { return true; }
void DeviceData::SetImageBuffer(Video::ImageBuffer* imageBuffer, int hotX, int hotY) {
 // Retail slot 8 (devicedata.cpp): the game cursor from a 32x32 ImageBuffer, rebuilt only when
 // (image, hotspot) change. Without this the main-menu/game cursor was never set (macOS: invisible).
 static Video::ImageBuffer* shownImage=nullptr; static int shownX=0,shownY=0;
 if(imageBuffer==shownImage&&hotX==shownX&&hotY==shownY)return;
 if(!imageBuffer)return;
 uint32_t texels[32*32];
 _owner->Lock(); bool ok=imageBuffer->CopyFromTexture(texels,32,32); _owner->Unlock();
 if(!ok){fprintf(stderr,"[Metal] cursor image not readable\n");return;}
 Platform::Cursor::SetFromTexels(texels,hotX,hotY);
 shownImage=imageBuffer;shownX=hotX;shownY=hotY;
}
Video::ContextData* DeviceData::CreateContextData(Video::Context* owner) {
return new ContextData(owner);
}
Video::ImageBufferData* DeviceData::CreateImageBufferData(Video::ImageBuffer* owner) {
return new ImageBufferData(owner);
}
Video::ConstantBufferData* DeviceData::CreateConstantBufferData(Video::ConstantBuffer* owner) {
return new ConstantBufferData(owner);
}
Video::IndexBufferData* DeviceData::CreateIndexBufferData(Video::IndexBuffer* owner) {
return new IndexBufferData(owner);
}
Video::VertexBufferData* DeviceData::CreateVertexBufferData(Video::VertexBuffer* owner) {
return new VertexBufferData(owner);
}
Video::VertexLayoutData* DeviceData::CreateVertexLayoutData(Video::VertexLayout* owner) {
return new VertexLayoutData(owner);
}
Video::VertexProgramData* DeviceData::CreateVertexProgramData(Video::VertexProgram* owner) {
return new VertexProgramData(owner);
}
Video::PixelProgramData* DeviceData::CreatePixelProgramData(Video::PixelProgram* owner) {
return new PixelProgramData(owner);
}
Video::RenderStateData* DeviceData::CreateRenderStateData(Video::RenderState* owner) {
return new RenderStateData(owner);
}
Video::VideoProgramData* DeviceData::CreateVideoProgramData(Video::VideoProgram* owner) {
return new VideoProgramData(owner);
}
Video::RenderBufferData* DeviceData::CreateRenderBufferData(Video::RenderBuffer* owner) {
return new RenderBufferData(owner);
}
// Options Apply (resolution / fullscreen) and a user drag-resize raise settingsChanged. The DX11 backend answers with
// Dx11_DeviceData_Resize: Device::AdjustDisplay sizes the real window to the engine's back buffer (Platform::Window units
// are converted to points there), then the buffers are rebuilt. Metal did nothing, so the engine believed in a client area
// the window never had (mouse mapping off). Here: size the window if the engine's size differs from it or fullscreen
// flipped, publish the REAL pixel size as the back buffer, then rebuild the drawable and all dynamic targets (including the shadow map).
static void ApplyEngineSettingsChange(Video::Context* ctx,Video::Device* dev) {
 auto w=static_cast<SDL_Window*>(dev->windowHandle);
 if(!w||(SDL_GetWindowFlags(w)&SDL_WINDOW_MINIMIZED))return;
 auto cfg=Video::Configuration::_singleton;int fs=cfg&&cfg->fullscreen?1:0;
 int cw=0,ch=0;SDL_GetWindowSizeInPixels(w,&cw,&ch);
 bool fsFlip=g_lastFullscreen>=0&&g_lastFullscreen!=fs;g_lastFullscreen=fs;
 if(fsFlip||dev->backBufferWidth!=cw||dev->backBufferHeight!=ch){
  int wantW=dev->backBufferWidth,wantH=dev->backBufferHeight;
  dev->AdjustDisplay();
  SDL_SyncWindow(w);SDL_GetWindowSizeInPixels(w,&cw,&ch);
  dev->backBufferWidth=cw;dev->backBufferHeight=ch;if(cfg&&!fs){cfg->windowWidth=cw;cfg->windowHeight=ch;}
  fprintf(stderr,"[Metal] resize: wanted %dx%d px fullscreen=%d -> window %dx%d px\n",wantW,wantH,fs,cw,ch);
 }
 if(cw>0&&ch>0&&mainColor){
  // Apply also changes shadowScalar without changing the window. Recreate
  // every dynamic image/render buffer so textures and cached extents agree.
  EndEncoder();dev->ReleaseDynamic();mainColor=nil;mainDepth=nil;dev->RecreateDynamic();
  if(!mainColor)mainColor=NewMainColor(cw,ch,DesiredSamples());
  RebuildMainTargets(ctx,unsigned(mainColor.sampleCount),cw,ch);   // repoint the default render buffer, its aliases and the AO depth
  fprintf(stderr,"[Metal] back buffer rebuilt %dx%d\n",cw,ch);
 }
}
bool ContextData::BeginCommands() {
@autoreleasepool {
 if(!queue || !layer) return false;
 auto dev=Video::Device::_singleton;
 if(dev->settingsChanged)ApplyEngineSettingsChange(_owner,dev);
 int w=0,h=0;SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(dev->windowHandle),&w,&h);
 if(w<=0||h<=0)return false;
 {CGSize want=CGSizeMake(w,h);CGSize have=layer.drawableSize;if(have.width!=want.width||have.height!=want.height)layer.drawableSize=want;}dev->settingsChanged=0;
double semT0=StatNow();dispatch_semaphore_wait(inFlight,DISPATCH_TIME_FOREVER);statSem+=StatNow()-semT0;
 for(auto it=bufferUses.begin();it!=bufferUses.end();){
  if(it->second.completion->load(std::memory_order_acquire))it=bufferUses.erase(it);else ++it;
 }
 frameSlot=frameGeneration++%3;
 // Preserve bindings that the engine leaves clean across frames before arena reuse.
 std::vector<uint8_t> saved[2][31];
 for(int stage=0;stage<2;++stage)for(int i=0;i<31;++i)if(bound.constants[stage][i]&&constantSizes[stage][i]){
  auto src=static_cast<const uint8_t*>(bound.constants[stage][i].contents)+constantOffsets[stage][i];
  saved[stage][i].assign(src,src+constantSizes[stage][i]);
 }
 uploads[frameSlot].page=uploads[frameSlot].cursor=0;
 for(int stage=0;stage<2;++stage)for(int i=0;i<31;++i)if(!saved[stage][i].empty())bound.constants[stage][i]=Upload(saved[stage][i].data(),saved[stage][i].size(),constantOffsets[stage][i]);
 if(Opt().metalStats&&frameGeneration%120==0){ReportBuffers();size_t pages=0,bytes=0,expanded=0;for(auto& a:uploads)for(auto b:a.pages){++pages;bytes+=b.length;}for(auto& e:expandedInputs)expanded+=e.second.expanded.length;
 fprintf(stderr,"[MetalMemory] renames=%llu copied_or_discarded_bytes=%llu inplace=%llu tracked=%zu upload_pages=%zu upload_bytes=%zu expanded_count=%zu expanded_bytes=%zu device_bytes=%llu\n",(unsigned long long)renameCount,(unsigned long long)renameBytes,(unsigned long long)inPlaceWrites,bufferUses.size(),pages,bytes,expandedInputs.size(),expanded,(unsigned long long)gpu.currentAllocatedSize);
 }
 if(Opt().metalStats&&frameGeneration%120==0)fprintf(stderr,"[MetalShadow] frame=%llu depth-only draws=%llu\n",(unsigned long long)frameGeneration,(unsigned long long)shadowDraws);
 shadowDraws=0;
 TreePasses::BeginFrame();
// The drawable is acquired lazily in Present(): nextDrawable blocks on the compositor, and acquiring it here stalled the
 // whole frame's CPU encoding behind it (every p99 frame in the 2026-10-19 soak was a 15-20 ms nextDrawable wait).
 drawable=nil;commands=NewCommands();
 if(!commands)dispatch_semaphore_signal(inFlight);
 if(!commands)return false;
 dev->Lock();
 if(unsigned want=DesiredSamples();want!=msaaSamples)RebuildMainTargets(_owner,want);
 _owner->_trianglesSubmitted=_owner->_verticesSubmitted=_owner->_drawsSubmitted=_owner->_objectsSubmitted=0;
 _owner->SetRenderBuffer(_owner->_defaultRenderBuffer);
 Video::Viewport vp{};vp.width=dev->backBufferWidth;vp.height=dev->backBufferHeight;_owner->SetViewport(vp);
 return true;
}
}
void ContextData::EndCommands() {  }
void ContextData::Finish() {
Present(); _owner->_device->Unlock();
}
void ContextData::Clear(unsigned flags, unsigned color, float depth, int stencil) {
if(flags&1) clearColor=color;
 BindRenderBuffer();
 if(!commands)return;
 EndEncoder();
 auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
 if(targetColor){auto a=pass.colorAttachments[0];a.texture=targetColor;a.loadAction=(flags&1)?MTLLoadActionClear:MTLLoadActionLoad;a.storeAction=MTLStoreActionStore;a.clearColor=MTLClearColorMake(((color>>16)&255)/255.,((color>>8)&255)/255.,(color&255)/255.,(color>>24)/255.);}
 if(targetDepth){pass.depthAttachment.texture=targetDepth;pass.depthAttachment.loadAction=(flags&2)?MTLLoadActionClear:MTLLoadActionLoad;pass.depthAttachment.storeAction=MTLStoreActionStore;pass.depthAttachment.clearDepth=depth;
 pass.stencilAttachment.texture=targetDepth;pass.stencilAttachment.loadAction=(flags&4)?MTLLoadActionClear:MTLLoadActionLoad;pass.stencilAttachment.storeAction=MTLStoreActionStore;pass.stencilAttachment.clearStencil=stencil;}
 if(targetColor||targetDepth){auto enc=[commands renderCommandEncoderWithDescriptor:pass];[enc endEncoding];}
}
void ContextData::BindRenderBuffer() {
 if(auto rb=_owner->_renderBuffer){targetColor=ImageTexture(rb->colorBuffer);targetDepth=ImageTexture(rb->depthBuffer);}
}
// Viewport and scissor are read from the owner when each encoder is created.
void ContextData::BindViewport() {}
void ContextData::BindScissor() {}
void ContextData::BindTransforms() {
 auto c=_owner;if(c->_dirty&0x4000)c->_transformConstants.Bind(*c,c->_transforms,0,c->_transformCount<<6);
 if(c->_unknown0024>0)c->_boneConstants.Bind(*c,c->_boneAndProjectionStaging,0,0x0cc0);
}
void ContextData::BindTextureNone(int slot) { if(slot>=0&&slot<31){bound.images[slot]=nil;bound.samplers[slot]=nil;} }
void ContextData::BindIndexNone() { bound.index=nil; }
void ContextData::BindVertexNone(int slot) { if(slot>=0&&slot<31)bound.vertex[slot]=nil; }
void ContextData::DrawIndexed(int primitiveType, int baseVertex, int vertexCount, int startIndex, int indexCount, int instanceCount) {
 _owner->BindResources();Draw(_owner,primitiveType,indexCount,instanceCount,true,startIndex,baseVertex);
 _owner->_trianglesSubmitted+=(indexCount/3)*instanceCount;_owner->_verticesSubmitted+=vertexCount*instanceCount;_owner->_drawsSubmitted++;_owner->_objectsSubmitted+=instanceCount;
}
int ContextData::GetMaxDynamicInstances(int a, int b, int c) {
return 52;
}
bool ContextData::BeginVertices(int vertexCount, int vertexStride) {
 if(vertexCount<0||vertexStride<=0)return false;
 auto c=_owner;c->_dynamicVertexStride=vertexStride;c->_unknown2104=vertexCount;
 c->_dynamicVertexCursor=static_cast<uint8_t*>(c->_dynamicVertexBuffer->Lock(vertexCount*vertexStride,c->_unknown2100));c->_unknown2108=c->_dynamicVertexCursor;
 return c->_dynamicVertexCursor!=nullptr;
}
void ContextData::EndVertices(int primitiveType) {
 auto c=_owner;c->_dynamicVertexCursor=nullptr;c->_dynamicVertexBuffer->Unlock();
 c->SetVertexBuffer(c->_dynamicVertexBuffer,0,c->_unknown2100,c->_dynamicVertexStride);c->BindResources();
 Draw(c,primitiveType,c->_unknown2104,1,false,0,0);
 c->_trianglesSubmitted+=c->_unknown2104/3;c->_verticesSubmitted+=c->_unknown2104;c->_drawsSubmitted++;c->_objectsSubmitted++;
}
bool ContextData::BeginIndexedVertices(int vertexCount, int vertexStride, int indexCount) {
return BeginInstancedIndexedVertices(vertexCount,vertexStride,indexCount,1);
}
void ContextData::EndIndexedVertices(int primitiveType) {EndInstancedIndexedVertices(primitiveType);}
bool ContextData::BeginInstancedIndexedVertices(int vertexCount, int vertexStride, int indexCount, int instanceCount) {
 if(!BeginVertices(vertexCount,vertexStride)||indexCount<0)return false;
 auto c=_owner;c->_instanceCount=instanceCount;c->_unknown2124=indexCount;
 c->_dynamicIndexCursor=static_cast<uint16_t*>(c->_dynamicIndexBuffer->Lock(indexCount*2,c->_unknown2120));c->_unknown2128=c->_dynamicIndexCursor;return c->_dynamicIndexCursor!=nullptr;
}
void ContextData::EndInstancedIndexedVertices(int primitiveType) {
 auto c=_owner;c->_dynamicIndexCursor=nullptr;c->_dynamicIndexBuffer->Unlock();
 c->_dynamicVertexCursor=nullptr;c->_dynamicVertexBuffer->Unlock();
 c->SetVertexBuffer(c->_dynamicVertexBuffer,0,c->_unknown2100,c->_dynamicVertexStride);
 if(c->_indexBuffer!=c->_dynamicIndexBuffer||c->_indexOffset!=c->_unknown2120){c->_dirty|=0x10;c->_indexBuffer=c->_dynamicIndexBuffer;c->_indexOffset=c->_unknown2120;}
 DrawIndexed(primitiveType,0,c->_unknown2104,0,c->_unknown2124,c->_instanceCount);
}
ImageBufferData::~ImageBufferData(){colorAliases.erase(this);depthAliases.erase(this);}
void ImageBufferData::AttachColorBuffer() {
 auto dev=Video::Device::_singleton;_owner->_width=dev->backBufferWidth;_owner->_height=dev->backBufferHeight;_owner->_dimension=0;_owner->_kind=0;_owner->_format=5;_owner->_flags=0x10;
if(!mainColor){msaaSamples=DesiredSamples();mainColor=NewMainColor(_owner->Width(),_owner->Height(),msaaSamples);}
 colorAliases.insert(this);texture=mainColor;
}
void ImageBufferData::AttachDepthBuffer() {
 if(_owner->Width()<=0||_owner->Height()<=0){auto dev=Video::Device::_singleton;_owner->_width=dev->backBufferWidth;_owner->_height=dev->backBufferHeight;}
 _owner->_dimension=0;_owner->_kind=1;_owner->_format=7;
if(!mainDepth){msaaSamples=mainColor?unsigned(mainColor.sampleCount):DesiredSamples();mainDepth=NewMainDepth(_owner->Width(),_owner->Height(),msaaSamples);}depthAliases.insert(this);texture=mainDepth;
}
bool ImageBufferData::Create(const Collection::Array<Collection::Array<uint8_t> >* levels) {
@autoreleasepool {
 int w=_owner->Width(),h=_owner->Height();if(w<=0||h<=0)return true;
 static const MTLPixelFormat formats[]={MTLPixelFormatBC1_RGBA,MTLPixelFormatBC3_RGBA,MTLPixelFormatBC3_RGBA,MTLPixelFormatA8Unorm,MTLPixelFormatR8Unorm,MTLPixelFormatBGRA8Unorm,MTLPixelFormatR32Float,MTLPixelFormatDepth32Float_Stencil8};
 int f=int(_owner->_format);if(f<0||f>=8)return false;
 if(f==7){auto depth=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:w height:h mipmapped:NO];depth.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;depth.storageMode=MTLStorageModePrivate;texture=NewTextureResource(depth);}
 else {
 auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:formats[f] width:w height:h mipmapped:(_owner->_flags&1)!=0];
 if(_owner->_dimension!=0)d.textureType=MTLTextureTypeCube;
 d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead;
 if(f>=3&&int(_owner->_kind)!=2)d.usage|=MTLTextureUsageRenderTarget;
 texture=NewTextureResource(d);if(!texture)return false;
 const int bytes[]={8,16,16,1,1,4,4,4};
 if(levels)for(int i=0;i<levels->GetSize();++i){unsigned mip=i%d.mipmapLevelCount,face=i/d.mipmapLevelCount;if(face>=(_owner->_dimension!=0?6u:1u))break;
 int mw=std::max(1,w>>mip),mh=std::max(1,h>>mip);size_t row=(f<3?std::max(1,(mw+3)/4):mw)*bytes[f];size_t total=row*(f<3?std::max(1,(mh+3)/4):mh);
 auto& b=(*levels)[i];if(size_t(b.GetSize())<total)return false;
 [texture replaceRegion:MTLRegionMake2D(0,0,mw,mh) mipmapLevel:mip slice:face withBytes:b.GetData() bytesPerRow:row bytesPerImage:total];}
 }
 if(Opt().metalStats&&(_owner->_flags&0x40))fprintf(stderr,"[MetalShadow] create %dx%d flags=%x samples=%lu format=%lu\n",w,h,_owner->_flags,(unsigned long)texture.sampleCount,(unsigned long)texture.pixelFormat);
 auto sd=[MTLSamplerDescriptor new];sd.supportArgumentBuffers=YES;sd.compareFunction=(_owner->_flags&0x40)?MTLCompareFunctionLess:MTLCompareFunctionNever;sd.lodMaxClamp=(_owner->_flags&1)?float(texture.mipmapLevelCount-1):0;sd.minFilter=sd.magFilter=int(_owner->_filter)==0?MTLSamplerMinMagFilterNearest:MTLSamplerMinMagFilterLinear;sd.mipFilter=int(_owner->_filter)==2?MTLSamplerMipFilterLinear:MTLSamplerMipFilterNearest;
 // D3D11 table (imagebufferdata.cpp CreateSamplerState): _filter 2 with textureFilter>0 is ANISOTROPIC (capped at 16).
 {auto cfg=Video::Configuration::_singleton;int aniso=cfg?int(cfg->textureFilter):0;if(!(_owner->_flags&0x40)&&int(_owner->_filter)==2&&aniso>0)sd.maxAnisotropy=std::clamp(aniso,1,16);}
 const MTLSamplerAddressMode modes[]={MTLSamplerAddressModeRepeat,MTLSamplerAddressModeClampToEdge,MTLSamplerAddressModeClampToEdge,MTLSamplerAddressModeClampToBorderColor};
 sd.sAddressMode=modes[std::clamp(int(_owner->_addressU),0,3)];sd.tAddressMode=modes[std::clamp(int(_owner->_addressV),0,3)];sd.rAddressMode=modes[std::clamp(int(_owner->_addressW),0,3)];sampler=[gpu newSamplerStateWithDescriptor:sd];return true;
}
}
void ImageBufferData::Bind(void* context, unsigned slot) { if(slot<31){bound.images[slot]=texture;bound.samplers[slot]=sampler;} }
bool ImageBufferData::Update(void* data, int width, int height) {
if(!texture||!data||width<=0||height<=0||width>int(texture.width)||height>int(texture.height))return false;
 int f=int(_owner->_format);if(f<3||f==7)return false;size_t row=width*(f==3||f==4?1:4);
 [texture replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0 withBytes:data bytesPerRow:row];return true;
}
bool ImageBufferData::Read(void* dst, int width, int height) {
if(!texture||!dst||width<=0||height<=0||width>int(texture.width)||height>int(texture.height))return false;
 int f=int(_owner->_format);if(f<3||f==7)return false;size_t row=width*(f==3||f==4?1:4);
 // Read uses the same command stream as rendering/FrameCopy Resolve. Commit
 // this segment and resume encoding without consuming another in-flight slot.
 EndEncoder();auto cb=commands?commands:[queue commandBuffer];auto source=texture;
 if(source.sampleCount>1){
  auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:source.pixelFormat width:source.width height:source.height mipmapped:NO];
  d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget;
  auto resolved=NewTextureResource(d);if(!resolved)return false;
  auto pass=[MTLRenderPassDescriptor renderPassDescriptor];auto a=pass.colorAttachments[0];a.texture=source;a.resolveTexture=resolved;a.loadAction=MTLLoadActionLoad;a.storeAction=MTLStoreActionStoreAndMultisampleResolve;
  [[cb renderCommandEncoderWithDescriptor:pass] endEncoding];source=resolved;
 }
 size_t pitch=(row+255)&~size_t(255);auto staging=NewBuffer(pitch*height,"readback");
 if(!staging)return false;
 auto blit=[cb blitCommandEncoder];
 [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width,height,1) toBuffer:staging destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:pitch*height];[blit endEncoding];
 bool resume=commands!=nil;[cb commit];[cb waitUntilCompleted];if(resume)commands=NewCommands();
 if(cb.error){fprintf(stderr,"[Metal] readback: %s\n",cb.error.localizedDescription.UTF8String);return false;}
 // Format 5 is already BGRA8 (also DX11's staging format). Preserve encoded
 // sRGB bytes; the engine's BMP writer consumes packed 0xAARRGGBB directly.
 for(int y=0;y<height;++y)memcpy(static_cast<uint8_t*>(dst)+y*row,static_cast<const uint8_t*>(staging.contents)+y*pitch,row);
 return true;
}
bool IndexBufferData::Create(uint32_t sizeBytes, uint8_t dynamic, const void* data) {
_owner->size=sizeBytes; _owner->dynamic=dynamic;
 buffer=NewBuffer(std::max(1u,sizeBytes),"index");cursor=0;
 if(data && sizeBytes && buffer)memcpy(buffer.contents,data,sizeBytes);return buffer!=nil;
}
bool IndexBufferData::SetData(int32_t sizeBytes, int32_t offsetBytes, const void* src) {
if(sizeBytes<0||offsetBytes<0||!buffer||size_t(offsetBytes)+sizeBytes>buffer.length||(!src&&sizeBytes))return false;
 if(sizeBytes){InvalidateExpanded(buffer);RenameBuffer(buffer,true);memcpy(static_cast<uint8_t*>(buffer.contents)+offsetBytes,src,sizeBytes);}return true;
}
void* IndexBufferData::Lock(int32_t sizeBytes, int32_t* outByteOffset) {
if(!buffer||sizeBytes<0||size_t(sizeBytes)>buffer.length)return nullptr;
 // Rewind on the first write of a frame or wrap (DISCARD); rename only
 // while encoded/in-flight draws read the allocation. Appends are NO_OVERWRITE.
 if(generation!=frameGeneration || cursor+sizeBytes>buffer.length){
  InvalidateExpanded(buffer);RenameBuffer(buffer,false);cursor=0;generation=frameGeneration;
 }
 if(outByteOffset)*outByteOffset=int(cursor);auto ptr=static_cast<uint8_t*>(buffer.contents)+cursor;cursor+=sizeBytes;return ptr;
}
void IndexBufferData::Unlock() {  }
void IndexBufferData::Bind(void* context) { bound.index=buffer; }
void IndexBufferData::Serialize(IO::Serializer* serializer) {
serializer->Serialize(_owner->dynamic,nullptr);serializer->Serialize(_owner->size,nullptr);
 if(static_cast<int>(serializer->GetDirection())==0) Create(_owner->size,_owner->dynamic,nullptr);
 if(buffer)serializer->SerializeBytes(_owner->size,buffer.contents,nullptr);
}
VertexBufferData::~VertexBufferData() { InvalidateExpanded(buffer); }
bool VertexBufferData::Create(uint32_t sizeBytes, uint8_t dynamic, const void* data) {
InvalidateExpanded(buffer);
_owner->size=sizeBytes; _owner->dynamic=dynamic;
 buffer=NewBuffer(std::max(1u,sizeBytes),"vertex");cursor=0;
 if(data && sizeBytes && buffer)memcpy(buffer.contents,data,sizeBytes);return buffer!=nil;
}
bool VertexBufferData::SetData(int32_t sizeBytes, int32_t offsetBytes, const void* src) {
if(sizeBytes<0||offsetBytes<0||!buffer||size_t(offsetBytes)+sizeBytes>buffer.length||(!src&&sizeBytes))return false;
 if(sizeBytes){InvalidateExpanded(buffer);RenameBuffer(buffer,true);memcpy(static_cast<uint8_t*>(buffer.contents)+offsetBytes,src,sizeBytes);}return true;
}
void* VertexBufferData::Lock(int32_t sizeBytes, int32_t* outByteOffset) {
if(!buffer||sizeBytes<0||size_t(sizeBytes)>buffer.length)return nullptr;
 // Rewind on the first write of a frame or wrap (DISCARD); rename only
 // while encoded/in-flight draws read the allocation. Appends are NO_OVERWRITE.
 if(generation!=frameGeneration || cursor+sizeBytes>buffer.length){
  InvalidateExpanded(buffer);RenameBuffer(buffer,false);cursor=0;generation=frameGeneration;
 }
 if(outByteOffset)*outByteOffset=int(cursor);auto ptr=static_cast<uint8_t*>(buffer.contents)+cursor;cursor+=sizeBytes;return ptr;
}
void VertexBufferData::Unlock() {  }
void VertexBufferData::Bind(void* context, uint32_t startSlot, uint32_t offsetBytes, uint32_t strideBytes) {
 if(startSlot<31){bound.vertex[startSlot]=buffer;bound.vertexDynamic[startSlot]=_owner->dynamic!=0;bound.vertexOffsets[startSlot]=offsetBytes;bound.strides[startSlot]=strideBytes;}
}
void VertexBufferData::Serialize(IO::Serializer* serializer) {
serializer->Serialize(_owner->dynamic,nullptr);serializer->Serialize(_owner->size,nullptr);
 if(static_cast<int>(serializer->GetDirection())==0) Create(_owner->size,_owner->dynamic,nullptr);
 if(buffer)serializer->SerializeBytes(_owner->size,buffer.contents,nullptr);
}
bool ConstantBufferData::Create() {
int offset=0;for(int i=0;i<_owner->_constantCount;++i){auto& e=_owner->_constantData[i];e.offset=offset;offset+=e.size;}
 buffer=NewBuffer(std::max(16,_owner->_registerCount*16),"constant");return !gpu||buffer!=nil;
}
void ConstantBufferData::Bind(void* context, const void* srcData, int32_t first, int32_t sizeBytes) {
if(sizeBytes<=0||!srcData)return;
 int stage=_owner->_bindToVertex?1:0,slot=_owner->_uploadIndex;
 if(slot<0||slot>=31)return;
 size_t size=std::max(sizeBytes,_owner->_registerCount*16);
 std::vector<uint8_t> bytes(size,0);memcpy(bytes.data(),srcData,sizeBytes);
 buffer=Upload(bytes.data(),size,constantOffsets[stage][slot]);
 constantSizes[stage][slot]=size;bound.constants[stage][slot]=buffer;
}
int32_t ConstantBufferData::SlotIndexPlusOne() {
return _owner->_uploadIndex+1;
}
bool RenderBufferData::Slot1() { return true; }
void RenderBufferData::Bind(void* context) {targetColor=ImageTexture(_owner->colorBuffer);targetDepth=ImageTexture(_owner->depthBuffer);}
void RenderBufferData::Resolve(void* context, Video::RenderBuffer* other, bool flag) {
 auto dst=ImageTexture(_owner->colorBuffer);auto src=other?ImageTexture(other->colorBuffer):nil;
 if(!src||!dst||src==dst||src.pixelFormat!=dst.pixelFormat||src.width!=dst.width||src.height!=dst.height)return;
 EndEncoder();
 if(src.sampleCount>1){
  if(dst.sampleCount!=1||!(dst.usage&MTLTextureUsageRenderTarget))return;
  auto cb=commands ? commands : [queue commandBuffer];
  auto pass=[MTLRenderPassDescriptor renderPassDescriptor];auto a=pass.colorAttachments[0];a.texture=src;a.resolveTexture=dst;a.loadAction=MTLLoadActionLoad;a.storeAction=MTLStoreActionStoreAndMultisampleResolve;
  [[cb renderCommandEncoderWithDescriptor:pass] endEncoding];if(!commands){[cb commit];[cb waitUntilCompleted];}
  return;
 }
 auto cb=commands ? commands : [queue commandBuffer];auto blit=[cb blitCommandEncoder];
 [blit copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(src.width,src.height,1) toTexture:dst destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];[blit endEncoding];if(!commands){[cb commit];[cb waitUntilCompleted];}
}
void VertexLayoutData::Bind(void* context) { bound.layout=descriptor; }
void VertexLayoutData::PlatformCreate() {
descriptor=[MTLVertexDescriptor vertexDescriptor];
 const MTLVertexFormat formats[]={MTLVertexFormatFloat4,MTLVertexFormatFloat3,MTLVertexFormatFloat2,MTLVertexFormatFloat,MTLVertexFormatShort4,MTLVertexFormatShort2,MTLVertexFormatUChar4Normalized_BGRA,MTLVertexFormatUChar4,MTLVertexFormatUChar4Normalized};
 const size_t sizes[]={16,12,8,4,8,4,4,4,4};
 for(int i=0;i<std::min(31,_owner->_elements.GetSize());++i){auto& e=_owner->_elements[i];if(e.format<0||e.format>=9||e.stream<0||e.stream>=23)continue;
 auto a=descriptor.attributes[i];a.format=formats[e.format];a.bufferIndex=e.stream+8;a.offset=e.offset;
 auto l=descriptor.layouts[e.stream+8];l.stride=std::max(l.stride,NSUInteger(e.offset+sizes[e.format]));l.stepFunction=MTLVertexStepFunctionPerVertex;l.stepRate=1;
 }
}
bool VertexProgramData::PlatformCreate() {
 bytecode.clear();shaders.clear();bool ok=true;
 for(int i=0;i<_owner->_byteCode.GetSize();++i){auto& b=_owner->_byteCode[i];bytecode.emplace_back(b.GetData(),b.GetData()+b.GetSize());shaders.push_back(Translate(bytecode.back(),gpu));ok&=shaders.back().function!=nil;}return ok;
}
void VertexProgramData::Bind(void* context, int permutation) {
 bound.vs=permutation>=0&&size_t(permutation)<shaders.size()?TreePasses::Select(&shaders[permutation],bytecode[permutation],gpu,true):nullptr;
}
bool PixelProgramData::PlatformCreate() {
 bytecode.clear();shaders.clear();bool ok=true;
 for(int i=0;i<_owner->_byteCode.GetSize();++i){auto& b=_owner->_byteCode[i];bytecode.emplace_back(b.GetData(),b.GetData()+b.GetSize());shaders.push_back(Translate(bytecode.back(),gpu));ok&=shaders.back().function!=nil;}return ok;
}
void PixelProgramData::Bind(void* context, int permutation) {
 bound.ps=permutation>=0&&size_t(permutation)<shaders.size()?TreePasses::Select(&shaders[permutation],bytecode[permutation],gpu,false):nullptr;
}
bool RenderStateData::Create() {
auto rs=_owner;const MTLCompareFunction cmp[]={MTLCompareFunctionNever,MTLCompareFunctionAlways,MTLCompareFunctionEqual,MTLCompareFunctionLess,MTLCompareFunctionLessEqual,MTLCompareFunctionGreater,MTLCompareFunctionGreaterEqual,MTLCompareFunctionNotEqual};
 auto d=[MTLDepthStencilDescriptor new];d.depthCompareFunction=rs->_depth.test?cmp[std::clamp<int>(rs->_depth.func,0,7)]:MTLCompareFunctionAlways;d.depthWriteEnabled=rs->_depth.write;
 if(rs->_stencil.enabled){const MTLStencilOperation ops[]={MTLStencilOperationKeep,MTLStencilOperationZero,MTLStencilOperationIncrementWrap,MTLStencilOperationDecrementWrap,MTLStencilOperationIncrementClamp,MTLStencilOperationDecrementClamp,MTLStencilOperationReplace,MTLStencilOperationInvert};
 auto st=[MTLStencilDescriptor new];st.stencilCompareFunction=cmp[std::clamp<int>(rs->_stencil.func,0,7)];st.stencilFailureOperation=ops[std::clamp<int>(rs->_stencil.failOp,0,7)];st.depthFailureOperation=ops[std::clamp<int>(rs->_stencil.depthFailOp,0,7)];st.depthStencilPassOperation=ops[std::clamp<int>(rs->_stencil.passOp,0,7)];d.frontFaceStencil=st;d.backFaceStencil=[st copy];
 if(rs->_stencil.twoSided){d.backFaceStencil.stencilFailureOperation=ops[std::clamp<int>(rs->_stencil.backFailOp,0,7)];d.backFaceStencil.depthFailureOperation=ops[std::clamp<int>(rs->_stencil.backDepthFailOp,0,7)];d.backFaceStencil.depthStencilPassOperation=ops[std::clamp<int>(rs->_stencil.backPassOp,0,7)];}}
 depthState=[gpu newDepthStencilStateWithDescriptor:d];blend=[MTLRenderPipelineColorAttachmentDescriptor new];blend.pixelFormat=MTLPixelFormatBGRA8Unorm;blend.blendingEnabled=rs->_alphaBlend.enabled;
 const MTLBlendFactor factors[]={MTLBlendFactorZero,MTLBlendFactorOne,MTLBlendFactorSourceColor,MTLBlendFactorOneMinusSourceColor,MTLBlendFactorSourceAlpha,MTLBlendFactorOneMinusSourceAlpha,MTLBlendFactorDestinationColor,MTLBlendFactorOneMinusDestinationColor,MTLBlendFactorDestinationAlpha,MTLBlendFactorOneMinusDestinationAlpha};
 blend.sourceRGBBlendFactor=blend.sourceAlphaBlendFactor=factors[std::clamp<int>(rs->_alphaBlend.srcBlend,0,9)];blend.destinationRGBBlendFactor=blend.destinationAlphaBlendFactor=factors[std::clamp<int>(rs->_alphaBlend.dstBlend,0,9)];blend.rgbBlendOperation=blend.alphaBlendOperation=MTLBlendOperation(std::clamp<int>(rs->_alphaBlend.blendOp,0,2));
 unsigned mask=rs->_alphaBlend.writeMask;blend.writeMask=MTLColorWriteMask(((mask&1)?MTLColorWriteMaskRed:0)|((mask&2)?MTLColorWriteMaskGreen:0)|((mask&4)?MTLColorWriteMaskBlue:0)|((mask&8)?MTLColorWriteMaskAlpha:0));return depthState!=nil;
}
void RenderStateData::Bind(void* context) { bound.depth=depthState;bound.blend=blend;bound.scissor=_owner->_flag70!=0;
 const MTLCullMode modes[]={MTLCullModeFront,MTLCullModeBack,MTLCullModeNone};bound.cull=modes[std::clamp<int>(_owner->_cullMode,0,2)];
 bound.fill=_owner->_fillMode?MTLTriangleFillModeLines:MTLTriangleFillModeFill;// Match DX11 RenderStateData: engine bias is normalized to D24 units,
 // and slope bias is stored in 1/4096 units. Metal also takes rasterizer units.
 bound.bias=float(int(_owner->_depth.bias*16777215.0f));bound.slopeBias=_owner->_depth.slopeBias*4096.0f;bound.stencilReference=_owner->_stencil.reference; }
bool VideoProgramData::Slot1() { return true; }
void VideoProgramData::Bind(void* context, int detail) {  }
void Assao::AfterOpaque(Video::Context& context,const Graphics::ExtAoFrame& frame,
                        const Graphics::RendererSceneEntry& entry) {
 EndEncoder();
 Assao::EncodeAfterOpaque(gpu,commands,context,frame,entry);
}
}
