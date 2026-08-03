#pragma once

/**
    Factory presets: named typesetting looks an operator can reach in one
    gesture. Each entry is a recognisable *thing that typeset pictures* — a
    phosphor terminal, a typewriter, a teletext page — not a random
    collection of slider positions.

    The values live in the same 0..1 parameter space both builds expose (the
    FFGL and OFX builds deliberately share it), so ONE table drives both and a
    preset looks identical in Resolume and Resolve. Plain data only; the
    application machinery lives with each host's glue.

    Element 0 of the host-facing dropdown is "Custom" and is not in this
    table: it means "the sliders are the truth".

    A preset covers the type and colour parameters. The Custom Set string
    belongs to the operator (a preset overwriting typed text would be rude),
    and Mix is the wet/dry — how much effect is wanted is not part of any
    look.
*/

namespace asciify
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. The FFGL build binds
/// this order to its ParamIDs and the OFX build to its param handles; both
/// static_assert against kParamCount so the three lists cannot drift apart
/// silently.
enum Param
{
	kColumns,
	kSet,
	kStructure,
	kTone,
	kContrast,
	kInvert,
	kDither,
	kTint,
	kInkR,
	kInkG,
	kInkB,
	kPaperR,
	kPaperG,
	kPaperB,
	kPaperOpacity,
	kEdge,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// Columns is the 8..320 geometric mapping (0.624 is 80 columns, the terminal
// standard). Characters is the Set enum's index: 0 ASCII, 4 Classic ramp,
// 5 Binary, 6 Blocks, 7 Box. Tint 0 is the ink's colour, 1 the picture's.
inline constexpr Preset kPresets[] = {
	// The green phosphor terminal: 80 columns of ASCII, ink that glows a
	// little too evenly to be paint. The plugin's own defaults, named.
	{ "Green Terminal",
	  { /*Cols*/ 0.624f, /*Set*/ 0, /*Struct*/ 0.35f, /*Tone*/ 0.5f, /*Contrast*/ 0.5f, /*Invert*/ 0.0f,
	    /*Dither*/ 0.5f, /*Tint*/ 0.0f, /*Ink*/ 0.60f, 1.00f, 0.70f, /*Paper*/ 0.02f, 0.05f, 0.03f,
	    /*PaperOp*/ 1.0f, /*Edge*/ 0.0f } },

	// The same terminal a few years earlier, when the phosphor was P3.
	{ "Amber Terminal",
	  { /*Cols*/ 0.624f, /*Set*/ 0, /*Struct*/ 0.35f, /*Tone*/ 0.5f, /*Contrast*/ 0.5f, /*Invert*/ 0.0f,
	    /*Dither*/ 0.5f, /*Tint*/ 0.0f, /*Ink*/ 1.00f, 0.72f, 0.20f, /*Paper*/ 0.05f, 0.03f, 0.00f,
	    /*PaperOp*/ 1.0f, /*Edge*/ 0.0f } },

	// Dark ink struck onto cream paper: the classic ramp, inverted so the
	// marks are the dark end, at a page-like sixty columns.
	{ "Typewriter",
	  { /*Cols*/ 0.546f, /*Set*/ 4, /*Struct*/ 0.3f, /*Tone*/ 0.5f, /*Contrast*/ 0.55f, /*Invert*/ 1.0f,
	    /*Dither*/ 0.35f, /*Tint*/ 0.0f, /*Ink*/ 0.10f, 0.09f, 0.08f, /*Paper*/ 0.95f, 0.92f, 0.85f,
	    /*PaperOp*/ 1.0f, /*Edge*/ 0.0f } },

	// A wire-photo halftone: fine columns of the tonal ramp, black on white,
	// dithered hard so the midtones grain rather than band.
	{ "Newsprint",
	  { /*Cols*/ 0.812f, /*Set*/ 4, /*Struct*/ 0.2f, /*Tone*/ 0.5f, /*Contrast*/ 0.6f, /*Invert*/ 1.0f,
	    /*Dither*/ 0.8f, /*Tint*/ 0.0f, /*Ink*/ 0.08f, 0.08f, 0.08f, /*Paper*/ 0.97f, 0.96f, 0.93f,
	    /*PaperOp*/ 1.0f, /*Edge*/ 0.0f } },

	// Ones and zeros pouring down a dark screen, dense and bright.
	{ "Binary Cascade",
	  { /*Cols*/ 0.734f, /*Set*/ 5, /*Struct*/ 0.25f, /*Tone*/ 0.5f, /*Contrast*/ 0.55f, /*Invert*/ 0.0f,
	    /*Dither*/ 0.6f, /*Tint*/ 0.0f, /*Ink*/ 0.35f, 1.00f, 0.45f, /*Paper*/ 0.00f, 0.03f, 0.01f,
	    /*PaperOp*/ 1.0f, /*Edge*/ 0.0f } },

	// The block-element set carrying the picture's own colour: forty fat
	// cells across, the teletext mosaic.
	{ "Teletext Mosaic",
	  { /*Cols*/ 0.436f, /*Set*/ 6, /*Struct*/ 0.3f, /*Tone*/ 0.5f, /*Contrast*/ 0.55f, /*Invert*/ 0.0f,
	    /*Dither*/ 0.4f, /*Tint*/ 1.0f, /*Ink*/ 1.00f, 1.00f, 1.00f, /*Paper*/ 0.00f, 0.00f, 0.00f,
	    /*PaperOp*/ 1.0f, /*Edge*/ 0.0f } },

	// Box-drawing characters in pale line-work on drawing-office blue:
	// almost no tone available, so Structure does the talking.
	{ "Blueprint",
	  { /*Cols*/ 0.546f, /*Set*/ 7, /*Struct*/ 0.8f, /*Tone*/ 0.5f, /*Contrast*/ 0.5f, /*Invert*/ 0.0f,
	    /*Dither*/ 0.3f, /*Tint*/ 0.0f, /*Ink*/ 0.85f, 0.92f, 1.00f, /*Paper*/ 0.05f, 0.15f, 0.35f,
	    /*PaperOp*/ 1.0f, /*Edge*/ 0.0f } },
};

inline constexpr int kCount = int( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace asciify
