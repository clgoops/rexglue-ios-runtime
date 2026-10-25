// SPDX-License-Identifier: Apache-2.0
#import <Foundation/Foundation.h>

// One snapshot of the most recent game session, or latest startup if no game
// session exists. Only a capture whose filename timestamp matches that session's
// Play request is admitted. Source logs/captures are never modified or deleted.
NSURL* RexCreateLatestSessionExport(NSURL* logs, NSURL* diagnostics,
                                  NSDictionary* options, NSError** error);
