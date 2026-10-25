// SPDX-License-Identifier: Apache-2.0
#import "latest-session-export.h"
#include <cmath>

static bool Fail(NSError** error, NSString* message) {
  if (error) *error = [NSError errorWithDomain:@"RexLatestSessionExport" code:1
      userInfo:@{NSLocalizedDescriptionKey:message}];
  return false;
}
static bool Regular(NSURL* url) {
  NSNumber *regular=nil, *link=nil;
  return [url getResourceValue:&regular forKey:NSURLIsRegularFileKey error:nil] && regular.boolValue &&
      [url getResourceValue:&link forKey:NSURLIsSymbolicLinkKey error:nil] && !link.boolValue;
}
static NSData* Read(NSURL* url, NSUInteger limit, NSError** error) {
  NSNumber* size=nil;
  if (!Regular(url) || ![url getResourceValue:&size forKey:NSURLFileSizeKey error:error]) {
    Fail(error,@"The selected log is not a readable regular file."); return nil;
  }
  if (size.unsignedLongLongValue>limit) {Fail(error,@"The selected diagnostic exceeds the 64 MB export limit.");return nil;}
  NSData* result=[NSData dataWithContentsOfURL:url options:NSDataReadingMappedIfSafe error:error];
  if(result.length>limit) {Fail(error,@"The selected diagnostic grew beyond the export limit.");return nil;}
  return result;
}
static NSString* Prefix(NSURL* url) {
  NSFileHandle* input=[NSFileHandle fileHandleForReadingFromURL:url error:nil];
  NSData* data=[input readDataUpToLength:256*1024 error:nil];[input closeFile];
  // The bounded prefix can end in the middle of a UTF-8 character. Markers and
  // timestamps are ASCII, so preserve their searchability in that case.
  return data ? ([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?:
      [[NSString alloc] initWithData:data encoding:NSISOLatin1StringEncoding]) : nil;
}
static NSDate* PlayDate(NSString* text, NSString* marker) {
  NSDateFormatter* date=[NSDateFormatter new];date.locale=[[NSLocale alloc] initWithLocaleIdentifier:@"en_US_POSIX"];
  date.dateFormat=@"yyyy-MM-dd HH:mm:ss.SSS";date.lenient=NO;
  NSDate* result=nil;
  for(NSString* line in [text componentsSeparatedByString:@"\n"])
    if([line containsString:marker] && [line hasPrefix:@"["] && line.length>=24) {
      NSDate* parsed=[date dateFromString:[line substringWithRange:NSMakeRange(1,23)]];
      if(parsed)result=parsed;
    }
  return result;
}
NSURL* RexCreateLatestSessionExport(NSURL* logs, NSURL* diagnostics, NSDictionary* options, NSError** error) {
  const NSUInteger limit=64*1024*1024;
  NSString* marker=options[@"GameplayMarker"];
  NSString* prefix=options[@"FilePrefix"];
  NSString* capturePrefix=options[@"CapturePrefix"];
  if(!marker.length || !prefix.length || [prefix containsString:@"/"] || [prefix containsString:@"\\"])
    {Fail(error,@"Invalid latest-session export configuration.");return nil;}
  NSFileManager* manager=NSFileManager.defaultManager;
  NSArray<NSURL*>* files=[manager contentsOfDirectoryAtURL:logs
      includingPropertiesForKeys:@[NSURLIsRegularFileKey,NSURLIsSymbolicLinkKey,NSURLContentModificationDateKey]
      options:NSDirectoryEnumerationSkipsHiddenFiles error:error];
  if(!files)return nil;
  files=[files sortedArrayUsingComparator:^NSComparisonResult(NSURL* a,NSURL* b) {
    NSDate *ad=nil,*bd=nil;[a getResourceValue:&ad forKey:NSURLContentModificationDateKey error:nil];
    [b getResourceValue:&bd forKey:NSURLContentModificationDateKey error:nil];
    NSComparisonResult order=[(bd ?: NSDate.distantPast) compare:(ad ?: NSDate.distantPast)];
    return order==NSOrderedSame ? [b.lastPathComponent compare:a.lastPathComponent] : order;
  }];
  NSURL *latest=nil,*selected=nil;NSString* selectedPrefix=nil;
  for(NSURL* file in files) {
    if(![file.pathExtension isEqualToString:@"log"] || !Regular(file))continue;
    if(!latest)latest=file;
    NSString* text=Prefix(file);
    if([text containsString:marker]) {selected=file;selectedPrefix=text;break;}
  }
  if(!selected)selected=latest;
  if(!selected){Fail(error,@"No session log is available yet.");return nil;}
  NSData* log=Read(selected,limit,error);if(!log)return nil;
  NSMutableData* report=[NSMutableData data];
  NSString* header=[NSString stringWithFormat:@"Latest session snapshot\nSource: %@\nEarlier sessions are excluded.\n\n--- SESSION LOG ---\n",selected.lastPathComponent];
  [report appendData:[header dataUsingEncoding:NSUTF8StringEncoding]];[report appendData:log];
  NSDate* play=selectedPrefix ? PlayDate(selectedPrefix,marker) : nil;
  NSURL* capture=nil;double closest=2.0;
  if(play && capturePrefix.length) {
    NSArray<NSURL*>* captures=[manager contentsOfDirectoryAtURL:diagnostics includingPropertiesForKeys:nil options:NSDirectoryEnumerationSkipsHiddenFiles error:nil];
    for(NSURL* file in captures) {
      NSString* name=file.lastPathComponent;
      if(![name hasPrefix:capturePrefix] || ![file.pathExtension isEqualToString:@"csv"] || !Regular(file))continue;
      NSString* number=[[name stringByDeletingPathExtension] substringFromIndex:capturePrefix.length];
      if((number.length!=13 && number.length!=16 && number.length!=19) ||
         [number rangeOfCharacterFromSet:NSCharacterSet.decimalDigitCharacterSet.invertedSet].location!=NSNotFound)continue;
      double divisor=number.length==13?1e3:(number.length==16?1e6:1e9);
      double delta=std::abs(number.doubleValue/divisor-play.timeIntervalSince1970);
      if(delta<closest){closest=delta;capture=file;}
    }
  }
  if(capture) {
    NSData* csv=Read(capture,limit,error);if(!csv)return nil;
    NSString* heading=[NSString stringWithFormat:@"\n\n--- MATCHING PERFORMANCE CSV: %@ ---\n",capture.lastPathComponent];
    [report appendData:[heading dataUsingEncoding:NSUTF8StringEncoding]];[report appendData:csv];
  } else [report appendData:[@"\n\nNo performance capture matched this session. Older captures were excluded.\n" dataUsingEncoding:NSUTF8StringEncoding]];
  if(report.length>limit){Fail(error,@"The session snapshot exceeds the 64 MB export limit.");return nil;}
  if(![manager createDirectoryAtURL:diagnostics withIntermediateDirectories:YES attributes:nil error:error])return nil;
  NSURL* output=[diagnostics URLByAppendingPathComponent:[NSString stringWithFormat:@"%@%@.txt",prefix,NSUUID.UUID.UUIDString]];
  return [report writeToURL:output options:NSDataWritingAtomic error:error] ? output : nil;
}
