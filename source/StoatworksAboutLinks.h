/*
 * Stoatworks Labs - the links and the maker line, with no host SDK attached.
 *
 * This file is the MASTER, in stoatworks-backend/about/cpp. It is vendored into
 * each plugin repo by ../../scripts/sync-about.py - edit it THERE and re-run the
 * sync, never the copies. The facts come from StoatworksAbout.h beside it,
 * which is generated from the website's projects.json.
 *
 * ------------------------------------------------------------ why it is split
 *
 * A plugin repo builds the same effect for two plugin APIs: FFGL, for Resolume,
 * and OpenFX, for Resolve and Nuke. Both need the same four links and the same
 * one-line credit, and neither can include the other's SDK - so what they share
 * lives here, in plain C++, and each API's own header sits on top of it:
 *
 *     StoatworksAboutLinks.h    <- this file. No SDK. Links, credit, openUrl.
 *     StoatworksAboutParams.h   <- FFGL. Needs FFGLSDK.h first.
 *     StoatworksAboutOFX.h      <- OpenFX. Needs ofxsImageEffect.h first.
 *
 * The surfaces they build are NOT the same, because the two APIs do not offer
 * the same things, and pretending otherwise would make both worse. FFGL has no
 * window and only parameters, so the credit is a text parameter and each link is
 * an event parameter the host draws as a button. OpenFX has a real read-only
 * label and a real push button, and groups that fold, so it gets those.
 *
 * ------------------------------------------------------------------- ASCII
 *
 * Every string here is ASCII, matching the generated header. MSVC warns on a
 * UTF-8 source without /utf-8, and these build on all three platforms.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdlib>
#include <string>

#include "StoatworksAbout.h"

namespace stoatworks::about
{

/*
 * The buttons, in the order the host shows them. A link the product does not
 * have - a guide that is not written, a repo that is still private - is left
 * out of the list rather than shown as a button that opens a 404.
 *
 * All of it is constexpr because an FFGL parameter id has to be: the host is
 * told how many parameters a plugin has before any of this could run, and the
 * plugin's own enum has to name the block's size.
 */
struct Button
{
	const char* label;
	const char* url;
};

inline constexpr bool present( const char* s )
{
	return s != nullptr && s[ 0 ] != '\0';
}

/**
	One funding button rather than four.

	The host draws these beside the controls somebody is using mid-show, and
	four donation buttons in there would be shouting. It goes to the website's
	support page, which lists all four.
*/
inline constexpr const char* kSupportUrl = "https://stoatworks-labs.com/support";

inline constexpr unsigned kButtonCount =
	( present( guide ) ? 1u : 0u ) + ( present( page ) ? 1u : 0u ) + ( present( repo ) ? 1u : 0u ) + 1u;

inline const std::array< Button, kButtonCount >& buttons()
{
	static const std::array< Button, kButtonCount > list = [] {
		std::array< Button, kButtonCount > out{};
		std::size_t i = 0;
		if constexpr( present( guide ) )
			out[ i++ ] = { "User guide", guide };
		if constexpr( present( page ) )
			out[ i++ ] = { "Project page", page };
		if constexpr( present( repo ) )
			out[ i++ ] = { "Source on GitHub", repo };
		out[ i++ ] = { "Support the work", kSupportUrl };
		return out;
	}();
	return list;
}

/// Open a URL in the user's browser.
///
/// A plugin shelling out is not pretty, but a plugin has no host API for this
/// and the alternative is a URL the user has to copy off the screen by hand.
/// The URLs are compile-time constants from the generated header, never
/// anything a project file or a host could supply, so there is nothing here for
/// a crafted string to reach.
inline void openUrl( const char* url )
{
#if defined( _WIN32 )
	std::string command = std::string( "start \"\" \"" ) + url + "\"";
#elif defined( __APPLE__ )
	std::string command = std::string( "open \"" ) + url + "\"";
#else
	std::string command = std::string( "xdg-open \"" ) + url + "\" &";
#endif
	std::system( command.c_str() );
}

/**
	The maker, as `Stoatworks Labs, stoatworks-labs.com`.

	`org` and `home` come from the generated header, so this is not a second
	place the name is written down. The scheme is dropped because the line is
	read, never clicked: it is a label in every host that shows it, the field it
	sits in is narrow, and the buttons beside it are what open a browser.
*/
inline std::string maker()
{
	std::string url = home;
	for( const char* scheme : { "https://", "http://" } )
		if( url.rfind( scheme, 0 ) == 0 )
		{
			url.erase( 0, std::string( scheme ).size() );
			break;
		}
	if( !url.empty() && url.back() == '/' )
		url.pop_back();

	return std::string( org ) + ", " + url;
}

/**
	The credit line: what this is, what version of it is loaded, and whose it is.

	The maker is on it because a button's label is all a host shows until
	somebody presses it, so with the buttons alone nothing on screen ever said
	Stoatworks Labs. This is the one part of the block that can be read rather
	than clicked.
*/
inline std::string creditLine()
{
	std::string out = std::string( name ) + " " + versionFallback;
	if( std::string( licence ).size() )
		out += " - " + std::string( licence );
	return out + " - " + maker();
}

} // namespace stoatworks::about
