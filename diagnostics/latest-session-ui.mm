// SPDX-License-Identifier: Apache-2.0
#import "latest-session-ui.h"
#import "latest-session-export.h"
#import <objc/runtime.h>
#include <dlfcn.h>

void RexPresentLatestSessionExport(UIViewController* controller,UIButton* sender,NSURL* logs,NSURL* diagnostics,NSDictionary* options) {
  sender.enabled=NO;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
    NSError* error=nil;
    NSURL* output=RexCreateLatestSessionExport(logs,diagnostics,options,&error);
    dispatch_async(dispatch_get_main_queue(),^{
      sender.enabled=YES;
      if(!output) {
        UIAlertController* alert=[UIAlertController alertControllerWithTitle:@"Log export"
            message:error.localizedDescription ?: @"The latest session could not be exported." preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [controller presentViewController:alert animated:YES completion:nil];return;
      }
      UIActivityViewController* sheet=[[UIActivityViewController alloc] initWithActivityItems:@[output] applicationActivities:nil];
      sheet.popoverPresentationController.sourceView=sender;sheet.popoverPresentationController.sourceRect=sender.bounds;
      [controller presentViewController:sheet animated:YES completion:nil];
    });
  });
}
static NSDictionary* config;
static IMP originalRefresh;
static void ReplaceDescription(UIView* view) {
  if([view isKindOfClass:UILabel.class]) {
    UILabel* label=(UILabel*)view;
    if([label.text hasPrefix:config[@"DescriptionPrefix"]])label.text=config[@"Description"];
  }
  for(UIView* child in view.subviews)ReplaceDescription(child);
}
static void Refresh(id self,SEL cmd) {
  ((void(*)(id,SEL))originalRefresh)(self,cmd);
  ReplaceDescription(((UIViewController*)self).view);
}
static void Export(id self,SEL,UIButton* sender) {
  const char* symbol=[config[@"FlushSymbol"] UTF8String];
  if(symbol)if(auto flush=(void(*)())dlsym(RTLD_DEFAULT,symbol))flush();
  NSURL* docs=[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
  RexPresentLatestSessionExport(self,sender,[docs URLByAppendingPathComponent:config[@"LogsPath"] isDirectory:YES],
      [docs URLByAppendingPathComponent:config[@"DiagnosticsPath"] isDirectory:YES],config);
}
bool RexInstallLatestSessionExport(NSDictionary* options) {
  if(config)return false;
  for(NSString* key in @[@"ControllerClass",@"ExportSelector",@"RefreshSelector",@"LogsPath",@"DiagnosticsPath",@"DescriptionPrefix",@"Description",@"GameplayMarker",@"FilePrefix"])
    if(![options[key] isKindOfClass:NSString.class] || ![options[key] length])return false;
  for(NSString* key in @[@"LogsPath",@"DiagnosticsPath"])
    if([options[key] isAbsolutePath] || [[options[key] pathComponents] containsObject:@".."])return false;
  Class type=NSClassFromString(options[@"ControllerClass"]);
  if(!type || ![type isSubclassOfClass:UIViewController.class])return false;
  Method action=class_getInstanceMethod(type,NSSelectorFromString(options[@"ExportSelector"]));
  Method refresh=class_getInstanceMethod(type,NSSelectorFromString(options[@"RefreshSelector"]));
  if(!action || !refresh || method_getNumberOfArguments(action)!=3 || method_getNumberOfArguments(refresh)!=2)return false;
  char result[8]={},argument[8]={};method_getReturnType(action,result,sizeof(result));method_getArgumentType(action,2,argument,sizeof(argument));
  if(result[0]!='v' || argument[0]!='@')return false;
  method_getReturnType(refresh,result,sizeof(result));if(result[0]!='v')return false;
  config=[options copy];method_setImplementation(action,(IMP)Export);
  originalRefresh=method_setImplementation(refresh,(IMP)Refresh);return true;
}
#ifndef REX_EXPORT_NO_CONSTRUCTOR
__attribute__((constructor)) static void Install() {
  @autoreleasepool {
    NSDictionary* options=NSBundle.mainBundle.infoDictionary[@"RexLatestSessionExport"];
    if(options)dispatch_async(dispatch_get_main_queue(),^{
      if(!RexInstallLatestSessionExport(options))NSLog(@"Latest-session export integration could not be installed.");
    });
  }
}
#endif
