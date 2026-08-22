/*
 * Stoatworks Labs - the About surface for the OpenFX builds.
 *
 * This file is the MASTER, in stoatworks-backend/about/ofx. It is vendored into
 * each plugin repo by ../../scripts/sync-about.py - edit it THERE and re-run the
 * sync, never the copies. The links and the credit line come from
 * StoatworksAboutLinks.h, shared with the FFGL surface.
 *
 * ------------------------------------------------- why this is not the FFGL one
 *
 * FFGL has no read-only control and no button: the credit has to be a text
 * parameter the host happens to render as a value, and each link has to be an
 * event parameter the host happens to render as a button. OpenFX has both for
 * real -- a string parameter of type Label is read-only by definition, and a
 * push button is a push button -- and it has groups that fold, so the block can
 * sit at the bottom of the page out of the way. So this surface is built out of
 * what OFX actually offers rather than mirroring the FFGL shape.
 *
 * Three things are set here and all three matter:
 *
 *   setIsPersistant( false )  the credit holds nothing worth saving into a
 *                             project file. A push button has no persistence
 *                             property at all -- PushButtonParamDescriptor
 *                             derives straight from ParamDescriptor, not from
 *                             ValueParamDescriptor, because a button is not a
 *                             value -- so this is set on the label only.
 *   setAnimates( false )      the credit is not keyframeable, and a host will
 *                             happily offer a keyframe track for anything that
 *                             does not say so. Same reason it is label-only.
 *   kOfxParamPropEvaluate-    pressing a link must not dirty the render cache.
 *       OnChange = 0          Without it every press re-renders the timeline.
 *                             Set through the property set: the Support
 *                             library exposes no setter for it.
 *
 * ------------------------------------------------------------------ using it
 *
 * INCLUDE IT AFTER "ofxsImageEffect.h". This header names OFX types and does
 * not pull the Support library in itself.
 *
 *     // describeInContext, last, after the plugin's own parameters
 *     stoatworks::about::ofx::describe( desc, page );
 *
 *     // the plugin's changedParam override -- add one if there is none
 *     void MyPlugin::changedParam( const OFX::InstanceChangedArgs& args,
 *                                  const std::string& name )
 *     {
 *         if( stoatworks::about::ofx::changedParam( args, name ) )
 *             return;
 *         ...
 *     }
 *
 * Forgetting the changedParam line is a dead button, not a crash: the block
 * still draws and nothing happens when it is pressed.
 */

#pragma once

#include "../StoatworksAboutLinks.h"

namespace stoatworks::about::ofx
{

/// Parameter names, which are the host-visible script names and so are stable.
/// Prefixed because a host puts every parameter of one effect in one namespace
/// and `about` is a plausible name for something a plugin already has.
inline constexpr const char* kGroupName  = "stoatworksAbout";
inline constexpr const char* kCreditName = "stoatworksAboutCredit";
inline constexpr const char* kButtonPrefix = "stoatworksAboutLink";

/// Take the parameter out of the render cache's dependency set.
///
/// Through the raw property because the Support library wraps no setter for
/// `kOfxParamPropEvaluateOnChange`. `throwOnFailure` is off: a host that does
/// not support the property should leave the block drawn and working, not stop
/// the effect from describing itself.
inline void noEvaluate( OFX::ParamDescriptor& param )
{
	param.getPropertySet().propSetInt( kOfxParamPropEvaluateOnChange, 0, false );
}

/// The script name of the nth link button. Index is into `buttons()`.
inline std::string buttonName( std::size_t index )
{
	return std::string( kButtonPrefix ) + std::to_string( index );
}

/**
	Add the block to the page. Call it last, after the plugin's own parameters.

	The group is closed by default: this is reference material, not a control
	somebody reaches for mid-grade, and an open group would push the real
	parameters up out of view every time the effect is added.
*/
inline void describe( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page )
{
	OFX::GroupParamDescriptor* group = desc.defineGroupParam( kGroupName );
	group->setLabels( "About", "About", "About" );
	group->setOpen( false );
	if( page != nullptr )
		page->addChild( *group );

	OFX::StringParamDescriptor* credit = desc.defineStringParam( kCreditName );
	credit->setLabels( "", "", "" );
	// A Label string is read-only in the host by definition -- there is no
	// editable field to leave the operator wondering what happens if they type
	// in it.
	credit->setStringType( OFX::eStringTypeLabel );
	credit->setDefault( creditLine() );
	credit->setIsPersistant( false );
	credit->setAnimates( false );
	noEvaluate( *credit );
	credit->setParent( *group );
	if( page != nullptr )
		page->addChild( *credit );

	for( std::size_t i = 0; i < buttons().size(); ++i )
	{
		const Button& b = buttons()[ i ];

		OFX::PushButtonParamDescriptor* button = desc.definePushButtonParam( buttonName( i ) );
		button->setLabels( b.label, b.label, b.label );
		// The URL as the tooltip: a link nobody can read before pressing is
		// worse than one they can, and this is the only place OFX will show it.
		button->setHint( b.url );
		noEvaluate( *button );
		button->setParent( *group );
		if( page != nullptr )
			page->addChild( *button );
	}
}

/**
	Handle a press. Returns true when the name belonged to this block, so the
	caller can return without a second comparison.

	Only a user edit opens a browser. A host is free to re-send parameter
	changes when a project loads or a plugin is duplicated, and
	`eChangeUserEdit` is the only reason that means somebody clicked.
*/
inline bool changedParam( const OFX::InstanceChangedArgs& args, const std::string& name )
{
	if( name.rfind( kButtonPrefix, 0 ) != 0 )
		return name == kCreditName;// ours, and nothing to do

	if( args.reason != OFX::eChangeUserEdit )
		return true;

	for( std::size_t i = 0; i < buttons().size(); ++i )
		if( name == buttonName( i ) )
		{
			openUrl( buttons()[ i ].url );
			return true;
		}

	return true;
}

} // namespace stoatworks::about::ofx
