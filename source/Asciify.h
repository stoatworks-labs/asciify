#pragma once

#include "Alphabet.h"
#include "Match.h"
#include "PassBuffer.h"
#include "Presets.h"

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

#include <mutex>
#include <string>
#include <vector>

/**
    Asciify -- a character renderer for Resolume.

    The picture is divided into character cells and each cell is replaced by the
    one character that stands in for it best. That is the entire mechanism, and
    the interesting half of it is what "best" means, which is in Match.h.

    What is worth saying here is what this deliberately is *not*. It is not a
    brightness ramp with a lookup table, and it does not carry a hand-ordered
    string of characters from dark to light. That ordering -- " .:-=+*#%@" and
    its many cousins -- is folklore. It is one person's eye applied to one
    person's font, and it is wrong for every other font including this one. Here
    the font is drawn in FontData.cpp, every glyph's ink is measured off the
    bitmap, and the ordering is a *consequence*. Change a glyph and it moves.
    Choose a different alphabet and the plugin re-measures the range and maps
    the picture's tones onto whatever that set can actually express, which is
    why the Blocks set reaches solid black and white and the Box set does not
    and neither has to be told so.

    The second thing that falls out rather than being arranged is the Structure
    control. A cell is matched on how much ink it wants *and* on where that ink
    sits, and Structure is nothing but the weight between the two. At zero it is
    the classic tone ramp. Wind it up and edges start choosing `/`, `|`, `-` and
    `\` on their own -- not because anything detects an edge, but because a cell
    with a diagonal in it has a shape vector pointing the same way a slash does.
    A flat cell has no direction to match, so it ignores the control entirely
    without needing a threshold.

    Three passes, at three resolutions, for the reason set out in Shaders.h: a
    character is chosen for a cell, not for a pixel.

    See AGENTS.md for the traps.
*/
class Asciify : public CFFGLPlugin
{
public:
	Asciify();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;

	/// The order the host shows them in: what is being typeset, what colour it
	/// comes out, and how it meets the picture underneath.
	enum ParamID : FFUInt32
	{
		//Type
		PT_COLUMNS,
		PT_SET,
		PT_CUSTOM,
		PT_STRUCTURE,
		PT_TONE,
		PT_CONTRAST,
		PT_INVERT,
		PT_DITHER,

		//Colour
		PT_TINT,
		PT_INK_R,
		PT_INK_G,
		PT_INK_B,
		PT_PAPER_R,
		PT_PAPER_G,
		PT_PAPER_B,
		PT_PAPER_OPACITY,

		//Output
		PT_EDGE,
		PT_MIX,

		//Preset. Declared after the real controls so their IDs — which a saved
		//composition refers to — do not shift under existing users.
		PT_PRESET,

		// -- The Stoatworks About block ------------------------------------------
		//
		// One display-only text line, then one button per link the block carries:
		// the guide, the project page, the source, the funding page. A button opens
		// a browser and stores nothing.
		//
		// How many buttons there are is decided by which URLs StoatworksAbout.h
		// actually holds, so Asciify.cpp static_asserts this run against
		// `about::kParamCount` -- writing a user guide later adds one, and without
		// the assert that would silently shift PT_COUNT and leave the last button
		// undeclared.
		//
		// Last in the enum so no saved composition's parameter ids shift.
		PT_ABOUT_TEXT,
			PT_ABOUT_BUTTON_1,
			PT_ABOUT_BUTTON_2,
			PT_ABOUT_BUTTON_3,
			PT_ABOUT_BUTTON_4,
		PT_COUNT
	};

	// The buttons are declared one per link, so the run above and the run the
	// block actually has must agree. They diverge the day somebody writes a
	// user guide, and this is what says so. In the header, beside the enum,
	// because a namespace-scope assert in the .cpp cannot name a class-scoped
	// enumerator unqualified.
	static_assert( PT_COUNT - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
	               "the About run no longer matches StoatworksAbout.h -- "
	               "add or remove a PT_ABOUT_BUTTON_n to match" );

private:
	/// The ParamID each presets::Param drives, in presets::Param order. The
	/// preset table stays host-agnostic; this is the FFGL binding of it.
	static constexpr unsigned int kPresetParamIDs[ asciify::presets::kParamCount ] = {
		PT_COLUMNS, PT_SET, PT_STRUCTURE, PT_TONE, PT_CONTRAST, PT_INVERT, PT_DITHER,
		PT_TINT, PT_INK_R, PT_INK_G, PT_INK_B, PT_PAPER_R, PT_PAPER_G, PT_PAPER_B,
		PT_PAPER_OPACITY, PT_EDGE
	};

	/// Copy a factory preset's values into params[] and raise value events so
	/// the host re-reads the sliders. `presetIndex` is 1-based; 0 is Custom.
	void applyPreset( int presetIndex );

	/// Re-measure the alphabet and upload it. Called when the character set or
	/// the custom string changes -- not per frame: the measurement is over a
	/// hundred glyphs and does not depend on the picture.
	void RebuildAlphabet();

	/// Upload the font bitmaps. Once, at InitGL.
	bool UploadAtlas();

	ffglex::FFGLShader copyShader;
	ffglex::FFGLShader cellShader;
	ffglex::FFGLShader typeShader;
	ffglex::FFGLScreenQuad quad;

	asciify::PassBuffer copyBuffer;///< the picture, ours, mipmapped
	asciify::PassBuffer cellBuffer;///< one texel per character: colour + glyph slot

	GLuint atlasTexture = 0;///< the font, kAtlasCols x kAtlasRows slots of 10x10
	GLuint glyphTexture = 0;///< the alphabet's measured moments, one column each

	//---------------------------------------------------------------------
	// The alphabet, as measured. Rebuilt on a control change and then left
	// alone; `alphabetDirty` exists because parameters arrive on the host's
	// thread and this has to be uploaded on the render thread.
	//---------------------------------------------------------------------
	std::vector< int > slots;                 ///< atlas slot per alphabet entry
	std::vector< asciify::Moments > alphabet; ///< measured moments per entry
	float coverLow  = 0.0f;
	float coverHigh = 1.0f;
	bool alphabetDirty = true;

	//---------------------------------------------------------------------
	// The Custom Set string is the one piece of plugin state that is not a
	// float, and the only one where the host's thread and the render thread
	// touch the same object rather than racing on a word. A std::string being
	// reallocated under a reader is a crash in somebody else's process, so it
	// gets a lock -- taken twice per change, never per frame.
	//---------------------------------------------------------------------
	std::mutex textMutex;
	std::string customText;

	/// Zero-initialised: the constructor writes a default for every real
	/// control, but the About block's ids are never stored to -- pressing a
	/// button opens a browser and returns -- so without this GetFloatParameter
	/// hands the host whatever was on the stack for them.
	float params[ PT_COUNT ] = {};
};
