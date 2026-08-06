/// The FxPlug 4 build of Asciify, for Final Cut Pro and Motion.
///
/// The alphabet, glyph matching and control curves are in source/, shared with
/// the FFGL and OpenFX builds; the two render phases are in AsciifyTile.h so
/// they can be tested without a host. What is here is the shape FxPlug demands.
///
/// Like Porthole and unlike Luma Key, this reads outside its tile — a cell
/// spans many source pixels — so `kFxPropertyKey_NeedsFullBuffer` is YES and
/// `-sourceTileRect:...` asks for the whole source image.
///
/// One thing is specific to this plugin: the custom alphabet is a string, and
/// the pluginState blob is raw bytes. It travels as a fixed-size char array
/// rather than a std::string, which is why AsciifyState stays a POD.

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <FxPlug/FxPlugSDK.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "AsciifyTile.h"
#include "Presets.h"

enum {
	kAsciifyError_UnsupportedPixelFormat = kFxError_ThirdPartyDeveloperStart + 1,
};

/// Parameter IDs. A saved project refers to a parameter by ID, so changing one
/// silently detaches every existing use of the effect from its value. Never
/// renumber.
enum {
	kParamID_Preset       = 1,
	kParamID_TextGroup    = 2,
	kParamID_Columns      = 3,
	kParamID_Set          = 4,
	kParamID_Custom       = 5,
	kParamID_ToneGroup    = 6,
	kParamID_Structure    = 7,
	kParamID_Tone         = 8,
	kParamID_Contrast     = 9,
	kParamID_Invert       = 10,
	kParamID_Dither       = 11,
	kParamID_ColourGroup  = 12,
	kParamID_Ink          = 13,
	kParamID_Tint         = 14,
	kParamID_Paper        = 15,
	kParamID_PaperOpacity = 16,
	kParamID_OutputGroup  = 17,
	kParamID_Edge         = 18,
	kParamID_Mix          = 19,
};

using fxsurface::Layout;
using asciify::AsciifyState;

namespace {

Layout layoutForSurface( IOSurfaceRef surface )
{
	switch( IOSurfaceGetPixelFormat( surface ) )
	{
	case 'BGRA': return Layout::BGRA8;
	case 'RGBA': return Layout::RGBA8;
	case 'RGhA':
	case 'RGbA': return Layout::RGBAh;
	case 'RGfA':
	case 'RGFA': return Layout::RGBAf;
	default:     return Layout::Unsupported;
	}
}

} // namespace


@interface AsciifyPlugIn : NSObject <FxTileableEffect>
@end

@implementation AsciifyPlugIn
{
	__weak id<PROAPIAccessing> _apiManager;
}

- (nullable instancetype)initWithAPIManager:(id<PROAPIAccessing>)apiManager
{
	self = [super init];
	if( self != nil )
		_apiManager = apiManager;
	return self;
}

- (BOOL)addSlider:(id<FxParameterCreationAPI_v5>)params
			 name:(NSString*)name
		  paramID:(UInt32)paramID
		  default:(double)defaultValue
{
	return [params addFloatSliderWithName:name
							  parameterID:paramID
							 defaultValue:defaultValue
							 parameterMin:0.0
							 parameterMax:1.0
								sliderMin:0.0
								sliderMax:1.0
									delta:0.01
						   parameterFlags:kFxParameterFlag_DEFAULT];
}

