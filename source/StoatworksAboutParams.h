/*
 * Stoatworks Labs - the About surface for the FFGL plugins.
 *
 * This file is the MASTER, in stoatworks-backend/about/ffgl. It is vendored
 * into each FFGL repo by ../../scripts/sync-about.py - edit it THERE and re-run
 * the sync, never the copies. The links and the credit line come from
 * StoatworksAboutLinks.h beside it, which is shared with the OpenFX surface;
 * the facts come from StoatworksAbout.h, generated from projects.json.
 *
 * ------------------------------------------------------ why this is not a window
 *
 * An FFGL 2.x plugin has no window and cannot make one. It declares parameters;
 * Resolume draws them, in Resolume's own layout, and that is the entire surface
 * a plugin gets. So there is no About dialog here and there is no mark behind
 * it - those need a UI, and this API has none.
 *
 * What it does have is a text parameter, which the host shows as a labelled
 * value, and an event parameter, which the host shows as a button. That is
 * enough for the same six facts: the name, the version, the licence and the
 * maker go in the text, and the guide, the project page, the source and the
 * funding page each get a button that opens the browser. Anything more
 * ambitious would mean drawing our own UI into the plugin's output texture,
 * which would be putting a dialog on the programme feed.
 *
 * The OpenFX build of the same effect has a read-only label and a real push
 * button, so it gets those instead - see StoatworksAboutOFX.h. The two surfaces
 * differ on purpose; only the links and the credit line are shared.
 *
 * ------------------------------------------------------------------ using it
 *
 * INCLUDE IT AFTER <FFGLSDK.h>. This header names FFUInt32 and does not pull
 * the SDK in itself, so an include placed above the SDK's fails with four
 * "unknown type name" errors that point here rather than at the include order.
 *
 * Extend the plugin's ParamID enum with the block, in the constructor declare
 * them, and forward the two callbacks:
 *
 *     enum ParamID : FFUInt32 {
 *         ...
 *         PT_ABOUT_FIRST,
 *         PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
 *     };
 *
 *     // constructor, after the plugin's own SetParamInfof calls. Inline
 *     // rather than a helper: SetParamInfo is protected on CFFGLPlugin, so
 *     // nothing outside the class can call it.
 *     SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT,
 *                   stoatworks::about::defaultText() );
 *     FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
 *     for( const auto& b : stoatworks::about::buttons() )
 *         SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
 *
 *     // SetFloatParameter
 *     if( index >= PT_ABOUT_FIRST )
 *         return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value )
 *                    ? FF_SUCCESS : FF_FAIL;
 *
 *     // GetTextParameter - the returned buffer must outlive the call.
 *     if( index == PT_ABOUT_FIRST )
 *     {
 *         static const std::string line = stoatworks::about::textParam( 0 );
 *         return const_cast< char* >( line.c_str() );
 *     }
 *
 *     // SetTextParameter - LOAD-BEARING, and its absence is invisible offline.
 *     // instantiateGL pushes every declared default back through the setters
 *     // and deletes the instance the moment one returns FF_FAIL, which is
 *     // exactly what CFFGLPlugin's stub does. Omit this and the plugin cannot
 *     // be created in any real host, while every in-repo harness still passes.
 *     if( index == PT_ABOUT_FIRST )
 *         return FF_SUCCESS;
 *
 * Zero-initialise the plugin's `params[]` array. The About ids are never
 * stored to, so without it GetFloatParameter hands the host whatever was on
 * the stack for them.
 */

#pragma once

#include "StoatworksAboutLinks.h"

namespace stoatworks::about
{

/// The text line, then one button each. Use this to size the plugin's enum.
inline constexpr unsigned kParamCount = 1u + kButtonCount;

/**
	The credit line as a stable `const char*`, for the parameter's DECLARED
	DEFAULT.

	Declare it as the default as well as answering `GetTextParameter` with it.
	Nothing in FFGL says which of the two a host reads, and they do differ --
	oxbow's host reads only the prototype's default and never asks the instance
	-- so a plugin that supplies one and not the other shows a blank About line
	in whichever host happened to pick the other. Supplying both costs a string.

	Function-local static because `SetParamInfo` keeps the pointer: a temporary
	`std::string`'s buffer is gone before the host reads it.
*/
inline const char* defaultText()
{
	static const std::string line = creditLine();
	return line.c_str();
}

/// The text line: the credit, unchanged. Kept as a function taking an offset
/// because that is the shape the plugins call it with.
inline std::string textParam( FFUInt32 offset )
{
	if( offset != 0 )
		return {};

	return creditLine();
}

/**
	Handle a press. `offset` is the id relative to the block's first parameter.

	Returns true when the parameter belonged to this block, so the caller can
	return FF_SUCCESS without a second range check.
*/
inline bool handleParam( FFUInt32 offset, float value )
{
	if( offset == 0 )
		return true;// the text line, nothing to do

	const std::size_t index = static_cast< std::size_t >( offset ) - 1;
	if( index >= kButtonCount )
		return false;

	// An event parameter arrives as 1.0 on press and 0.0 on release; opening
	// the browser on both would open it twice.
	if( value >= 0.5f )
		openUrl( buttons()[ index ].url );

	return true;
}

} // namespace stoatworks::about
