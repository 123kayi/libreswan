//
//  Controller.h
//  Openswan
//
//  Created by Jose Quaresma on 26/5/09.
//  Copyright 2009 __MyCompanyName__. All rights reserved.
//

#import <Cocoa/Cocoa.h>
#import "Connection.h"

@interface Controller : NSObject {
	NSMutableArray* connections;
	NSArray* Type;
	NSArray* Auto;
	NSArray* phase2;
	NSArray* leftSendCert; 
	NSArray* rightSendCert; 
	NSArray* dpdAction;
	NSArray* plutoDebug;
	NSArray* authBy;
	NSArray* endUserOpts;
	
	IBOutlet NSWindow* window;
	IBOutlet NSButton* forceEncaps;
	IBOutlet NSPopUpButton* authByButton;
	IBOutlet NSPopUpButton* userOpts;
	IBOutlet NSTextField* rawRSAText;
	IBOutlet NSView* PSKView;
	IBOutlet NSView* X509View;
	IBOutlet NSView* rawRSAView;
}

@property (readwrite, copy) NSMutableArray* connections;
@property (readwrite, copy) NSArray *Type, *Auto, *phase2, *leftSendCert, *rightSendCert, *dpdAction, *plutoDebug, *authBy, *endUserOpts;

- (id)init;

- (IBAction)advancedOpt: (id) sender;
- (IBAction)natTraversal: (id) sender;
- (IBAction)authByAction: (id) sender;
- (IBAction)selectedEndUserOpt: (id)sender;

@end
