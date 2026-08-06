/// Entry point of the XPC service that hosts the FxPlug plug-in.
///
/// FxPlug 4 plug-ins run out of process. This binary is not loaded into Final
/// Cut Pro or Motion; it is launched as a service and handed work over XPC.
/// FxPrincipal is the class named as PrincipalClass in Info.plist, and starting
/// it is the whole of main.

#import <FxPlug/FxPlugSDK.h>

int main( int argc, const char* argv[] )
{
	[FxPrincipal startServicePrincipal];
	return 0;
}
