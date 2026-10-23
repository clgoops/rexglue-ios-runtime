"""Compile the actual deferred command writer and optimization with Vulkan structs.

No GPU or game is simulated. These checks verify that all affected pixels remain
inside each optimized pass, and unsupported attachment/state semantics fall back.
"""
from pathlib import Path
import sys,re
sdk=Path(sys.argv[1])
header=(sdk/'include/rex/graphics/vulkan/deferred_command_buffer.h').read_text(encoding='utf-8')
source=(sdk/'src/graphics/vulkan/deferred_command_buffer.cpp').read_text(encoding='utf-8')
header=re.sub(r'#include <rex/[^>]+>','',header).replace('#pragma once','').replace(' private:',' public:')
def function(marker):
 start=source.index(marker);i=source.index('{',start);depth=1;end=i+1
 while depth:
  if source[end]=='{':depth+=1
  elif source[end]=='}':depth-=1
  end+=1
 return source[start:end]
out=r'''#include <vulkan/vulkan.h>
#include <rex/graphics/vulkan/transfer_scissor.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#define assert_null(x) ((void)0)
namespace rex {template<class T,class U> constexpr T align(T value,U alignment){return (value+alignment-1)/alignment*alignment;}}
'''+header+'\nnamespace rex::graphics::vulkan {\nclass VulkanCommandProcessor {};\n'
for marker in ['DeferredCommandBuffer::DeferredCommandBuffer(', 'void DeferredCommandBuffer::Reset()',
 'DeferredCommandBuffer::RenderAreaStats DeferredCommandBuffer::OptimizeRenderAreas(',
 'void DeferredCommandBuffer::CmdVkBeginRendering(', 'void* DeferredCommandBuffer::WriteCommand(']:
 out+=function(marker)+'\n'
