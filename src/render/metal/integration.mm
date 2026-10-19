// Compatibility symbols keep the existing plugin slot ABI; all objects are Metal.
#include "VideoMetalData.h"
#include "../dx11/VideoDX11Plugin.h"
#include "video/Context.h"
#include "video/Device.h"
#include "../dx11/ao/ao_backend.h"
#include "../dx11/foliage_aa/foliage_aa_backend.h"
#include "../dx11/billboard_shadow/billboard_shadow_backend.h"
#include "ext/render_pipe/finish_split.h"
#include "passes/assao_pass.h"
namespace VideoDX11 {
Video::DeviceData* Dx11_CreateDeviceData(Video::Device* d){return new VideoMetal::DeviceData(d);}
const char* Dx11_GetDeviceName(){return "Metal";}
Video::DeviceData* Dx9Placeholder_CreateDeviceData(Video::Device* d){return new VideoMetal::DeviceData(d);}
const char* Dx9Placeholder_GetDeviceName(){return "Metal (compatibility slot)";}
bool Dx11EnumerateDxgiInterfaces(Video::Device* d,const char* n){return VideoMetal::Enumerate(d,n);}
IDXGISwapChain* Dx11SwapChain(){return nullptr;}
}
namespace AoBackend {bool Supported(){return VideoMetal::DeviceLive();} void RequestReadableDepth(bool){} bool DepthReadable(){return VideoMetal::DepthReadable();}}
namespace FaaBackend {void Request(int,float,float,const BbsRewrite::Params*){}}
namespace BbsBackend {void Request(bool){}}
namespace RenderPipe {
int32_t FinishPresent(Video::Context& ctx){ctx._contextData->Finish();return 0;}
void FinishRecover(Video::Device&,int32_t){}
}
#include "graphics/renderer_ext_ao.h"
namespace Graphics {void ExtAoAfterOpaque(Video::Context& context,const ExtAoFrame& frame,const RendererSceneEntry& entry) {VideoMetal::Assao::AfterOpaque(context,frame,entry);}}
