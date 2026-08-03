#include "Asciify.h"

#include "Controls.h"
#include "Diag.h"
#include "Font.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name. The symptom without it is an
//unknown-type error on ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace asciify;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Asciify >,                       // Create method
	"AS01",                                         // Plugin unique ID of maximum length 4.
	"Asciify",                                      // Plugin name
	2,                                              // API major version number
	1,                                              // API minor version number
	0,                                              // Plugin major version number
	1,                                              // Plugin minor version number
	FF_EFFECT,                                      // Plugin type
	"Renders the clip as ASCII art",                // Plugin description
	"Asciify FFGL effect"                           // About
);

namespace
{
/// The most characters an alphabet can hold, which is every glyph the font
/// draws. The moment texture is allocated at this width once and never resized.
constexpr int kMaxAlphabet = kAtlasCols * kAtlasRows;

/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be the
/// thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

/// Mip level whose texels are `pixels` source pixels across. Zero when the
/// footprint is a pixel or smaller, which is the supersampled case: there is
/// nothing above level 0 to fetch and the bilinear tap is already right.
float LodForFootprint( float pixels )
{
	return pixels <= 1.0f ? 0.0f : std::log2( pixels );
}
} // namespace

Asciify::Asciify()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfof reads each one back out of GetFloatParameter,
	// so these assignments are what the host is told the defaults are.
	//
	// They add up to a green-on-black terminal at eighty columns, because an
	// effect that needs three sliders found before it does anything
	// recognisable is an effect nobody keeps. The null is Mix at zero.
	//---------------------------------------------------------------------
	params[ PT_COLUMNS ]   = 0.624f;//80 columns. See ColumnsFromParam.
	params[ PT_SET ]       = static_cast< float >( asciify::Set::Ascii );
	params[ PT_CUSTOM ]    = 0.0f;//a text parameter; the value lives in customText
	params[ PT_STRUCTURE ] = 0.35f;
	params[ PT_TONE ]      = 0.5f;//gamma 1
	params[ PT_CONTRAST ]  = 0.5f;//contrast 1
	params[ PT_INVERT ]    = 0.0f;
	params[ PT_DITHER ]    = 0.5f;

	params[ PT_TINT ]          = 0.0f;//the ink colour, not the picture's
	params[ PT_INK_R ]         = 0.60f;
	params[ PT_INK_G ]         = 1.00f;
	params[ PT_INK_B ]         = 0.70f;
	params[ PT_PAPER_R ]       = 0.02f;
	params[ PT_PAPER_G ]       = 0.05f;
	params[ PT_PAPER_B ]       = 0.03f;
	params[ PT_PAPER_OPACITY ] = 1.0f;

	params[ PT_EDGE ] = 0.0f;//crisp
	params[ PT_MIX ]  = 1.0f;

	params[ PT_PRESET ] = 0.0f;//Custom: the sliders are the truth

	customText = "@%#*+=-:. ";

	//---------------------------------------------------------------------
	// Declaration.
	//
	// Every numeric parameter is a plain 0..1 float even where it stands for a
	// column count or a gamma. SetParamInfo clamps an FF_TYPE_STANDARD default
	// into 0..1 *before* a range can be attached (SDK b1afaf9), so a parameter
	// declared in columns cannot declare a default in columns. The conversions
	// live in Controls.cpp.
	//---------------------------------------------------------------------
	SetParamInfof( PT_COLUMNS, "Columns", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_SET, "Characters", static_cast< int >( asciify::Set::Count ), params[ PT_SET ] );
	for( int i = 0; i < static_cast< int >( asciify::Set::Count ); ++i )
		SetParamElementInfo( PT_SET, i, SetName( static_cast< asciify::Set >( i ) ), static_cast< float >( i ) );

	//Only read when Characters is set to Custom. Left visible in the other
	//modes on purpose -- a field that appears and disappears as a neighbouring
	//control moves reads as a glitch, and this way what you typed is still
	//there when you come back to it.
	SetParamInfo( PT_CUSTOM, "Custom Set", FF_TYPE_TEXT, customText.c_str() );

	SetParamInfof( PT_STRUCTURE, "Structure", FF_TYPE_STANDARD );
	SetParamInfof( PT_TONE, "Tone", FF_TYPE_STANDARD );
	SetParamInfof( PT_CONTRAST, "Contrast", FF_TYPE_STANDARD );
	SetParamInfof( PT_INVERT, "Invert", FF_TYPE_BOOLEAN );
	SetParamInfof( PT_DITHER, "Dither", FF_TYPE_STANDARD );

	SetParamInfof( PT_TINT, "Tint", FF_TYPE_STANDARD );

	//Consecutive red/green/blue parameters are what a host needs to show a
	//colour swatch instead of three sliders, so the naming follows the SDK's
	//own convention rather than being tidied up.
	SetParamInfof( PT_INK_R, "Ink", FF_TYPE_RED );
	SetParamInfof( PT_INK_G, "Ink_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_INK_B, "Ink_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_PAPER_R, "Paper", FF_TYPE_RED );
	SetParamInfof( PT_PAPER_G, "Paper_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_PAPER_B, "Paper_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_PAPER_OPACITY, "Paper Opacity", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_EDGE, "Glyph Edge", 2, params[ PT_EDGE ] );
	SetParamElementInfo( PT_EDGE, 0, "Crisp", 0.0f );
	SetParamElementInfo( PT_EDGE, 1, "Smooth", 1.0f );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	// Factory presets. Element 0 is Custom; picking anything else copies that
	// preset's values into the covered parameters and raises value events so
	// the host re-reads the sliders. Editing a covered slider flips back to
	// Custom.
	SetOptionParamInfo( PT_PRESET, "Preset", 1 + asciify::presets::kCount, params[ PT_PRESET ] );
	SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
	for( int i = 0; i < asciify::presets::kCount; ++i )
		SetParamElementInfo( PT_PRESET, 1 + i, asciify::presets::kPresets[ i ].name, static_cast< float >( 1 + i ) );

	//Eighteen parameters is well past the point where an ungrouped list in
	//somebody else's inspector stops being readable.
	SetParamGroup( PT_COLUMNS, "Type" );
	SetParamGroup( PT_SET, "Type" );
	SetParamGroup( PT_CUSTOM, "Type" );
	SetParamGroup( PT_STRUCTURE, "Type" );
	SetParamGroup( PT_TONE, "Type" );
	SetParamGroup( PT_CONTRAST, "Type" );
	SetParamGroup( PT_INVERT, "Type" );
	SetParamGroup( PT_DITHER, "Type" );

	SetParamGroup( PT_TINT, "Colour" );
	SetParamGroup( PT_INK_R, "Colour" );
	SetParamGroup( PT_INK_G, "Colour" );
	SetParamGroup( PT_INK_B, "Colour" );
	SetParamGroup( PT_PAPER_R, "Colour" );
	SetParamGroup( PT_PAPER_G, "Colour" );
	SetParamGroup( PT_PAPER_B, "Colour" );
	SetParamGroup( PT_PAPER_OPACITY, "Colour" );

	SetParamGroup( PT_EDGE, "Output" );
	SetParamGroup( PT_MIX, "Output" );

	SetParamGroup( PT_PRESET, "Preset" );

	FFGLLog::LogToHost( "Created Asciify effect" );

	diag::init();
}

