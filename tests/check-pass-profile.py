"""Exercise actual command replay/planning with captured Vulkan calls, not a GPU."""
from pathlib import Path
import re, sys
sdk=Path(sys.argv[1])
header=(sdk/'include/rex/graphics/vulkan/deferred_command_buffer.h').read_text()
source=(sdk/'src/graphics/vulkan/deferred_command_buffer.cpp').read_text()
header=re.sub(r'#include <rex/[^>]+>','',header).replace('#pragma once','').replace(' private:',' public:')
def function(marker):
 start=source.index(marker); end=source.index('{',start)+1; depth=1
 while depth:
  if source[end]=='{':depth+=1
  elif source[end]=='}':depth-=1
  end+=1
 return source[start:end]
execute=function('void DeferredCommandBuffer::Execute(')
names=sorted(set(re.findall(r'dfn\.(vk\w+)',execute)))
ids={name:i for i,name in enumerate(names)}
code=r'''#include <vulkan/vulkan.h>
#include <rex/graphics/vulkan/pass_profile.h>
#include <algorithm>
#include <type_traits>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <vector>
#define assert_null(x) ((void)0)
#define assert_unhandled_case(x) std::abort()
static unsigned checks=0;
static void check(bool b){++checks;if(!b){std::fprintf(stderr,"Pass profile check %u failed\n",checks);std::abort();}}
static bool inside=false;
static std::vector<int> calls;
static std::vector<uint32_t> queries;
namespace rex {template<class T,class U> constexpr T align(T value,U alignment){return (value+alignment-1)/alignment*alignment;}}
'''
code+='static void event(int id){\n'
code+=f'if(id=={ids["vkCmdBeginRendering"]} || id=={ids["vkCmdBeginRenderPass"]}){{check(!inside);inside=true;}}\n'
code+=f'if(id=={ids["vkCmdEndRendering"]} || id=={ids["vkCmdEndRenderPass"]}){{check(inside);inside=false;}}\n'
code+='calls.push_back(id);}\n'
code+=r'''template<int Id, class F> struct Stub;
template<int Id, class R, class... A> struct Stub<Id,R(VKAPI_PTR*)(A...)> {
 static R VKAPI_CALL call(A...){event(Id);if constexpr(!std::is_void_v<R>)return R{};}
};
static void VKAPI_CALL timestamp(VkCommandBuffer,VkPipelineStageFlagBits stage,VkQueryPool pool,uint32_t query){
 check(!inside && stage==VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT && pool!=VK_NULL_HANDLE);
 queries.push_back(query);
}
namespace rex::ui::vulkan { struct VulkanDevice {struct Functions {
'''
for name in names:
 code+=f'PFN_{name} {name} = '+ ('&timestamp;' if name=='vkCmdWriteTimestamp' else f'&Stub<{ids[name]},PFN_{name}>::call;')+'\n'
code+=r'''} functions_; const Functions& functions()const{return functions_;}};}
'''+header+r'''
namespace rex::graphics::vulkan {
class VulkanCommandProcessor {public:ui::vulkan::VulkanDevice* GetVulkanDevice()const{static ui::vulkan::VulkanDevice d;return &d;}};
'''
for marker in ['DeferredCommandBuffer::DeferredCommandBuffer(', 'void DeferredCommandBuffer::Reset()',
 'PassProfileCapture DeferredCommandBuffer::BuildPassProfile(', 'void DeferredCommandBuffer::Execute(',
 'void DeferredCommandBuffer::CmdVkBeginRendering(', 'void* DeferredCommandBuffer::WriteCommand(']:
 code+=function(marker)+'\n'
