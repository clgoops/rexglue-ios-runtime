// SPDX-License-Identifier: Apache-2.0
#import "../diagnostics/latest-session-export.h"
#include <cstdio>
#include <stdexcept>
#include <string>
int main(){@autoreleasepool {
  int checks=0;
  auto require=[&](bool ok){++checks;if(!ok)throw std::runtime_error("Latest-session check "+std::to_string(checks));};
  NSFileManager* manager=NSFileManager.defaultManager;
  NSURL* root=[NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString] isDirectory:YES];
  NSURL* logs=[root URLByAppendingPathComponent:@"logs" isDirectory:YES];
  NSURL* diagnostics=[root URLByAppendingPathComponent:@"Diagnostics" isDirectory:YES];
  [manager createDirectoryAtURL:logs withIntermediateDirectories:YES attributes:nil error:nil];
  [manager createDirectoryAtURL:diagnostics withIntermediateDirectories:YES attributes:nil error:nil];
  NSTimeZone.defaultTimeZone=[NSTimeZone timeZoneForSecondsFromGMT:0];
  NSDictionary* options=@{@"GameplayMarker":@"Play requested",@"FilePrefix":@"Session-",@"CapturePrefix":@"Performance-"};
  auto write=[&](NSURL* directory,NSString* name,NSString* contents,double modified){
    NSURL* path=[directory URLByAppendingPathComponent:name];
    require([contents writeToURL:path atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    require([manager setAttributes:@{NSFileModificationDate:[NSDate dateWithTimeIntervalSince1970:modified]} ofItemAtPath:path.path error:nil]);return path;
  };
  NSError* error=nil;
  require(RexCreateLatestSessionExport(logs,diagnostics,options,&error)==nil && error!=nil);
  NSURL* old=write(logs,@"000.log",@"[2026-10-25 12:00:00.000] Play requested\nOLD SESSION\n",1);
  NSURL* game=write(logs,@"001.log",@"[2026-10-25 13:00:00.000] Play requested\nLATEST GAME\n",2);
  NSURL* launcher=write(logs,@"002.log",@"[2026-10-25 13:10:00.000] Startup only\nLATEST STARTUP\n",3);
  write(logs,@"unrelated.txt",@"Play requested\nUNRELATED FILE\n",10);
  require([manager createSymbolicLinkAtPath:[[logs URLByAppendingPathComponent:@"linked.log"] path] withDestinationPath:old.path error:nil]);
  NSDateFormatter* dates=[NSDateFormatter new];dates.locale=[[NSLocale alloc] initWithLocaleIdentifier:@"en_US_POSIX"];dates.dateFormat=@"yyyy-MM-dd HH:mm:ss.SSS";
  long long micro=(long long)([dates dateFromString:@"2026-10-25 13:00:00.000"].timeIntervalSince1970*1e6);
  NSURL* matching=write(diagnostics,[NSString stringWithFormat:@"Performance-%lld.csv",micro+500000],@"seconds,game_fps\n1,7\nMATCHED CAPTURE\n",4);
  write(diagnostics,[NSString stringWithFormat:@"Performance-%lld.csv",micro-3600000000LL],@"OLD CAPTURE\n",5);
  write(diagnostics,[NSString stringWithFormat:@"Performance-%lld.csv",micro+86400000000LL],@"FUTURE CAPTURE\n",6);
  write(diagnostics,@"Performance-garbage.csv",@"MALFORMED CAPTURE\n",7);
  auto content=[&](NSURL* url){return [NSString stringWithContentsOfURL:url encoding:NSUTF8StringEncoding error:nil];};
  NSURL* output=RexCreateLatestSessionExport(logs,diagnostics,options,&error);
  require(output!=nil);NSString* text=content(output);
  require([text containsString:@"LATEST GAME"] && ![text containsString:@"OLD SESSION"] && ![text containsString:@"LATEST STARTUP"]);
  require([text containsString:@"MATCHED CAPTURE"] && ![text containsString:@"OLD CAPTURE"] && ![text containsString:@"FUTURE CAPTURE"] && ![text containsString:@"MALFORMED CAPTURE"]);
  require([manager fileExistsAtPath:old.path] && [manager fileExistsAtPath:game.path] && [manager fileExistsAtPath:matching.path]);
  NSURL* again=RexCreateLatestSessionExport(logs,diagnostics,options,&error);
  require(again && ![again isEqual:output] && [content(again) isEqualToString:text]);
  require([manager removeItemAtURL:matching error:nil]);
  text=content(RexCreateLatestSessionExport(logs,diagnostics,options,&error));
  require([text containsString:@"No performance capture matched"] && ![text containsString:@"OLD CAPTURE"]);
  // A large selected file must fail visibly, not silently export an older run.
  NSFileHandle* big=[NSFileHandle fileHandleForWritingToURL:game error:nil];[big truncateFileAtOffset:64*1024*1024+1];[big closeFile];
  require(RexCreateLatestSessionExport(logs,diagnostics,options,&error)==nil && error!=nil);
  require([manager removeItemAtURL:game error:nil]);require([manager removeItemAtURL:old error:nil]);
  text=content(RexCreateLatestSessionExport(logs,diagnostics,options,&error));
  require([text containsString:@"LATEST STARTUP"] && ![text containsString:@"OLD CAPTURE"]);
  require(RexCreateLatestSessionExport(logs,diagnostics,@{@"GameplayMarker":@"Play requested",@"FilePrefix":@"../bad"},&error)==nil);
  require([manager removeItemAtURL:root error:nil]);
  std::printf("%d latest-session export checks passed\n",checks);
}}