- (BOOL)addParametersWithError:(NSError**)error
{
	id<FxParameterCreationAPI_v5> params =
		[_apiManager apiForProtocol:@protocol( FxParameterCreationAPI_v5 )];
	if( params == nil )
	{
		if( error != NULL )
			*error = [NSError errorWithDomain:FxPlugErrorDomain
										 code:kFxError_APIUnavailable
									 userInfo:@{ NSLocalizedDescriptionKey :
												 @"Asciify: no parameter creation API" }];
		return NO;
	}

	NSMutableArray<NSString*>* presetNames = [NSMutableArray arrayWithObject:@"Custom"];
	for( int i = 0; i < asciify::presets::kCount; ++i )
		[presetNames addObject:@( asciify::presets::kPresets[ i ].name )];
	if( ![params addPopupMenuWithName:@"Preset"
						  parameterID:kParamID_Preset
						 defaultValue:0
						  menuEntries:presetNames
					   parameterFlags:kFxParameterFlag_DEFAULT] )
		return NO;

	// ------------------------------------------------------------------ Text
	if( ![params startParameterSubGroup:@"Text"
							parameterID:kParamID_TextGroup
						 parameterFlags:kFxParameterFlag_DEFAULT] )
		return NO;

	// 0.624 is 80 columns on the logarithmic 8..320 curve.
	if( ![self addSlider:params name:@"Columns" paramID:kParamID_Columns default:0.624] )
		return NO;

	NSMutableArray<NSString*>* setNames = [NSMutableArray array];
	for( int i = 0; i < int( asciify::Set::Count ); ++i )
		[setNames addObject:@( asciify::SetName( asciify::Set( i ) ) )];
	if( ![params addPopupMenuWithName:@"Characters"
						  parameterID:kParamID_Set
						 defaultValue:0
						  menuEntries:setNames
					   parameterFlags:kFxParameterFlag_DEFAULT] )
		return NO;

	if( ![params addStringParameterWithName:@"Custom Set"
								parameterID:kParamID_Custom
							   defaultValue:@"@%#*+=-:. "
							 parameterFlags:kFxParameterFlag_DEFAULT] )
		return NO;

	if( ![params endParameterSubGroup] )
		return NO;

	// ------------------------------------------------------------------ Tone
	if( ![params startParameterSubGroup:@"Tone"
							parameterID:kParamID_ToneGroup
						 parameterFlags:kFxParameterFlag_DEFAULT] )
		return NO;

	if( ![self addSlider:params name:@"Structure" paramID:kParamID_Structure default:0.35] )
		return NO;
	if( ![self addSlider:params name:@"Tone" paramID:kParamID_Tone default:0.5] )
		return NO;
	if( ![self addSlider:params name:@"Contrast" paramID:kParamID_Contrast default:0.5] )
		return NO;
	if( ![params addToggleButtonWithName:@"Invert"
							 parameterID:kParamID_Invert
							defaultValue:NO
						  parameterFlags:kFxParameterFlag_DEFAULT] )
		return NO;
	if( ![self addSlider:params name:@"Dither" paramID:kParamID_Dither default:0.5] )
		return NO;

	if( ![params endParameterSubGroup] )
		return NO;

	// ---------------------------------------------------------------- Colour
	if( ![params startParameterSubGroup:@"Colour"
							parameterID:kParamID_ColourGroup
						 parameterFlags:kFxParameterFlag_DEFAULT] )
		return NO;

	if( ![params addColorParameterWithName:@"Ink"
							   parameterID:kParamID_Ink
							  defaultRed:0.60
							defaultGreen:1.00
							 defaultBlue:0.70
						  parameterFlags:kFxParameterFlag_DEFAULT] )
		return NO;
	if( ![self addSlider:params name:@"Tint" paramID:kParamID_Tint default:0.0] )
		return NO;
	if( ![params addColorParameterWithName:@"Paper"
							   parameterID:kParamID_Paper
							  defaultRed:0.02
							defaultGreen:0.05
							 defaultBlue:0.03
						  parameterFlags:kFxParameterFlag_DEFAULT] )
		return NO;
	if( ![self addSlider:params name:@"Paper Opacity" paramID:kParamID_PaperOpacity default:1.0] )
		return NO;

	if( ![params endParameterSubGroup] )
		return NO;

	// ---------------------------------------------------------------- Output
	if( ![params startParameterSubGroup:@"Output"
							parameterID:kParamID_OutputGroup
						 parameterFlags:kFxParameterFlag_DEFAULT] )
		return NO;

	if( ![params addPopupMenuWithName:@"Glyph Edge"
						  parameterID:kParamID_Edge
						 defaultValue:0
						  menuEntries:@[ @"Crisp", @"Smooth" ]
					   parameterFlags:kFxParameterFlag_DEFAULT] )
		return NO;
	if( ![self addSlider:params name:@"Mix" paramID:kParamID_Mix default:1.0] )
		return NO;

	if( ![params endParameterSubGroup] )
		return NO;

	return YES;
}

- (BOOL)properties:(NSDictionary* _Nonnull* _Nullable)properties error:(NSError**)error
{
	// A cell spans many source pixels, so this cannot render from a tile of the
	// source. Frames remain independent of each other and of render order.
	*properties = @{
		kFxPropertyKey_NeedsFullBuffer           : @YES,
		kFxPropertyKey_VariesWhenParamsAreStatic : @NO,
		kFxPropertyKey_ChangesOutputSize         : @NO,
	};
	return YES;
}

