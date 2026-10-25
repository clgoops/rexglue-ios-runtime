#include "REXNativeMetalTiming.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <initializer_list>
static unsigned checks=0;
static void check(bool b){++checks;if(!b){std::fprintf(stderr,"Native window check %u failed\n",checks);std::abort();}}
static bool near(double a,double b){return std::abs(a-b)<1e-6;}
int main(){
  auto w=rexNativeMetalWindow(true,12,12.1,12.3,12.01,12.02,12.4);
  check(w.valid_gpu);check(near(w.gpu_ms,200));check(near(w.pre_gpu_ms,100));
  check(near(w.driver_ms,10));check(near(w.callback_lag_ms,100));
  for(double bad:{0.0,-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
    w=rexNativeMetalWindow(true,12,bad,12.3,0,0,12.4);check(!w.valid_gpu);check(w.gpu_ms==-1);
    w=rexNativeMetalWindow(true,12,12.1,bad,0,0,12.4);check(!w.valid_gpu);check(w.gpu_ms==-1);
  }
  w=rexNativeMetalWindow(false,12,12.1,12.3,12.01,12.02,12.4);check(!w.valid_gpu);check(w.gpu_ms==-1);
  w=rexNativeMetalWindow(true,12.2,12.1,12.3,12.02,12.01,12.2);
  check(w.valid_gpu);check(w.pre_gpu_ms==-1);check(w.driver_ms==-1);check(w.callback_lag_ms==-1);
  w=rexNativeMetalWindow(true,12.1,12.1,12.1,12.1,12.1,12.1);
  check(w.valid_gpu);check(w.gpu_ms==0);check(w.pre_gpu_ms==0);check(w.driver_ms==0);check(w.callback_lag_ms==0);
  std::printf("%u native Metal execution-window arithmetic checks passed\n",checks);
}