out+=r'''}
using namespace rex::graphics::vulkan;
static unsigned checks=0;
static void check(bool value){++checks;if(!value){std::fprintf(stderr,"Render-area check %u failed\n",checks);std::abort();}}
static bool equal(VkRect2D a,VkRect2D b){return a.offset.x==b.offset.x&&a.offset.y==b.offset.y&&a.extent.width==b.extent.width&&a.extent.height==b.extent.height;}
static std::vector<VkRect2D> areas(DeferredCommandBuffer& c){
 std::vector<VkRect2D> result;size_t i=0;
 while(i<c.command_stream_.size()){
  auto& h=*reinterpret_cast<DeferredCommandBuffer::CommandHeader*>(c.command_stream_.data()+i);
  i+=DeferredCommandBuffer::kCommandHeaderSizeElements;
  if(h.command==DeferredCommandBuffer::Command::kVkBeginRendering)
   result.push_back(reinterpret_cast<DeferredCommandBuffer::ArgsVkBeginRendering*>(c.command_stream_.data()+i)->render_area);
  i+=h.arguments_size_elements;
 }
 return result;
}
static VkRect2D full={{0,0},{1280,8192}};
static void begin(DeferredCommandBuffer& c,int fallback=0){
 VkRenderingAttachmentInfo a={};a.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
 a.imageView=(VkImageView)(uintptr_t)1;a.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
 if(fallback==1)a.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
 if(fallback==2)a.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
 if(fallback==3)a.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
 if(fallback==4)a.resolveMode=VK_RESOLVE_MODE_AVERAGE_BIT;
 if(fallback==5)a.pNext=&a;
 VkRenderingInfo info={};info.sType=VK_STRUCTURE_TYPE_RENDERING_INFO;info.renderArea=full;
 info.layerCount=fallback==6?2:1;info.viewMask=fallback==7?1:0;info.flags=fallback==8?VK_RENDERING_SUSPENDING_BIT:0;
 info.colorAttachmentCount=1;info.pColorAttachments=&a;
 c.CmdVkBeginRendering(&info);
}
static void draw(DeferredCommandBuffer& c,VkRect2D rect,bool indexed=false){
 c.CmdVkSetScissor(0,1,&rect);
 if(indexed)c.CmdVkDrawIndexed(3,1,0,0,0);else c.CmdVkDraw(3,1,0,0);
}
static void clear(DeferredCommandBuffer& c,VkRect2D rect){
 VkClearAttachment a={};a.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
 VkClearRect r={rect,0,1};c.CmdVkClearAttachments(1,&a,1,&r);
}
static void contains(VkRect2D outer,VkRect2D raw){
 int64_t x0=std::max<int64_t>(full.offset.x,raw.offset.x),y0=std::max<int64_t>(full.offset.y,raw.offset.y);
 int64_t x1=std::min(int64_t(full.offset.x)+full.extent.width,int64_t(raw.offset.x)+raw.extent.width);
 int64_t y1=std::min(int64_t(full.offset.y)+full.extent.height,int64_t(raw.offset.y)+raw.extent.height);
 if(x1<=x0||y1<=y0)return;
 check(outer.offset.x<=x0&&outer.offset.y<=y0&&int64_t(outer.offset.x)+outer.extent.width>=x1&&int64_t(outer.offset.y)+outer.extent.height>=y1);
}
int main(){
 VulkanCommandProcessor cp;DeferredCommandBuffer c(cp,8);
 // Feed actual ownership-transfer scissors to the actual deferred optimizer.
 TransferScissor copy_bounds(full.extent,1,1);copy_bounds.Include(0,0,320,180);
 begin(c);draw(c,copy_bounds.Get(true));c.CmdVkEndRendering();
 auto copy_stats=c.OptimizeRenderAreas(true);
 check(equal(areas(c)[0],{{0,0},{352,192}}));
 check(copy_stats.used_pixels==352*192&&copy_stats.original_pixels==1280ull*8192);
 c.Reset();begin(c);draw(c,copy_bounds.Get(true));
 clear(c,{{1000,8000},{280,192}});c.CmdVkEndRendering();c.OptimizeRenderAreas(true);
 check(equal(areas(c)[0],full)); // Clear rectangles still define writes independently.
 c.Reset();begin(c);draw(c,copy_bounds.Get(false));c.CmdVkEndRendering();c.OptimizeRenderAreas(true);
 check(equal(areas(c)[0],full)); // Disabling the setting retains the previous behavior.
 c.Reset();begin(c);draw(c,copy_bounds.Get(true));c.CmdVkEndRendering();
 begin(c);draw(c,{{0,0},{1280,720}});c.CmdVkEndRendering();c.OptimizeRenderAreas(true);
 check(equal(areas(c)[1],{{0,0},{1280,736}})); // The following guest scissor replaces copy state.
 c.Reset();
 begin(c);draw(c,{{0,0},{320,180}});c.CmdVkEndRendering();
 auto stats=c.OptimizeRenderAreas(true);
 check(equal(areas(c)[0],{{0,0},{320,192}}));check(stats.passes==1&&stats.reduced_passes==1);
 check(stats.original_pixels==1280ull*8192&&stats.used_pixels==320*192);
 c.Reset();begin(c);draw(c,{{0,0},{320,180}});draw(c,{{900,700},{100,80}},true);
 clear(c,{{600,1200},{300,100}});c.CmdVkEndRendering();stats=c.OptimizeRenderAreas(true);
 check(equal(areas(c)[0],{{0,0},{1024,1312}}));
 for(int fallback=1;fallback<=8;++fallback){
  c.Reset();begin(c,fallback);draw(c,{{0,0},{32,32}});c.CmdVkEndRendering();c.OptimizeRenderAreas(true);
  check(equal(areas(c)[0],full));
 }
 c.Reset();begin(c);draw(c,{{0,0},{32,32}});c.CmdVkEndRendering();stats=c.OptimizeRenderAreas(false);
 check(equal(areas(c)[0],full)&&stats.original_pixels==stats.used_pixels&&stats.reduced_passes==0);
 c.Reset();begin(c);c.CmdVkDraw(3,1,0,0);draw(c,{{0,0},{32,32}});c.CmdVkEndRendering();c.OptimizeRenderAreas(true);
 check(equal(areas(c)[0],full)); // Unknown scissor at first draw cannot be repaired by later state.
 c.Reset();begin(c);draw(c,{{0,0},{32,32}});c.OptimizeRenderAreas(true);check(equal(areas(c)[0],full));
 c.Reset();begin(c);c.CmdVkEndRendering();c.OptimizeRenderAreas(true);check(equal(areas(c)[0],full));
 c.Reset();begin(c);draw(c,{{0,0},{0,0}});c.CmdVkEndRendering();c.OptimizeRenderAreas(true);check(equal(areas(c)[0],full));
 c.Reset();draw(c,{{96,96},{64,64}});begin(c);c.CmdVkDraw(3,1,0,0);c.CmdVkEndRendering();
 begin(c);c.CmdVkDraw(3,1,0,0);c.CmdVkEndRendering();c.OptimizeRenderAreas(true);
 check(equal(areas(c)[0],{{96,96},{64,64}})&&equal(areas(c)[1],areas(c)[0]));
 // Explicit clears do not use the current scissor. Include a far clear rectangle.
 c.Reset();begin(c);draw(c,{{0,0},{32,32}});clear(c,{{0,8000},{1280,192}});c.CmdVkEndRendering();c.OptimizeRenderAreas(true);
 check(equal(areas(c)[0],full));
 c.Reset();begin(c);VkRect2D two[2]={{{0,0},{32,32}},{{100,100},{32,32}}};c.CmdVkSetScissor(0,2,two);c.CmdVkDraw(3,1,0,0);c.CmdVkEndRendering();c.OptimizeRenderAreas(true);
 check(equal(areas(c)[0],full));
 std::mt19937 random(43);
 for(int trial=0;trial<1000;++trial){
  c.Reset();begin(c);std::vector<VkRect2D> touched;
  for(int i=0;i<20;++i){VkRect2D r={{int32_t(random()%1800)-200,int32_t(random()%10000)-200},{uint32_t(random()%400),uint32_t(random()%500)}};
   touched.push_back(r);if(i%7==0)clear(c,r);else draw(c,r,i%2);}
  c.CmdVkEndRendering();stats=c.OptimizeRenderAreas(true);auto area=areas(c)[0];
  check(stats.used_pixels<=stats.original_pixels&&area.extent.width&&area.extent.height);
  for(auto r:touched)contains(area,r);
 }
 std::printf("%u actual deferred render-area checks passed\n",checks);
}
'''
Path(sys.argv[2]).write_text(out,encoding='utf-8')