- (BOOL)pluginState:(NSData* _Nonnull* _Nullable)pluginState
			 atTime:(CMTime)renderTime
			quality:(FxQuality)qualityLevel
			  error:(NSError**)error
{
	id<FxParameterRetrievalAPI_v6> params =
		[_apiManager apiForProtocol:@protocol( FxParameterRetrievalAPI_v6 )];
	if( params == nil )
	{
		if( error != NULL )
			*error = [NSError errorWithDomain:FxPlugErrorDomain
										 code:kFxError_APIUnavailable
									 userInfo:@{ NSLocalizedDescriptionKey :
												 @"Asciify: no parameter retrieval API" }];
		return NO;
	}

	AsciifyState state = {};

	double columns = 0.624, structure = 0.35, tone = 0.5, contrast = 0.5, dither = 0.5;
	double tint = 0.0, paperOpacity = 1.0, mix = 1.0;
	double inkR = 0.6, inkG = 1.0, inkB = 0.7;
	double paperR = 0.02, paperG = 0.05, paperB = 0.03;
	int setValue = 0, edgeValue = 0;
	BOOL invertValue = NO;

	BOOL ok = YES;
	ok = ok && [params getFloatValue:&columns      fromParameter:kParamID_Columns      atTime:renderTime];
	ok = ok && [params getIntValue:&setValue       fromParameter:kParamID_Set          atTime:renderTime];
	ok = ok && [params getFloatValue:&structure    fromParameter:kParamID_Structure    atTime:renderTime];
	ok = ok && [params getFloatValue:&tone         fromParameter:kParamID_Tone         atTime:renderTime];
	ok = ok && [params getFloatValue:&contrast     fromParameter:kParamID_Contrast     atTime:renderTime];
	ok = ok && [params getBoolValue:&invertValue   fromParameter:kParamID_Invert       atTime:renderTime];
	ok = ok && [params getFloatValue:&dither       fromParameter:kParamID_Dither       atTime:renderTime];
	ok = ok && [params getRedValue:&inkR greenValue:&inkG blueValue:&inkB
					 fromParameter:kParamID_Ink atTime:renderTime];
	ok = ok && [params getFloatValue:&tint         fromParameter:kParamID_Tint         atTime:renderTime];
	ok = ok && [params getRedValue:&paperR greenValue:&paperG blueValue:&paperB
					 fromParameter:kParamID_Paper atTime:renderTime];
	ok = ok && [params getFloatValue:&paperOpacity fromParameter:kParamID_PaperOpacity atTime:renderTime];
	ok = ok && [params getIntValue:&edgeValue      fromParameter:kParamID_Edge         atTime:renderTime];
	ok = ok && [params getFloatValue:&mix          fromParameter:kParamID_Mix          atTime:renderTime];

	NSString* custom = nil;
	ok = ok && [params getStringParameterValue:&custom fromParameter:kParamID_Custom];

	if( !ok )
	{
		if( error != NULL )
			*error = [NSError errorWithDomain:FxPlugErrorDomain
										 code:kFxError_InvalidParameter
									 userInfo:@{ NSLocalizedDescriptionKey :
												 @"Asciify: could not read parameters" }];
		return NO;
	}

	state.columns      = float( columns );
	state.structure    = float( structure );
	state.tone         = float( tone );
	state.contrast     = float( contrast );
	state.dither       = float( dither );
	state.tint         = float( tint );
	state.inkR         = float( inkR );
	state.inkG         = float( inkG );
	state.inkB         = float( inkB );
	state.paperR       = float( paperR );
	state.paperG       = float( paperG );
	state.paperB       = float( paperB );
	state.paperOpacity = float( paperOpacity );
	state.mix          = float( mix );
	state.set          = setValue;
	state.smoothEdge   = edgeValue > 0 ? 1 : 0;
	state.invert       = invertValue ? 1u : 0u;

	// Truncated rather than refused: an alphabet longer than this is not a
	// usable alphabet, and the blob has to stay fixed size.
	const char* utf8 = custom.UTF8String ?: "";
	std::strncpy( state.customSet, utf8, asciify::kMaxCustomSet - 1 );
	state.customSet[ asciify::kMaxCustomSet - 1 ] = '\0';

	*pluginState = [NSData dataWithBytes:&state length:sizeof( state )];
	return YES;
}

- (BOOL)destinationImageRect:(FxRect*)destinationImageRect
				sourceImages:(NSArray<FxImageTile*>*)sourceImages
			destinationImage:(FxImageTile*)destinationImage
				 pluginState:(nullable NSData*)pluginState
					  atTime:(CMTime)renderTime
					   error:(NSError**)outError
{
	if( sourceImages.count > 0 )
		*destinationImageRect = sourceImages[ 0 ].imagePixelBounds;
	else
		*destinationImageRect = destinationImage.imagePixelBounds;
	return YES;
}

