// SPDX-License-Identifier: Apache-2.0
#import "../diagnostics/latest-session-ui.h"
#import <objc/runtime.h>
static NSArray* shared;
static int checks,failures,oldExports;
static IMP originalShare;
static void Check(bool ok){++checks;if(!ok)++failures;}
static id CaptureShare(id self,SEL cmd,NSArray* items,NSArray* activities){
  shared=[items copy];return ((id(*)(id,SEL,id,id))originalShare)(self,cmd,items,activities);
}
@interface ExportTestController:UIViewController
- (void)exportLog:(UIButton*)sender;
- (void)showPage;
@end
@implementation ExportTestController
- (void)exportLog:(UIButton*)sender {++oldExports;}
- (void)showPage {
  UILabel* label=[UILabel new];label.text=@"Export includes all old sessions";[self.view addSubview:label];
}
@end
static void Report(){
  NSURL* docs=[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
  NSData* json=[NSJSONSerialization dataWithJSONObject:@{@"passed":@(failures==0),@"checks":@(checks),@"failures":@(failures)} options:NSJSONWritingPrettyPrinted error:nil];
  [json writeToURL:[docs URLByAppendingPathComponent:@"latest-export-result.json"] atomically:YES];
}
static void Await(UIButton* button,int remaining){
  if(!shared && remaining>0){dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC/10),dispatch_get_main_queue(),^{Await(button,remaining-1);});return;}
  Check(shared.count==1);Check(oldExports==0);Check(button.enabled);
  if(shared.count==1){
    NSURL* url=shared.firstObject;Check([url isKindOfClass:NSURL.class]);
    NSString* text=[NSString stringWithContentsOfURL:url encoding:NSUTF8StringEncoding error:nil];
    Check([text containsString:@"NEW GAME"]);Check(![text containsString:@"OLD GAME"]);Check(![text containsString:@"STARTUP ONLY"]);
  }
  Report();
}
@interface ExportTestDelegate:UIResponder<UIApplicationDelegate>
@property(nonatomic,strong) UIWindow* window;
@end
@implementation ExportTestDelegate
- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
  self.window=[[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
  ExportTestController* controller=[ExportTestController new];self.window.rootViewController=controller;[self.window makeKeyAndVisible];
  NSFileManager* manager=NSFileManager.defaultManager;
  NSURL* docs=[manager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
  NSURL* logs=[docs URLByAppendingPathComponent:@"TestLogs" isDirectory:YES];[manager createDirectoryAtURL:logs withIntermediateDirectories:YES attributes:nil error:nil];
  NSArray* names=@[@"old.log",@"game.log",@"startup.log"];
  NSArray* texts=@[@"[2026-10-25 12:00:00.000] Play requested\nOLD GAME",@"[2026-10-25 13:00:00.000] Play requested\nNEW GAME",@"STARTUP ONLY"];
  for(NSUInteger i=0;i<names.count;++i){NSURL* path=[logs URLByAppendingPathComponent:names[i]];[texts[i] writeToURL:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
    [manager setAttributes:@{NSFileModificationDate:[NSDate dateWithTimeIntervalSince1970:i+1]} ofItemAtPath:path.path error:nil];}
  Check(!RexInstallLatestSessionExport(@{}));
  NSDictionary* configuration=@{@"ControllerClass":@"ExportTestController",@"ExportSelector":@"exportLog:",@"RefreshSelector":@"showPage",@"LogsPath":@"TestLogs",@"DiagnosticsPath":@"Diagnostics",@"DescriptionPrefix":@"Export includes",@"Description":@"Only the latest session",@"GameplayMarker":@"Play requested",@"FilePrefix":@"Session-"};
  Check(RexInstallLatestSessionExport(configuration));Check(!RexInstallLatestSessionExport(configuration));
  [controller showPage];Check([((UILabel*)controller.view.subviews.firstObject).text isEqualToString:@"Only the latest session"]);
  Method method=class_getInstanceMethod(UIActivityViewController.class,@selector(initWithActivityItems:applicationActivities:));originalShare=method_setImplementation(method,(IMP)CaptureShare);
  UIButton* button=[UIButton buttonWithType:UIButtonTypeSystem];button.frame=CGRectMake(20,20,160,40);[controller.view addSubview:button];
  [button addTarget:controller action:@selector(exportLog:) forControlEvents:UIControlEventTouchUpInside];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_main_queue(),^{[button sendActionsForControlEvents:UIControlEventTouchUpInside];Check(!button.enabled);Await(button,150);});
  return YES;
}
@end
int main(int argc,char** argv){@autoreleasepool{return UIApplicationMain(argc,argv,nil,NSStringFromClass(ExportTestDelegate.class));}}
