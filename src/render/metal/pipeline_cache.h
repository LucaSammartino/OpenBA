// Included inside VideoMetal: descriptor metadata only; never stores shader/game bytes.
struct PipelineEntry {
 std::mutex mutex;
 id<MTLRenderPipelineState> state=nil;
 bool started=false;
};
static std::unordered_map<std::string,std::shared_ptr<PipelineEntry>> warmPipelines;
static std::recursive_mutex pipelineMetadataMutex;
static NSMutableDictionary* pipelineRecords;
static NSMutableDictionary<NSString*,id<MTLFunction>>* pipelineFunctions;
static NSString* PipelinePath() {
 NSString* home=Opt().userProfile?@(Opt().userProfile):NSHomeDirectory();
 NSString* dir=[home stringByAppendingPathComponent:@"Library/Caches/OpenBA"];
 [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
 return [dir stringByAppendingPathComponent:@"pipelines.txt"];
}
static void LoadPipelineRecords() {
 if(pipelineRecords)return;
 pipelineRecords=[[NSMutableDictionary alloc] initWithContentsOfFile:PipelinePath()];
 if(!pipelineRecords)pipelineRecords=[NSMutableDictionary new];
 for(id key in [pipelineRecords allKeys]){
  id r=pipelineRecords[key];
  if(![key isKindOfClass:[NSString class]]||![r isKindOfClass:[NSDictionary class]]||
     ![r[@"vs"] isKindOfClass:[NSString class]]||![r[@"ps"] isKindOfClass:[NSString class]]||
     ![r[@"attrs"] isKindOfClass:[NSArray class]]||![r[@"layouts"] isKindOfClass:[NSArray class]]||![r[@"blend"] isKindOfClass:[NSArray class]]||
     [r[@"attrs"] count]!=31||[r[@"layouts"] count]!=31||[r[@"blend"] count]!=8)
   [pipelineRecords removeObjectForKey:key];
  else {
   bool valid=true;
   for(NSString* field in @[@"attrs",@"layouts"])
    for(id row in r[field]){if(![row isKindOfClass:[NSArray class]]||[row count]!=3){valid=false;break;}for(id n in row)valid&=[n isKindOfClass:[NSNumber class]];}
   for(id n in r[@"blend"])valid&=[n isKindOfClass:[NSNumber class]];
   for(NSString* field in @[@"color",@"depth",@"stencil",@"samples",@"coverage"])valid&=[r[field] isKindOfClass:[NSNumber class]];
   if(!valid)[pipelineRecords removeObjectForKey:key];
  }
 }
 pipelineFunctions=[NSMutableDictionary new];
}
static NSDictionary* DescribePipeline(MTLRenderPipelineDescriptor* d,NSString* vs,NSString* ps) {
 NSMutableArray* attrs=[NSMutableArray new];NSMutableArray* layouts=[NSMutableArray new];
 for(unsigned i=0;i<31;++i){auto a=d.vertexDescriptor.attributes[i];[attrs addObject:@[@(a.format),@(a.offset),@(a.bufferIndex)]];auto l=d.vertexDescriptor.layouts[i];[layouts addObject:@[@(l.stride),@(l.stepFunction),@(l.stepRate)]];}
 auto c=d.colorAttachments[0];
 return @{@"vs":vs,@"ps":ps,@"attrs":attrs,@"layouts":layouts,@"color":@(c.pixelFormat),@"depth":@(d.depthAttachmentPixelFormat),@"stencil":@(d.stencilAttachmentPixelFormat),@"samples":@(d.rasterSampleCount),@"coverage":@(d.alphaToCoverageEnabled),@"blend":@[@(c.blendingEnabled),@(c.sourceRGBBlendFactor),@(c.destinationRGBBlendFactor),@(c.rgbBlendOperation),@(c.sourceAlphaBlendFactor),@(c.destinationAlphaBlendFactor),@(c.alphaBlendOperation),@(c.writeMask)]};
}
static MTLRenderPipelineDescriptor* RestorePipeline(NSDictionary* r) {
 auto vs=pipelineFunctions[r[@"vs"]];auto ps=pipelineFunctions[r[@"ps"]];
 if(!vs||([r[@"ps"] length]&&!ps))return nil;
 auto d=[MTLRenderPipelineDescriptor new];d.vertexFunction=vs;d.fragmentFunction=ps;d.vertexDescriptor=[MTLVertexDescriptor vertexDescriptor];
 for(unsigned i=0;i<31;++i){NSArray* a=r[@"attrs"][i];auto v=d.vertexDescriptor.attributes[i];v.format=(MTLVertexFormat)[a[0] unsignedIntegerValue];v.offset=[a[1] unsignedIntegerValue];v.bufferIndex=[a[2] unsignedIntegerValue];NSArray* l=r[@"layouts"][i];auto b=d.vertexDescriptor.layouts[i];b.stride=[l[0] unsignedIntegerValue];b.stepFunction=(MTLVertexStepFunction)[l[1] unsignedIntegerValue];b.stepRate=[l[2] unsignedIntegerValue];}
 auto c=d.colorAttachments[0];c.pixelFormat=(MTLPixelFormat)[r[@"color"] unsignedIntegerValue];d.depthAttachmentPixelFormat=(MTLPixelFormat)[r[@"depth"] unsignedIntegerValue];d.stencilAttachmentPixelFormat=(MTLPixelFormat)[r[@"stencil"] unsignedIntegerValue];d.rasterSampleCount=[r[@"samples"] unsignedIntegerValue];d.alphaToCoverageEnabled=[r[@"coverage"] boolValue];
 NSArray* b=r[@"blend"];c.blendingEnabled=[b[0] boolValue];c.sourceRGBBlendFactor=(MTLBlendFactor)[b[1] unsignedIntegerValue];c.destinationRGBBlendFactor=(MTLBlendFactor)[b[2] unsignedIntegerValue];c.rgbBlendOperation=(MTLBlendOperation)[b[3] unsignedIntegerValue];c.sourceAlphaBlendFactor=(MTLBlendFactor)[b[4] unsignedIntegerValue];c.destinationAlphaBlendFactor=(MTLBlendFactor)[b[5] unsignedIntegerValue];c.alphaBlendOperation=(MTLBlendOperation)[b[6] unsignedIntegerValue];c.writeMask=(MTLColorWriteMask)[b[7] unsignedIntegerValue];return d;
}
static void StartPipeline(const std::string& key,MTLRenderPipelineDescriptor* d) {
 auto& entry=warmPipelines[key];if(!entry)entry=std::make_shared<PipelineEntry>();
 if(entry->started)return;entry->started=true;auto retained=entry;
 [d.vertexFunction.device newRenderPipelineStateWithDescriptor:d completionHandler:^(id<MTLRenderPipelineState> state,NSError* error){
  std::lock_guard<std::mutex> guard(retained->mutex);retained->state=state;
  if(error)fprintf(stderr,"[Metal] async pipeline: %s\n",error.localizedDescription.UTF8String);
 }];
}
void RegisterPipelineShader(const Shader& shader) {
 std::lock_guard<std::recursive_mutex> guard(pipelineMetadataMutex);
 if(!gpu||!shader.function||shader.identity.empty())return;
 LoadPipelineRecords();pipelineFunctions[@(shader.identity.c_str())]=shader.function;
 // Replayed only when both functions exist. Completion handlers never touch the maps.
 for(NSString* key in pipelineRecords){
  if(warmPipelines.count(key.UTF8String))continue;
  auto d=RestorePipeline(pipelineRecords[key]);if(d)StartPipeline(key.UTF8String,d);
 }
}
static id<MTLRenderPipelineState> CachedPipeline(MTLRenderPipelineDescriptor* d,const Shader* vs,const Shader* ps,bool required) {
 std::lock_guard<std::recursive_mutex> guard(pipelineMetadataMutex);
 LoadPipelineRecords();auto record=DescribePipeline(d,@(vs->identity.c_str()),ps?@(ps->identity.c_str()):@"");
 // JSON's sorted keys make the descriptor key stable across processes.
 NSData* json=[NSJSONSerialization dataWithJSONObject:record options:NSJSONWritingSortedKeys error:nil];
 NSString* key=[[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding];
 if(!pipelineRecords[key]){pipelineRecords[key]=record;[pipelineRecords writeToFile:PipelinePath() atomically:YES];}
 std::string nativeKey=key.UTF8String;StartPipeline(nativeKey,d);auto entry=warmPipelines[nativeKey];
 {std::lock_guard<std::mutex> guard(entry->mutex);if(entry->state)return entry->state;}
 if(!required)return nil;
 NSError* error=nil;double t=StatNow();auto state=[gpu newRenderPipelineStateWithDescriptor:d error:&error];statPipeline+=StatNow()-t;++statPipelinesNew;
 if(error)fprintf(stderr,"[Metal] pipeline: %s\n",error.localizedDescription.UTF8String);
 {std::lock_guard<std::mutex> guard(entry->mutex);entry->state=state;}return state;
}
