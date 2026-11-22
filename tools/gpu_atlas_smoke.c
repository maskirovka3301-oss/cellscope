#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#if defined(__APPLE__)
static const char *VS =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct I{float2 p [[attribute(0)]];float2 uv [[attribute(1)]];float4 c [[attribute(2)]];};\n"
    "struct O{float4 p [[position]];float2 uv;float4 c;};\n"
    "vertex O main0(I i [[stage_in]]){O o;o.p=float4(i.p,0,1);o.uv=i.uv;o.c=i.c;return o;}\n";
static const char *FS =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct O{float4 p [[position]];float2 uv;float4 c;};\n"
    "fragment float4 main0(O i [[stage_in]],texture2d<float> t [[texture(0)]],"
    "sampler s [[sampler(0)]]){return t.sample(s,i.uv)*i.c;}\n";
#endif

typedef struct { float x,y,u,v,r,g,b,a; } V;

static SDL_GPUShader* mk(SDL_GPUDevice*g,SDL_GPUShaderStage st,const char*c){
#if defined(__APPLE__)
 SDL_GPUShaderCreateInfo i={0};i.code=(const Uint8*)c;i.code_size=SDL_strlen(c);
 i.entrypoint="main0";i.format=SDL_GPU_SHADERFORMAT_MSL;i.stage=st;
 if(st==SDL_GPU_SHADERSTAGE_FRAGMENT)i.num_samplers=1;
 return SDL_CreateGPUShader(g,&i);
#else
 (void)g;(void)st;(void)c;return NULL;
#endif
}