- (BOOL)sourceTileRect:(FxRect*)sourceTileRect
	  sourceImageIndex:(NSUInteger)sourceImageIndex
		  sourceImages:(NSArray<FxImageTile*>*)sourceImages
   destinationTileRect:(FxRect)destinationTileRect
	  destinationImage:(FxImageTile*)destinationImage
		   pluginState:(nullable NSData*)pluginState
				atTime:(CMTime)renderTime
				 error:(NSError**)outError
{
	// The whole picture: a cell straddles any tile boundary you care to draw.
	if( sourceImages.count > sourceImageIndex )
		*sourceTileRect = sourceImages[ sourceImageIndex ].imagePixelBounds;
	else
		*sourceTileRect = destinationTileRect;
	return YES;
}

- (BOOL)renderDestinationImage:(FxImageTile*)destinationImage
				  sourceImages:(NSArray<FxImageTile*>*)sourceImages
				   pluginState:(nullable NSData*)pluginState
						atTime:(CMTime)renderTime
						 error:(NSError**)outError
{
	if( pluginState == nil || pluginState.length != sizeof( AsciifyState ) )
	{
		if( outError != NULL )
			*outError = [NSError errorWithDomain:FxPlugErrorDomain
											code:kFxError_InvalidParameter
										userInfo:@{ NSLocalizedDescriptionKey :
													@"Asciify: bad plug-in state" }];
		return NO;
	}

	AsciifyState state;
	[pluginState getBytes:&state length:sizeof( state )];

	if( sourceImages.count < 1 )
	{
		if( outError != NULL )
			*outError = [NSError errorWithDomain:FxPlugErrorDomain
											code:kFxError_InvalidParameter
										userInfo:@{ NSLocalizedDescriptionKey :
													@"Asciify: no source image" }];
		return NO;
	}

	FxImageTile* source = sourceImages[ 0 ];

	IOSurfaceRef srcSurface = (__bridge IOSurfaceRef)source.ioSurface;
	IOSurfaceRef dstSurface = (__bridge IOSurfaceRef)destinationImage.ioSurface;
	if( srcSurface == NULL || dstSurface == NULL )
	{
		if( outError != NULL )
			*outError = [NSError errorWithDomain:FxPlugErrorDomain
											code:kFxError_InvalidParameter
										userInfo:@{ NSLocalizedDescriptionKey :
													@"Asciify: image tile carried no surface" }];
		return NO;
	}

	const Layout srcLayout = layoutForSurface( srcSurface );
	const Layout dstLayout = layoutForSurface( dstSurface );
	if( srcLayout == Layout::Unsupported || dstLayout == Layout::Unsupported )
	{
		if( outError != NULL )
			*outError = [NSError errorWithDomain:FxPlugErrorDomain
											code:kAsciifyError_UnsupportedPixelFormat
										userInfo:@{ NSLocalizedDescriptionKey :
													@"Asciify: unsupported pixel format" }];
		return NO;
	}

	IOSurfaceLock( srcSurface, kIOSurfaceLockReadOnly, NULL );
	IOSurfaceLock( dstSurface, 0, NULL );

	const FxRect srcBounds = source.tilePixelBounds;
	const FxRect dstTile   = destinationImage.tilePixelBounds;
	const FxRect dstImage  = destinationImage.imagePixelBounds;

	const asciify::SourceImage src(
		static_cast<const uint8_t*>( IOSurfaceGetBaseAddress( srcSurface ) ),
		IOSurfaceGetBytesPerRow( srcSurface ), srcLayout,
		int( srcBounds.right - srcBounds.left ),
		int( srcBounds.top - srcBounds.bottom ) );

	// Phase one over the whole picture, phase two into this tile. The grid is
	// rebuilt per render rather than cached in pluginState: it depends on the
	// source pixels, which pluginState never sees.
	const asciify::CellGrid grid = asciify::buildCells( src, state );

	asciify::typeTile( src, grid,
					   static_cast<uint8_t*>( IOSurfaceGetBaseAddress( dstSurface ) ),
					   IOSurfaceGetBytesPerRow( dstSurface ), dstLayout,
					   int( dstTile.left - dstImage.left ),
					   int( dstTile.bottom - dstImage.bottom ),
					   int( dstTile.right - dstTile.left ),
					   int( dstTile.top - dstTile.bottom ),
					   int( dstImage.right - dstImage.left ),
					   int( dstImage.top - dstImage.bottom ),
					   state );

	IOSurfaceUnlock( dstSurface, 0, NULL );
	IOSurfaceUnlock( srcSurface, kIOSurfaceLockReadOnly, NULL );

	return YES;
}

@end