//---------------------------------------------------------------------------
// The alphabet.
//---------------------------------------------------------------------------
void Asciify::RebuildAlphabet()
{
	const asciify::Set set = static_cast< asciify::Set >( static_cast< int >( params[ PT_SET ] + 0.5f ) );

	std::string custom;
	{
		std::lock_guard< std::mutex > lock( textMutex );
		custom = customText;
	}

	slots = SlotsFor( set, custom );
	if( static_cast< int >( slots.size() ) > kMaxAlphabet )
		slots.resize( kMaxAlphabet );

	const std::vector< Glyph >& glyphs = Glyphs();

	alphabet.clear();
	alphabet.reserve( slots.size() );
	for( int slot : slots )
		alphabet.push_back( MeasureGlyph( glyphs[ static_cast< size_t >( slot ) ] ) );

	CoverageRange( alphabet, coverLow, coverHigh );

	//Two rows of RGBA: the six measured numbers plus the atlas slot, which is
	//what the type pass will address. The alphabet's *position* is never
	//written anywhere -- the cell pass converts it here and the type pass never
	//learns which characters were in play.
	std::vector< float > texels( static_cast< size_t >( kMaxAlphabet ) * 2 * 4, 0.0f );
	for( size_t i = 0; i < alphabet.size(); ++i )
	{
		const Moments& m = alphabet[ i ];

		float* rowA = &texels[ i * 4 ];
		rowA[ 0 ]   = m.coverage;
		rowA[ 1 ]   = m.shape[ 0 ];
		rowA[ 2 ]   = m.shape[ 1 ];
		rowA[ 3 ]   = m.shape[ 2 ];

		float* rowB = &texels[ ( static_cast< size_t >( kMaxAlphabet ) + i ) * 4 ];
		rowB[ 0 ]   = m.shape[ 3 ];
		rowB[ 1 ]   = m.shape[ 4 ];
		rowB[ 2 ]   = static_cast< float >( slots[ i ] );
		rowB[ 3 ]   = 0.0f;
	}

	//Only clear the flag if the upload actually happened. Called before InitGL
	//has made the texture -- or after DeInitGL has destroyed it -- this would
	//otherwise mark a measurement as uploaded that the GPU has never seen, and
	//the effect would render every cell as the first character of the alphabet
	//with nothing anywhere saying why.
	if( glyphTexture == 0 )
		return;

	{
		Scoped2DTextureBinding binding( glyphTexture );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, kMaxAlphabet, 2, 0, GL_RGBA, GL_FLOAT, texels.data() );
	}

	alphabetDirty = false;
}

