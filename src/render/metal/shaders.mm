#include "app/options.h"
#include "VideoMetalData.h"
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <mutex>
namespace VideoMetal {
struct Airconv {
 void* handle=nullptr;
#define FN(name) decltype(&name) name=nullptr
 FN(SM50Initialize);FN(SM50Destroy);FN(SM50Compile);FN(SM50GetArgumentsInfo);
 FN(SM50GetCompiledBitcode);FN(SM50DestroyBitcode);FN(SM50GetErrorMessage);FN(SM50FreeError);
#undef FN
 bool load() {
 if(handle)return true;
 const char* path=Opt().airconv;
 handle=dlopen(path?path:OPENBA_AIRCONV_PATH,RTLD_NOW|RTLD_LOCAL);
 if(!handle){fprintf(stderr,"[Metal] airconv: %s\n",dlerror());return false;}
#define LOAD(name) name=reinterpret_cast<decltype(name)>(dlsym(handle,#name));if(!name)return false
 LOAD(SM50Initialize);LOAD(SM50Destroy);LOAD(SM50Compile);LOAD(SM50GetArgumentsInfo);
 LOAD(SM50GetCompiledBitcode);LOAD(SM50DestroyBitcode);LOAD(SM50GetErrorMessage);LOAD(SM50FreeError);
#undef LOAD
 return true;
 }
 void error(sm50_error_t e){if(!e)return;char b[2048]={};SM50GetErrorMessage(e,b,sizeof(b)-1);fprintf(stderr,"[Metal] shader: %s\n",b);SM50FreeError(e);}
};
Shader Translate(const std::vector<uint8_t>& bytes,id<MTLDevice> device) { @autoreleasepool {
 static Airconv api;static std::mutex lock;std::lock_guard<std::mutex> guard(lock);
 Shader out;if(!api.load()||bytes.empty())return out;
 sm50_shader_t shader=nullptr;sm50_error_t err=nullptr;
 if(api.SM50Initialize(bytes.data(),bytes.size(),&shader,&out.reflection,&err)){api.error(err);return out;}
 out.constants.resize(out.reflection.NumConstantBuffers);out.arguments.resize(out.reflection.NumArguments);
 api.SM50GetArgumentsInfo(shader,out.constants.data(),out.arguments.data());
 unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(bytes.data(),(CC_LONG)bytes.size(),digest);
 char hex[65];for(int i=0;i<32;++i)snprintf(hex+2*i,3,"%02x",digest[i]);
 out.identity=hex;
 NSString* dir=[NSTemporaryDirectory() stringByAppendingPathComponent:@"OpenBA-metal-v1-airconv26"];
 [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
 NSString* path=[dir stringByAppendingPathComponent:[[NSString stringWithUTF8String:hex] stringByAppendingString:@".metallib"]];
 NSData* data=[NSData dataWithContentsOfFile:path];bool cached=data!=nil;
 if(!data){
 SM50_SHADER_COMMON_DATA args{};args.type=SM50_SHADER_COMMON;args.metal_version=SM50_SHADER_METAL_320;
 sm50_bitcode_t code=nullptr;
 if(api.SM50Compile(shader,reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA*>(&args),"shader_main",&code,&err))api.error(err);
 else {SM50_COMPILED_BITCODE bc{};api.SM50GetCompiledBitcode(code,&bc);data=[NSData dataWithBytes:bc.Data length:bc.Size];api.SM50DestroyBitcode(code);[data writeToFile:path atomically:YES];}
 }
 api.SM50Destroy(shader);
 if(data){NSError* error=nil;dispatch_data_t dd=dispatch_data_create(data.bytes,data.length,dispatch_get_global_queue(0,0),DISPATCH_DATA_DESTRUCTOR_DEFAULT);
 out.library=[device newLibraryWithData:dd error:&error];out.function=[out.library newFunctionWithName:@"shader_main"];
 if(!out.function)fprintf(stderr,"[Metal] library load: %s\n",error.localizedDescription.UTF8String);
 }
 static unsigned count=0;if(++count<=20)fprintf(stderr,"[Metal] shader %u: %s %s cb=%u args=%u\n",count,out.function?"loaded":"FAILED",cached?"cache":"converted",out.reflection.NumConstantBuffers,out.reflection.NumArguments);
 RegisterPipelineShader(out);
 return out;
} }
}
