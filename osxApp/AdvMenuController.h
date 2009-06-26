//
//  AdvMenuController.h
//  Openswan
//
//  Created by Jose Quaresma on 11/6/09.
//  Copyright 2009 __MyCompanyName__. All rights reserved.
//

#import <Cocoa/Cocoa.h>
#import "Connection.h"
#import "ConnectionsDB.h"

@class PreferenceController;

@interface AdvMenuController : NSWindowController {
	NSMutableArray* connections;
	
	IBOutlet NSTextField* rawRSAText;
	IBOutlet NSView* PSKView;
	IBOutlet NSView* X509View;
	IBOutlet NSView* rawRSAView;
	IBOutlet NSView* natView;
	IBOutlet NSView* oeView;
	
	IBOutlet NSPopUpButton* selConn;
}

@property (readwrite, retain) NSMutableArray* connections;
@property (readwrite, retain) NSPopUpButton* selConn;

- (IBAction)advancedOpt: (id) sender;
- (IBAction)selectedEndUserOpt: (id)sender;
- (IBAction)natTraversal: (id) sender;
- (IBAction)oe: (id) sender;
- (IBAction)save: (id)sender;

@end