bool Asciify::UploadAtlas()
{
	const std::vector< uint8_t > image = BuildAtlasImage();

	glGenTextures( 1, &atlasTexture );
	if( atlasTexture == 0 )
		return false;

	Scoped2DTextureBinding binding( atlasTexture );

	//One byte per texel and a width that is not a multiple of four in every
	//possible future layout, so say so rather than relying on it.
	GLint previousAlignment = 4;
	glGetIntegerv( GL_UNPACK_ALIGNMENT, &previousAlignment );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );

	glTexImage2D( GL_TEXTURE_2D, 0, GL_R8,
	              kAtlasCols * kSlotSize, kAtlasRows * kSlotSize, 0,
	              GL_RED, GL_UNSIGNED_BYTE, image.data() );

	glPixelStorei( GL_UNPACK_ALIGNMENT, previousAlignment );

	//Filters are set per frame from the Glyph Edge control. Wrapping is not:
	//clamping is the only correct answer, and a repeat here would let the last
	//column of the atlas bleed into the first.
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );

	return true;
}

//---------------------------------------------------------------------------
FFResult Asciify::InitGL( const FFGLViewportStruct* vp )
{
	// The GL strings first, and unconditionally: when a shader will not compile
	// it is almost always the driver or the GL version, and knowing which
	// machine reported what is most of the diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &copyShader, kCopyShader, "copy" },
		{ &cellShader, kCellShader, "cell" },
		{ &typeShader, kTypeShader, "type" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( kVertexShader, stage.fragment ) )
			continue;

		// Returning FF_FAIL here is invisible to the operator: the effect
		// simply does nothing in Resolume, with no message anywhere. These two
		// lines are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name
		             + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Asciify: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Asciify: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !UploadAtlas() )
	{
		diag::error( "could not upload the font atlas" );
		FFGLLog::LogToHost( "Asciify: could not upload the font atlas" );
		DeInitGL();
		return FF_FAIL;
	}

	glGenTextures( 1, &glyphTexture );
	if( glyphTexture == 0 )
	{
		diag::error( "could not allocate the alphabet texture" );
		DeInitGL();
		return FF_FAIL;
	}
	{
		Scoped2DTextureBinding binding( glyphTexture );
		//Measured numbers, addressed by texelFetch. Nothing may be interpolated
		//and nothing may wrap.
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	}

	RebuildAlphabet();

	diag::info( "initialised, " + std::to_string( Glyphs().size() ) + " glyphs in the font" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

FFResult Asciify::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	if( alphabetDirty )
		RebuildAlphabet();

	//The host's viewport, not the one InitGL was handed: Resolume changes
	//composition resolution without reinitialising the plugin.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );
	const float outputWidth  = std::max( 1.0f, static_cast< float >( hostViewport[ 2 ] ) );
	const float outputHeight = std::max( 1.0f, static_cast< float >( hostViewport[ 3 ] ) );

	const int pictureWidth  = static_cast< int >( picture.Width );
	const int pictureHeight = static_cast< int >( picture.Height );

	//---------------------------------------------------------------------
	// The grid.
	//
	// Rows follow from columns and the *output* aspect, so a cell is as square
	// as an integer count allows and the characters are not stretched. It is
	// the output that is measured because that is where the characters are
	// drawn; the source could be any shape and is resampled on the way in.
	//---------------------------------------------------------------------
	const int columns = ColumnsFromParam( params[ PT_COLUMNS ] );
	const int rows    = std::max( 1, static_cast< int >(
	                                  std::lround( static_cast< float >( columns ) * outputHeight / outputWidth ) ) );

	if( !copyBuffer.Ensure( pictureWidth, pictureHeight, GL_RGBA16F, PassBuffer::Sampling::Mipmapped )
	    || !cellBuffer.Ensure( columns, rows, GL_RGBA16F, PassBuffer::Sampling::Nearest ) )
	{
		diag::error( "could not allocate the pass buffers" );
		return FF_FAIL;
	}

	//---------------------------------------------------------------------
	// 1. The picture, into a texture of ours, with a mip chain on it.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( copyBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		copyBuffer.ResizeViewPort();
		ScopedShaderBinding shader( copyShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( picture.Handle );

		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( picture );
		copyShader.Set( "InputTexture", 0 );
		copyShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		copyShader.Set( "HalfTexel",
		                0.5f / static_cast< float >( pictureWidth ),
		                0.5f / static_cast< float >( pictureHeight ) );
		quad.Draw();
	}
	copyBuffer.GenerateMipmaps();

	//---------------------------------------------------------------------
	// 2. One character per cell.
	//---------------------------------------------------------------------
	const float cellWidthPixels  = static_cast< float >( pictureWidth ) / static_cast< float >( columns );
	const float cellHeightPixels = static_cast< float >( pictureHeight ) / static_cast< float >( rows );
	//The larger of the two axes, so a cell that is wide and short is averaged
	//over its whole width rather than aliasing along it.
	const float subLod  = LodForFootprint( std::max( cellWidthPixels, cellHeightPixels ) / 8.0f );
	const float cellLod = LodForFootprint( std::max( cellWidthPixels, cellHeightPixels ) );

	{
		ScopedFBOBinding fbo( cellBuffer.GetGLID(), ScopedFBOBinding::RB_REVERT );
		cellBuffer.ResizeViewPort();
		ScopedShaderBinding shader( cellShader.GetGLID() );

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, copyBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, glyphTexture );
		glActiveTexture( GL_TEXTURE0 );

		cellShader.Set( "CopyTexture", 0 );
		cellShader.Set( "GlyphTexture", 1 );
		cellShader.Set( "CellCount", static_cast< float >( columns ), static_cast< float >( rows ) );
		cellShader.Set( "SubLod", subLod );
		cellShader.Set( "GlyphCount", static_cast< int >( alphabet.size() ) );

		cellShader.Set( "Gamma", GammaFromParam( params[ PT_TONE ] ) );
		cellShader.Set( "Contrast", ContrastFromParam( params[ PT_CONTRAST ] ) );
		cellShader.Set( "Invert", params[ PT_INVERT ] );
		cellShader.Set( "Structure", params[ PT_STRUCTURE ] );
		cellShader.Set( "Dither", params[ PT_DITHER ] );
		cellShader.Set( "CoverLow", coverLow );
		cellShader.Set( "CoverHigh", coverHigh );

		quad.Draw();

		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	//---------------------------------------------------------------------
	// 3. Draw them.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( typeShader.GetGLID() );

		//Magnified, a character should be a grid of hard pixels, because that
		//is what a character on a screen was. Minified -- a hundred columns
		//into a small preview -- hard pixels alias into a shimmer, and this is
		//the control for it rather than something guessed at from the cell
		//size.
		const bool smooth = params[ PT_EDGE ] > 0.5f;
		{
			Scoped2DTextureBinding binding( atlasTexture );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth ? GL_LINEAR : GL_NEAREST );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth ? GL_LINEAR : GL_NEAREST );
		}

		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, cellBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, atlasTexture );
		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, copyBuffer.GetTextureInfo().Handle );
		glActiveTexture( GL_TEXTURE0 );

		typeShader.Set( "CellTexture", 0 );
		typeShader.Set( "AtlasTexture", 1 );
		typeShader.Set( "CopyTexture", 2 );

		typeShader.Set( "CellCount", static_cast< float >( columns ), static_cast< float >( rows ) );
		typeShader.Set( "AtlasSlots", static_cast< float >( kAtlasCols ), static_cast< float >( kAtlasRows ) );
		typeShader.Set( "AtlasSize",
		                static_cast< float >( kAtlasCols * kSlotSize ),
		                static_cast< float >( kAtlasRows * kSlotSize ) );
		typeShader.Set( "CellLod", cellLod );

		typeShader.Set( "InkColour", params[ PT_INK_R ], params[ PT_INK_G ], params[ PT_INK_B ] );
		typeShader.Set( "PaperColour", params[ PT_PAPER_R ], params[ PT_PAPER_G ], params[ PT_PAPER_B ] );
		typeShader.Set( "PaperOpacity", params[ PT_PAPER_OPACITY ] );
		typeShader.Set( "Tint", params[ PT_TINT ] );
		typeShader.Set( "Mix", params[ PT_MIX ] );

		quad.Draw();

		glActiveTexture( GL_TEXTURE2 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	return FF_SUCCESS;
}