int main(void){
 SDL_Init(SDL_INIT_VIDEO);
 SDL_Window*w=SDL_CreateWindow("CellScope GPU atlas",480,320,SDL_WINDOW_HIGH_PIXEL_DENSITY);
 SDL_GPUDevice*g=SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL,true,NULL);
 if(!w||!g||!SDL_ClaimWindowForGPUDevice(g,w)){fprintf(stderr,"init: %s\n",SDL_GetError());return 1;}
 SDL_GPUShader*vs=mk(g,SDL_GPU_SHADERSTAGE_VERTEX,VS),*fs=mk(g,SDL_GPU_SHADERSTAGE_FRAGMENT,FS);
 SDL_GPUVertexBufferDescription vb={0};vb.pitch=sizeof(V);vb.input_rate=SDL_GPU_VERTEXINPUTRATE_VERTEX;
 SDL_GPUVertexAttribute at[3]={{0,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,0},{1,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,2*sizeof(float)},{2,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,4*sizeof(float)}};
 SDL_GPUGraphicsPipelineCreateInfo pc={0};pc.vertex_shader=vs;pc.fragment_shader=fs;
 pc.vertex_input_state.vertex_buffer_descriptions=&vb;pc.vertex_input_state.num_vertex_buffers=1;
 pc.vertex_input_state.vertex_attributes=at;pc.vertex_input_state.num_vertex_attributes=3;
 pc.primitive_type=SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;pc.rasterizer_state.fill_mode=SDL_GPU_FILLMODE_FILL;
 pc.rasterizer_state.cull_mode=SDL_GPU_CULLMODE_NONE;
 SDL_GPUColorTargetDescription ct={0};ct.format=SDL_GetGPUSwapchainTextureFormat(g,w);
 pc.target_info.color_target_descriptions=&ct;pc.target_info.num_color_targets=1;
 SDL_GPUGraphicsPipeline*pipe=SDL_CreateGPUGraphicsPipeline(g,&pc);
 if(!pipe){fprintf(stderr,"pipeline: %s\n",SDL_GetError());return 1;}
 // 4x4 RGBA atlas: four colored quadrants
 unsigned char px[4*4*4]; for(int y=0;y<4;y++)for(int x=0;x<4;x++){
  int i=(y*4+x)*4; int q=(x<2?0:1)+(y<2?0:2);
  unsigned char col[4][3]={{230,80,90},{90,200,140},{90,150,240},{240,200,80}};
  px[i]=col[q][0];px[i+1]=col[q][1];px[i+2]=col[q][2];px[i+3]=255;
 }
 SDL_GPUTextureCreateInfo ti={0};ti.type=SDL_GPU_TEXTURETYPE_2D;ti.format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
 ti.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER;ti.width=4;ti.height=4;ti.layer_count_or_depth=1;ti.num_levels=1;
 SDL_GPUTexture*tex=SDL_CreateGPUTexture(g,&ti);
 SDL_GPUTransferBufferCreateInfo tbi={0};tbi.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;tbi.size=sizeof(px);
 SDL_GPUTransferBuffer*tb=SDL_CreateGPUTransferBuffer(g,&tbi);
 void*m=SDL_MapGPUTransferBuffer(g,tb,false);memcpy(m,px,sizeof(px));SDL_UnmapGPUTransferBuffer(g,tb);
 SDL_GPUCommandBuffer*cmd=SDL_AcquireGPUCommandBuffer(g);
 SDL_GPUCopyPass*cp=SDL_BeginGPUCopyPass(cmd);
 SDL_GPUTextureTransferInfo src={tb,0,4,4};
 SDL_GPUTextureRegion dst={tex,0,0,0,4,4,1};
 SDL_UploadToGPUTexture(cp,&src,&dst,false);SDL_EndGPUCopyPass(cp);
 // draw a quad
 V verts[6]={{-.8f,-.8f,0,1,1,1,1,1},{.8f,-.8f,1,1,1,1,1,1},{.8f,.8f,1,0,1,1,1,1},
             {-.8f,-.8f,0,1,1,1,1,1},{.8f,.8f,1,0,1,1,1,1},{-.8f,.8f,0,0,1,1,1,1}};
 SDL_GPUBufferCreateInfo bci={0};bci.usage=SDL_GPU_BUFFERUSAGE_VERTEX;bci.size=sizeof(verts);
 SDL_GPUBuffer*vbo=SDL_CreateGPUBuffer(g,&bci);
 SDL_GPUTransferBufferCreateInfo vbi={0};vbi.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;vbi.size=sizeof(verts);
 SDL_GPUTransferBuffer*vbt=SDL_CreateGPUTransferBuffer(g,&vbi);
 void*vm=SDL_MapGPUTransferBuffer(g,vbt,false);memcpy(vm,verts,sizeof(verts));SDL_UnmapGPUTransferBuffer(g,vbt);
 SDL_GPUCopyPass*cp2=SDL_BeginGPUCopyPass(cmd);SDL_GPUTransferBufferLocation vsrc={vbt,0};SDL_GPUBufferRegion vdst={vbo,0,sizeof(verts)};
 SDL_UploadToGPUBuffer(cp2,&vsrc,&vdst,false);SDL_EndGPUCopyPass(cp2);
 SDL_GPUSamplerCreateInfo si={0};si.min_filter=SDL_GPU_FILTER_NEAREST;si.mag_filter=SDL_GPU_FILTER_NEAREST;
 si.address_mode_u=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;si.address_mode_v=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
 SDL_GPUSampler*smp=SDL_CreateGPUSampler(g,&si);
 SDL_GPUTexture*swap=NULL;
 if(!SDL_AcquireGPUSwapchainTexture(cmd,w,&swap,NULL,NULL)){fprintf(stderr,"swap: %s\n",SDL_GetError());return 1;}
 if(swap){
  SDL_GPUColorTargetInfo ci={0};ci.texture=swap;ci.clear_color=(SDL_FColor){.03f,.04f,.05f,1};ci.load_op=SDL_GPU_LOADOP_CLEAR;ci.store_op=SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass*p=SDL_BeginGPURenderPass(cmd,&ci,1,NULL);
  SDL_BindGPUGraphicsPipeline(p,pipe);
  SDL_GPUBufferBinding bind={vbo,0};SDL_BindGPUVertexBuffers(p,0,&bind,1);
  SDL_GPUTextureSamplerBinding ts={tex,smp};SDL_BindGPUFragmentSamplers(p,0,&ts,1);
  SDL_DrawGPUPrimitives(p,6,1,0,0);SDL_EndGPURenderPass(p);
 }
 SDL_SubmitGPUCommandBuffer(cmd);SDL_Delay(1000);
 SDL_ReleaseGPUSampler(g,smp);SDL_ReleaseGPUTransferBuffer(g,vbt);SDL_ReleaseGPUBuffer(g,vbo);
 SDL_ReleaseGPUTransferBuffer(g,tb);SDL_ReleaseGPUTexture(g,tex);SDL_ReleaseGPUGraphicsPipeline(g,pipe);
 SDL_ReleaseGPUShader(g,vs);SDL_ReleaseGPUShader(g,fs);
 SDL_ReleaseWindowFromGPUDevice(g,w);SDL_DestroyGPUDevice(g);SDL_DestroyWindow(w);SDL_Quit();return 0;
}
