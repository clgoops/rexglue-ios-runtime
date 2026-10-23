"""Exercise actual completed-command-buffer reclamation and timestamp reading.

Uses a captured Vulkan query API; no GPU or performance speedup is simulated.
"""
from pathlib import Path
import sys
text=(Path(sys.argv[1])/'src/graphics/vulkan/command_processor.cpp').read_text(encoding='utf-8')
start=text.index('  while (!command_buffers_submitted_.empty()) {',text.index('void VulkanCommandProcessor::CheckSubmissionFenceAndDeviceLoss'))
i=text.index('{',start)+1;depth=1
while depth:
 if text[i]=='{':depth+=1
 elif text[i]=='}':depth-=1
 i+=1
body=text[start:i]
code=r'''#include <vulkan/vulkan.h>
#include <rex/graphics/performance_stats.h>
#include <rex/graphics/vulkan/pass_profile.h>
#define REXGPU_INFO(...) ((void)0)
using namespace rex::graphics::vulkan;
#include <algorithm>
#include <array>
#include <deque>
#include <vector>
#include <memory>
#include <cstdio>
#include <cstdlib>
using rex::graphics::PerformanceStats;
static unsigned checks=0;
static void check(bool b){++checks;if(!b){std::fprintf(stderr,"Timing check %u failed\n",checks);std::abort();}}
struct CommandBuffer {VkQueryPool timing_pool=VK_NULL_HANDLE;bool timing_recorded=false;PassProfileCapture pass_profile;};
static std::deque<std::pair<uint64_t,CommandBuffer>> command_buffers_submitted_;
static std::vector<CommandBuffer> command_buffers_writable_;
static uint64_t submission_completed_=0;
static uint32_t timestamp_valid_bits_=32;
static double timestamp_period_=1.0;
static auto performance_stats_=std::make_shared<PerformanceStats>();
static VkDevice device=VK_NULL_HANDLE;
static unsigned pass_calls=0;
static VkResult pass_status=VK_SUCCESS;
static unsigned calls=0;
static VkResult status=VK_SUCCESS;
static std::array<uint64_t,4> data={1000000,1,3500000,1};
static VkQueryResultFlags flags_seen=0;
struct Dfn {
 VkResult vkGetQueryPoolResults(VkDevice,VkQueryPool pool,uint32_t first,uint32_t count,
     size_t size,void* output,VkDeviceSize stride,VkQueryResultFlags flags){
  flags_seen=flags;
  if(pool==(VkQueryPool)(uintptr_t)2){
   ++pass_calls;check(first==0&&count==4&&stride==16&&size==256);
   uint64_t samples[]={100,1,200,1,300,1,600,1};
   std::copy(samples,samples+8,static_cast<uint64_t*>(output));return pass_status;
  }
  ++calls;
  check(pool!=VK_NULL_HANDLE&&first==0&&count==2&&size==32&&stride==16);
  std::copy(data.begin(),data.end(),static_cast<uint64_t*>(output));return status;
 }
} dfn;
static void reclaim(){
'''+body+r'''
}
static void queue(uint64_t submission=1){command_buffers_submitted_.push_back({submission,{(VkQueryPool)(uintptr_t)1,true}});}
int main(){
 performance_stats_->SetEnabled(true);queue();reclaim();
 check(calls==0&&command_buffers_writable_.empty()&&command_buffers_submitted_.size()==1);
 submission_completed_=1;reclaim();
 auto m=performance_stats_->GetMetrics();
 check(calls==1&&command_buffers_submitted_.empty()&&command_buffers_writable_.size()==1);
 check(!command_buffers_writable_.back().timing_recorded&&command_buffers_writable_.back().timing_pool!=VK_NULL_HANDLE);
 check(m.gpu_submission_ms==2.5&&m.gpu_timed_submissions==1);
 check((flags_seen&VK_QUERY_RESULT_WAIT_BIT)==0&&(flags_seen&VK_QUERY_RESULT_WITH_AVAILABILITY_BIT));
 queue();status=VK_NOT_READY;reclaim();check(performance_stats_->GetMetrics().gpu_timed_submissions==1);
 queue();status=VK_SUCCESS;data[3]=0;reclaim();check(performance_stats_->GetMetrics().gpu_timed_submissions==1);
 queue();data={UINT32_MAX-999,1,1000,1};reclaim();
 m=performance_stats_->GetMetrics();check(m.gpu_timed_submissions==2&&m.gpu_submission_ms==2.502);
 queue();timestamp_valid_bits_=64;data={1000000,1,5000000,1};timestamp_period_=2;reclaim();
 check(performance_stats_->GetMetrics().gpu_submission_ms==10.502);
 performance_stats_->SetEnabled(false);queue();reclaim();check(performance_stats_->GetMetrics().gpu_timed_submissions==3);
 const auto old_calls=calls;command_buffers_submitted_.push_back({1,{(VkQueryPool)(uintptr_t)1,false}});reclaim();check(calls==old_calls);
 queue(9);submission_completed_=8;reclaim();check(calls==old_calls&&command_buffers_submitted_.size()==1);
 submission_completed_=9;reclaim(); // Complete the deliberately outstanding basic sample.
 auto pass_queue=[](){CommandBuffer b;b.pass_profile.pool=(VkQueryPool)(uintptr_t)2;b.pass_profile.count=2;
  command_buffers_submitted_.push_back({11,b});};
 pass_queue();submission_completed_=10;reclaim();check(pass_calls==0);
 submission_completed_=11;reclaim();check(pass_calls==1);
 check(command_buffers_writable_.back().pass_profile.count==0);
 check(command_buffers_writable_.back().pass_profile.pool==(VkQueryPool)(uintptr_t)2);
 check(!(flags_seen&VK_QUERY_RESULT_WAIT_BIT)&&(flags_seen&VK_QUERY_RESULT_WITH_AVAILABILITY_BIT));
 pass_queue();pass_status=VK_NOT_READY;reclaim();check(pass_calls==2&&command_buffers_writable_.back().pass_profile.count==0);
 pass_queue();pass_status=VK_ERROR_DEVICE_LOST;reclaim();check(pass_calls==3&&command_buffers_writable_.back().pass_profile.count==0);
 std::printf("%u actual deferred GPU timing checks passed\n",checks);
}
'''
Path(sys.argv[2]).write_text(code,encoding='utf-8')