FFResult Asciify::DeInitGL()
{
	copyShader.FreeGLResources();
	cellShader.FreeGLResources();
	typeShader.FreeGLResources();
	quad.Release();

	copyBuffer.Destroy();
	cellBuffer.Destroy();

	if( atlasTexture != 0 )
	{
		glDeleteTextures( 1, &atlasTexture );
		atlasTexture = 0;
	}
	if( glyphTexture != 0 )
	{
		glDeleteTextures( 1, &glyphTexture );
		glyphTexture = 0;
	}

	//The alphabet has to be re-uploaded against whatever textures the next
	//InitGL creates, so mark it stale rather than assuming it survives.
	alphabetDirty = true;

	return FF_SUCCESS;
}

FFResult Asciify::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	if( index == PT_PRESET )
	{
		const int chosen = static_cast< int >( std::lround( value ) );
		if( chosen != static_cast< int >( std::lround( params[ PT_PRESET ] ) ) )
			applyPreset( chosen );
		return FF_SUCCESS;
	}

	//Deliberately not logged. A parameter change is not a diagnostic event: the
	//host already shows the value, and an operator animating a slider would put
	//a line in the log every frame. The log exists for the shader that will not
	//compile, and it is worth nothing if it is buried.
	if( index == PT_SET && value != params[ index ] )
		alphabetDirty = true;

	// A slider moved while a preset is active means the operator has taken
	// over: the dropdown falls back to Custom. The equality guard matters —
	// hosts that honour the value events echo the preset's own values straight
	// back through here, and that echo must not un-set the preset.
	const float previous = params[ index ];
	params[ index ]      = value;

	const int active = static_cast< int >( std::lround( params[ PT_PRESET ] ) );
	if( active > 0 && std::fabs( value - previous ) > 1e-4f )
	{
		for( unsigned int id : kPresetParamIDs )
		{
			if( id == index )
			{
				params[ PT_PRESET ] = 0.0f;
				RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );
				break;
			}
		}
	}

	return FF_SUCCESS;
}

