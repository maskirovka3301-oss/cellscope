#include <SDL3/SDL.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <string.h>
#include "fonts.h"

#if defined(__APPLE__)
static const char *VS =
 "#include <metal_stdlib>\nusing namespace metal;\n"
 "struct I{float2 p [[attribute(0)]];float2 uv [[attribute(1)]];float4 c [[attribute(2)]];};\n"
 "struct O{float4 p [[position]];float2 uv;float4 c;};\n"
 "vertex O main0(I i [[stage_in]]){O o;o.p=float4(i.p,0,1);o.uv=i.uv;o.c=i.c;return o;}\n";
static const char *FS =
 "#include <metal_stdlib>\nusing namespace metal;\n"
 "struct O{float4 p [[position]];float2 uv;float4 c;};\n"
 "fragment float4 main0(O i [[stage_in]],texture2d<float> t [[texture(0)]],sampler s [[sampler(0)]])"
 "{float a=t.sample(s,i.uv).r;return float4(i.c.rgb,i.c.a*a);}\n";
#endif
typedef struct { float x,y,u,v,r,g,b,a; } V;
typedef struct { float u,v,w,h,adv,bearing; } Glyph;

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
 Fonts fonts; char err[256];
 if(!fonts_init(&fonts,"assets",1.0f,err,sizeof(err))){fprintf(stderr,"fonts: %s\n",err);return 1;}
 if(!fonts_set_size(&fonts,FONT_UI,32)){fprintf(stderr,"font size failed\n");return 1;}
 FT_Face face=fonts.face[FONT_UI];
 const int AT=1024; unsigned char *atlas=calloc((size_t)AT*AT,1);
 Glyph glyphs[128]; memset(glyphs,0,sizeof(glyphs));
 int pen=2,row=2,rowh=0;
 for(int ch=32;ch<127;ch++){
  if(FT_Load_Char(face,(FT_ULong)ch,FT_LOAD_RENDER|FT_LOAD_TARGET_LIGHT)!=0)continue;
  FT_GlyphSlot g=face->glyph; int w=g->bitmap.width,h=g->bitmap.rows;
  if(pen+w+1>AT){pen=2;row+=rowh+1;rowh=0;}
  if(row+h+1>AT)break;
  for(unsigned y=0;y<h;y++)memcpy(atlas+(size_t)(row+y)*AT+pen,g->bitmap.buffer+(size_t)y*g->bitmap.pitch,w);
  glyphs[ch]=(Glyph){(float)pen/AT,(float)row/AT,(float)w,(float)h,
                     (float)(g->advance.x>>6),(float)g->bitmap_left};
  pen+=w+1; if((int)h>rowh)rowh=h;
 }
 SDL_Init(SDL_INIT_VIDEO);
 SDL_Window*win=SDL_CreateWindow("CellScope GPU text",640,220,SDL_WINDOW_HIGH_PIXEL_DENSITY);
 SDL_GPUDevice*gpu=SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL,true,NULL);
 if(!win||!gpu||!SDL_ClaimWindowForGPUDevice(gpu,win)){fprintf(stderr,"gpu: %s\n",SDL_GetError());return 1;}
 SDL_GPUShader*vs=mk(gpu,SDL_GPU_SHADERSTAGE_VERTEX,VS),*fs=mk(gpu,SDL_GPU_SHADERSTAGE_FRAGMENT,FS);
 SDL_GPUVertexBufferDescription vbd={0};vbd.pitch=sizeof(V);vbd.input_rate=SDL_GPU_VERTEXINPUTRATE_VERTEX;
 SDL_GPUVertexAttribute at[3]={{0,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,0},{1,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,2*sizeof(float)},{2,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,4*sizeof(float)}};
 SDL_GPUGraphicsPipelineCreateInfo pc={0};pc.vertex_shader=vs;pc.fragment_shader=fs;
 pc.vertex_input_state.vertex_buffer_descriptions=&vbd;pc.vertex_input_state.num_vertex_buffers=1;
 pc.vertex_input_state.vertex_attributes=at;pc.vertex_input_state.num_vertex_attributes=3;
 pc.primitive_type=SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;pc.rasterizer_state.fill_mode=SDL_GPU_FILLMODE_FILL;
 pc.rasterizer_state.cull_mode=SDL_GPU_CULLMODE_NONE;
 SDL_GPUColorTargetDescription ct={0};ct.format=SDL_GetGPUSwapchainTextureFormat(gpu,win);
 pc.target_info.color_target_descriptions=&ct;pc.target_info.num_color_targets=1;
 SDL_GPUGraphicsPipeline*pipe=SDL_CreateGPUGraphicsPipeline(gpu,&pc);
 if(!pipe){fprintf(stderr,"pipeline: %s\n",SDL_GetError());return 1;}
 SDL_GPUTextureCreateInfo ti={0};ti.type=SDL_GPU_TEXTURETYPE_2D;ti.format=SDL_GPU_TEXTUREFORMAT_R8_UNORM;
 ti.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER;ti.width=AT;ti.height=AT;ti.layer_count_or_depth=1;ti.num_levels=1;
 SDL_GPUTexture*tex=SDL_CreateGPUTexture(gpu,&ti);
 SDL_GPUTransferBufferCreateInfo tbi={0};tbi.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;tbi.size=(Uint32)AT*AT;
 SDL_GPUTransferBuffer*tb=SDL_CreateGPUTransferBuffer(gpu,&tbi);
 void*m=SDL_MapGPUTransferBuffer(gpu,tb,false);memcpy(m,atlas,(size_t)AT*AT);SDL_UnmapGPUTransferBuffer(gpu,tb);
 SDL_GPUCommandBuffer*cmd=SDL_AcquireGPUCommandBuffer(gpu);
 SDL_GPUCopyPass*cp=SDL_BeginGPUCopyPass(cmd);
 SDL_GPUTextureTransferInfo src={tb,0,AT,AT};SDL_GPUTextureRegion dst={tex,0,0,0,AT,AT,1};
 SDL_UploadToGPUTexture(cp,&src,&dst,false);SDL_EndGPUCopyPass(cp);
 SDL_GPUSamplerCreateInfo si={0};si.min_filter=SDL_GPU_FILTER_LINEAR;si.mag_filter=SDL_GPU_FILTER_LINEAR;
 si.address_mode_u=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;si.address_mode_v=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
 SDL_GPUSampler*smp=SDL_CreateGPUSampler(gpu,&si);
 V verts[6*64]; int nv=0; const char*text="CellScope"; float cx=-0.55f;
 for(const char*p=text;*p;p++){
  Glyph gl=glyphs[(unsigned char)*p]; if(gl.w<=0)continue;
  float x=cx+gl.bearing/320.0f, y=-0.10f; float w=gl.w/320.0f,h=gl.h/320.0f;
  float u0=gl.u,u1=gl.u+gl.w/AT,v0=gl.v,v1=gl.v+gl.h/AT;
  V q[6]={{x,y,u0,v0,1,1,1,1},{x+w,y,u1,v0,1,1,1,1},{x+w,y+h,u1,v1,1,1,1,1},
          {x,y,u0,v0,1,1,1,1},{x+w,y+h,u1,v1,1,1,1,1},{x,y+h,u0,v1,1,1,1,1}};
  memcpy(&verts[nv],q,sizeof(q));nv+=6;cx+=gl.adv/320.0f;
 }
 SDL_GPUBufferCreateInfo bci={0};bci.usage=SDL_GPU_BUFFERUSAGE_VERTEX;bci.size=nv*sizeof(V);
 SDL_GPUBuffer*vbo=SDL_CreateGPUBuffer(gpu,&bci);
 SDL_GPUTransferBufferCreateInfo vbi={0};vbi.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;vbi.size=nv*sizeof(V);
 SDL_GPUTransferBuffer*vbt=SDL_CreateGPUTransferBuffer(gpu,&vbi);
 void*vm=SDL_MapGPUTransferBuffer(gpu,vbt,false);memcpy(vm,verts,nv*sizeof(V));SDL_UnmapGPUTransferBuffer(gpu,vbt);
 SDL_GPUCommandBuffer*cmd2=SDL_AcquireGPUCommandBuffer(gpu);
 SDL_GPUCopyPass*cp2=SDL_BeginGPUCopyPass(cmd2);SDL_GPUTransferBufferLocation vsrc={vbt,0};SDL_GPUBufferRegion vdst={vbo,0,(Uint32)(nv*sizeof(V))};
 SDL_UploadToGPUBuffer(cp2,&vsrc,&vdst,false);SDL_EndGPUCopyPass(cp2);
 SDL_GPUTexture*swap=NULL;
 if(!SDL_AcquireGPUSwapchainTexture(cmd2,win,&swap,NULL,NULL)){fprintf(stderr,"swap: %s\n",SDL_GetError());return 1;}
 if(swap){SDL_GPUColorTargetInfo ci={0};ci.texture=swap;ci.clear_color=(SDL_FColor){.03f,.04f,.05f,1};ci.load_op=SDL_GPU_LOADOP_CLEAR;ci.store_op=SDL_GPU_STOREOP_STORE;
  SDL_GPURenderPass*p=SDL_BeginGPURenderPass(cmd2,&ci,1,NULL);SDL_BindGPUGraphicsPipeline(p,pipe);
  SDL_GPUBufferBinding b={vbo,0};SDL_BindGPUVertexBuffers(p,0,&b,1);
  SDL_GPUTextureSamplerBinding ts={tex,smp};SDL_BindGPUFragmentSamplers(p,0,&ts,1);
  SDL_DrawGPUPrimitives(p,nv,1,0,0);SDL_EndGPURenderPass(p);}
 SDL_SubmitGPUCommandBuffer(cmd);
 SDL_SubmitGPUCommandBuffer(cmd2);
 SDL_Delay(800);
 SDL_ReleaseGPUSampler(gpu,smp);SDL_ReleaseGPUTexture(gpu,tex);SDL_ReleaseGPUTransferBuffer(gpu,tb);
 SDL_ReleaseGPUTransferBuffer(gpu,vbt);SDL_ReleaseGPUBuffer(gpu,vbo);SDL_ReleaseGPUGraphicsPipeline(gpu,pipe);
 SDL_ReleaseGPUShader(gpu,vs);SDL_ReleaseGPUShader(gpu,fs);
 SDL_ReleaseWindowFromGPUDevice(gpu,win);SDL_DestroyGPUDevice(gpu);SDL_DestroyWindow(win);
 fonts_done(&fonts);free(atlas);SDL_Quit();return 0;
}
