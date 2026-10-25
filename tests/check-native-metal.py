"""Compile the actual native logging function with captured Metal properties.

No GPU is simulated or speedup measured. The object supplies completed-buffer
properties so formatting, invalid results and label gating run through source.
"""
from pathlib import Path
import sys
sdk=Path(sys.argv[1])
queue=(sdk/'thirdparty/moltenvk/MoltenVK/MoltenVK/GPUObjects/MVKQueue.mm').read_text(encoding='utf-8')
cp=(sdk/'src/graphics/vulkan/command_processor.cpp').read_text(encoding='utf-8')
start=queue.index('static void rexLogNativeMetalTiming(')
i=queue.index('{',start)+1;depth=1
while depth:
    if queue[i]=='{':depth+=1
    elif queue[i]=='}':depth-=1
    i+=1
function=queue[start:i]
assert 'const bool query_timing = performance_stats_->enabled() && !native_metal_timing;' in cp
assert 'command_buffer.timing_recorded=query_timing &&' in cp
assert 'if (query_timing && timestamp_valid_bits_' in cp
assert 'if (!native_metal_timing && REXCVAR_GET(vulkan_pass_timing)' in cp
assert 'SetObjectName(VK_OBJECT_TYPE_COMMAND_BUFFER' in cp
commit=queue[queue.index('VkResult MVKQueueCommandBufferSubmission::commitActiveMTLCommandBuffer'):]
assert commit.index('rexLogNativeMetalTiming(this,mtlCB,nativeCommit)')<commit.index('if (signalCompletion) { this->finish(); }')
assert 'waitUntilCompleted' not in function and 'getPerformanceStats' not in function
head=r'''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "REXNativeMetalTiming.h"
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
static unsigned checks=0,logs=0;static char message[2048];
static void check(bool b){++checks;if(!b){fprintf(stderr,"Native logging check %u failed\n",checks);abort();}}
constexpr int MVK_CONFIG_LOG_LEVEL_INFO=3;
struct MVKBaseObject {void reportMessage(int,const char* format,...){++logs;va_list args;va_start(args,format);vsnprintf(message,sizeof(message),format,args);va_end(args);}};
static uint64_t mvkGetRuntimeNanoseconds(){return 12400000000ULL;}
@interface TestMetalBuffer:NSObject
@property(copy) NSString* label;
@property double GPUStartTime;
@property double GPUEndTime;
@property double kernelStartTime;
@property double kernelEndTime;
@property MTLCommandBufferStatus status;
@end
@implementation TestMetalBuffer
@end
'''
tail=r'''
int main(){@autoreleasepool {
  MVKBaseObject owner;TestMetalBuffer* buffer=[TestMetalBuffer new];
  buffer.label=@"unrelated";buffer.status=MTLCommandBufferStatusCompleted;
  buffer.GPUStartTime=12.1;buffer.GPUEndTime=12.3;buffer.kernelStartTime=12.01;buffer.kernelEndTime=12.02;
  rexLogNativeMetalTiming(&owner,(id<MTLCommandBuffer>)buffer,12);check(logs==0);
  rexLogNativeMetalTiming(&owner,nil,12);check(logs==0);
  buffer.label=@"rex.native.frame-9.sub-33";
  rexLogNativeMetalTiming(&owner,(id<MTLCommandBuffer>)buffer,12);check(logs==1);
  check(strstr(message,"label=rex.native.frame-9.sub-33"));check(strstr(message,"valid=1"));
  check(strstr(message,"gpu_ms=200.000000"));check(strstr(message,"pre_gpu_ms=100.000000"));
  check(strstr(message,"driver_ms=10.000000"));check(strstr(message,"callback_lag_ms=100.000000"));
  check(strstr(message,"gpu_start_s=12.100000000"));check(strstr(message,"gpu_end_s=12.300000000"));
  buffer.status=MTLCommandBufferStatusError;buffer.GPUStartTime=0;buffer.GPUEndTime=0;
  rexLogNativeMetalTiming(&owner,(id<MTLCommandBuffer>)buffer,12);check(logs==2);
  check(strstr(message,"valid=0"));check(strstr(message,"gpu_ms=-1.000000"));
  check(strstr(message,"pre_gpu_ms=-1.000000"));
  printf("%u captured Metal-property logging and native-path source checks passed\n",checks);
}}
'''
Path(sys.argv[2]).write_text(head+function+tail,encoding='utf-8',newline='\n')
print('Actual native log function extracted; timestamp-query guards and completion order checked.')