void Asciify::applyPreset( int presetIndex )
{
	params[ PT_PRESET ] = static_cast< float >( presetIndex );

	if( presetIndex <= 0 || presetIndex > asciify::presets::kCount )
		return;//Custom: the sliders keep whatever they said

	const asciify::presets::Preset& preset = asciify::presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < asciify::presets::kParamCount; ++j )
	{
		const unsigned int id = kPresetParamIDs[ j ];
		if( std::fabs( params[ id ] - preset.v[ j ] ) <= 1e-6f )
			continue;

		if( id == PT_SET )
			alphabetDirty = true;

		// The copy is what changes the picture; the event only tells the host
		// to re-read the slider. A host that ignores it renders the preset
		// correctly and merely shows stale knobs.
		params[ id ] = preset.v[ j ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}
}

float Asciify::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

FFResult Asciify::SetTextParameter( unsigned int index, const char* value )
{
	if( index != PT_CUSTOM )
		return FF_FAIL;

	const std::string incoming = value ? value : "";

	std::lock_guard< std::mutex > lock( textMutex );
	if( incoming != customText )
	{
		customText = incoming;
		//Only the render thread may touch a GL object, and this is not it.
		alphabetDirty = true;
	}

	return FF_SUCCESS;
}

char* Asciify::GetTextParameter( unsigned int index )
{
	if( index != PT_CUSTOM )
		return nullptr;

	//The host reads this and does not take ownership. Returning a pointer into
	//the member string is what the SDK's own example does; it stays valid until
	//the next SetTextParameter, which is on the same thread.
	return const_cast< char* >( customText.c_str() );
}
