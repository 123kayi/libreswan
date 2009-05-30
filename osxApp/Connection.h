//
//  Connection.h
//  Openswan
//
//  Created by Jose Quaresma on 26/5/09.
//  Copyright 2009 __MyCompanyName__. All rights reserved.
//

#import <Cocoa/Cocoa.h>


@interface Connection : NSObject {

	NSString* connName;
	NSMutableString* selectedLeftIP;
	NSString* selectedRightIP;
	NSString* selectedKeySetupMode;
	NSString* selectedKey;
	NSString* selectedType;
	NSString* selectedAuto;
	NSString* selectedLeftRSAsig;
	NSString* selectedRightRSAsig;
}

@property (readwrite, copy) NSMutableString *selectedLeftIP;
@property (readwrite, copy) NSString *selectedRightIP, *selectedKeySetupMode, *connName;
@property (readwrite, copy) NSString *selectedKey, *selectedType, *selectedAuto;
@property (readwrite, copy) NSString *selectedLeftRSAsig , *selectedRightRSAsig;

- (id) initWithName:(NSString*)name;
@end