code+=r'''}
using namespace rex::graphics::vulkan;
static void begin(DeferredCommandBuffer& c,uint32_t width=1280,uint32_t height=720,VkRenderingFlags flags=0){
 VkRenderingInfo info{};info.sType=VK_STRUCTURE_TYPE_RENDERING_INFO;info.layerCount=1;info.flags=flags;
 info.renderArea={{0,0},{width,height}};c.CmdVkBeginRendering(&info);
}
int main(){
 VulkanCommandProcessor cp;DeferredCommandBuffer c(cp);
 check(c.BuildPassProfile(0).count==0);
 double ns=-1;uint64_t values[]={10,1,30,1};
 check(PassProfileNanoseconds(values,64,2,ns)&&ns==40);
 values[3]=0;check(!PassProfileNanoseconds(values,64,2,ns));values[3]=1;
 check(!PassProfileNanoseconds(values,0,1,ns));check(!PassProfileNanoseconds(values,65,1,ns));
 check(!PassProfileNanoseconds(values,64,0,ns));check(!PassProfileNanoseconds(values,64,-1,ns));
 check(!PassProfileNanoseconds(values,64,std::numeric_limits<double>::infinity(),ns));
 values[0]=UINT32_MAX-999;values[2]=1000;check(PassProfileNanoseconds(values,32,1,ns)&&ns==2000);
 values[0]=UINT64_MAX-9;values[2]=10;check(PassProfileNanoseconds(values,64,1,ns)&&ns==20);
 std::mt19937 rng(4422);
 for(unsigned trial=0;trial<160;++trial){
  c.Reset(); const unsigned passes=1+rng()%48;
  for(unsigned j=0;j<passes;++j){
   c.CmdProfileContext(PassDomain::kResolve);c.CmdVkDispatch(1,1,1);
   begin(c,640+j,360+j);
   c.CmdProfileContext(PassDomain::kGuest,100+j,200+j);c.CmdVkDraw(3,1,0,0);
   if(j%3==0){c.CmdProfileContext(PassDomain::kGuest,999,999);c.CmdVkDraw(6,1,0,0);}
   if(j%5==0){c.CmdProfileContext(PassDomain::kOwnership);c.CmdVkDraw(3,1,0,0);}
   c.CmdVkEndRendering();
  }
  calls.clear();queries.clear();c.Execute(VK_NULL_HANDLE);const auto baseline=calls;check(queries.empty());
  std::vector<bool> visited(2*passes,false);
  for(unsigned serial=0;serial<2*passes;++serial){
   auto profile=c.BuildPassProfile(serial);check(profile.population==2*passes);
   check(profile.count==std::min<unsigned>(8,2*passes));profile.pool=(VkQueryPool)(uintptr_t)1;
   std::vector<bool> distinct(2*passes,false);
   for(unsigned k=0;k<profile.count;++k){const auto& a=profile.segments[k];
    check(a.begin<a.end&&a.end<=c.command_stream_.size());check(!distinct[a.ordinal]);distinct[a.ordinal]=true;visited[a.ordinal]=true;
    if(a.render){unsigned j=a.ordinal/2;check(a.width==640+j&&a.height==360+j);
     check((a.domains&1)&&a.vertex_shader==100+j&&a.pixel_shader==200+j);
     check(a.mixed_shaders==(j%3==0));check(bool(a.domains&2)==(j%5==0));
    }else{check(a.dispatches==1&&a.domains==4&&a.draws==0);}
   }
   calls.clear();queries.clear();c.Execute(VK_NULL_HANDLE,&profile);
   check(calls==baseline&&queries.size()==2*profile.count&&!inside);
   std::sort(queries.begin(),queries.end());for(unsigned k=0;k<queries.size();++k)check(queries[k]==k);
  }
  check(std::all_of(visited.begin(),visited.end(),[](bool x){return x;}));
 }
 c.Reset();begin(c);check(c.BuildPassProfile(0).count==0); // Unterminated.
 c.Reset();begin(c,1,1,VK_RENDERING_SUSPENDING_BIT);c.CmdVkDraw(3,1,0,0);c.CmdVkEndRendering();check(c.BuildPassProfile(0).count==0);
 c.Reset();begin(c);begin(c);c.CmdVkEndRendering();check(c.BuildPassProfile(0).count==0); // Nested.
 c.Reset();c.CmdVkEndRendering();check(c.BuildPassProfile(0).count==0);
 std::printf("%u actual replay, metadata, rotating coverage and timestamp validation checks passed\n",checks);
}
'''
Path(sys.argv[2]).write_text(code,encoding='utf-8',newline='\n')
