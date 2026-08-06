/// The wrapper application.
///
/// This does nothing and is meant to. An FxPlug 4 plug-in is an XPC service,
/// and a service is only discoverable if it is embedded in an application the
/// system has registered. So the deliverable is an .app the user drags to
/// /Applications; its only job is to exist there and carry Contents/PlugIns.
///
/// Running it once is a REQUIRED install step, not a courtesy. Copying the app
/// into /Applications does not register the service on its own - measured:
/// `pluginkit -m -p FxPlug` does not list it until either the app is launched or
/// `pluginkit -a` is run by hand. Launching is what the user can be told to do,
/// so the alert below both explains the app and is the moment registration
/// happens.

#import <Cocoa/Cocoa.h>

@interface StoatworksWrapperDelegate : NSObject <NSApplicationDelegate>
@end

@implementation StoatworksWrapperDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification
{
	NSAlert* alert = [[NSAlert alloc] init];
	alert.messageText = @"Asciify is ready";
	alert.informativeText =
		@"This application carries the Asciify effect for Final Cut Pro and Motion. "
		 "Opening it just now is what registered the effect.\n\n"
		 "Keep the app in your Applications folder — deleting it removes the effect. "
		 "The effect appears under Stoatworks in the Effects browser, and you do not "
		 "need to open this app again.";
	[alert addButtonWithTitle:@"OK"];
	[alert runModal];

	[NSApp terminate:nil];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app
{
	return YES;
}

@end

int main( int argc, const char* argv[] )
{
	@autoreleasepool
	{
		NSApplication* app = [NSApplication sharedApplication];
		StoatworksWrapperDelegate* delegate = [[StoatworksWrapperDelegate alloc] init];
		app.delegate = delegate;
		app.activationPolicy = NSApplicationActivationPolicyRegular;
		[app run];
	}
	return 0;
}
