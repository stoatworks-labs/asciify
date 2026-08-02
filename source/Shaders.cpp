#include "Shaders.h"

namespace asciify
{

const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;

	//Straight through, in 0..1 frame space. The usual FFGL vertex shader folds
	//MaxUV in here; that happens once in the copy pass instead, and every pass
	//after it is working on a texture we allocated, where the picture really
	//does fill the texture.
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// Pass 1: copy.
//---------------------------------------------------------------------------
const char* const kCopyShader = R"(#version 410 core

uniform sampler2D InputTexture;
uniform vec2 MaxUV;       //the part of the input texture that is really picture
uniform vec2 HalfTexel;   //half an input texel, in picture space

in vec2 uv;
out vec4 fragColor;

void main()
{
	//The copy is allocated at picture size, so these samples land on input
	//texel centres and the clamp never bites. It is here for the frame where
	//the host changes resolution between allocating the buffer and drawing
	//into it, when a fetch at the edge would otherwise take half its weight
	//from the texture's undrawn padding.
	vec2 picture = clamp( uv, HalfTexel, vec2( 1.0 ) - HalfTexel );

	//Premultiplied in, premultiplied out. Left that way on purpose: the mip
	//chain built on this texture is a box filter, and averaging premultiplied
	//samples is the correct filter. Averaging straight colour is what smears
	//the colour of transparent pixels into the picture.
	fragColor = texture( InputTexture, picture * MaxUV );
}
)";

//---------------------------------------------------------------------------
// Pass 2: cell.
//---------------------------------------------------------------------------
const char* const kCellShader = R"(#version 410 core

uniform sampler2D CopyTexture;   //picture, premultiplied, mipmapped
uniform sampler2D GlyphTexture;  //the alphabet's measured moments

uniform vec2 CellCount;    //characters across, characters down
uniform float SubLod;      //mip level whose texels are one sub-cell across
uniform int GlyphCount;    //how many characters are in play

uniform float Gamma;
uniform float Contrast;
uniform float Invert;
uniform float Structure;
uniform float Dither;
uniform float CoverLow;    //least ink any character in the alphabet can put down
uniform float CoverHigh;   //most

in vec2 uv;
out vec4 fragColor;

//--- mirrored from Match.h -------------------------------------------------
const float kMomentC     = 0.328125;
const float kScaleLinear = 1.745743;
const float kScaleCross  = 3.047619;
const float kScaleQuad   = 3.491486;
const float kShapeFloor  = 0.04;
const float kShapeAllowance = 0.30;

float axis( int k )
{
	return ( float( k ) + 0.5 ) / 4.0 - 1.0;
}
//---------------------------------------------------------------------------

/// Ordered dither on a 4x4 grid, in units of one quantisation step. Broken out
/// because it is mirrored in Match-land too -- asctest predicts it rather than
/// switching it off.
float bayer( ivec2 cell )
{
	const float kMatrix[ 16 ] = float[ 16 ](
		 0.0,  8.0,  2.0, 10.0,
		12.0,  4.0, 14.0,  6.0,
		 3.0, 11.0,  1.0,  9.0,
		15.0,  7.0, 13.0,  5.0 );

	int x = cell.x - 4 * ( cell.x / 4 );
	int y = cell.y - 4 * ( cell.y / 4 );
	return ( kMatrix[ y * 4 + x ] + 0.5 ) / 16.0 - 0.5;
}

void main()
{
	vec2 cellIndex = floor( uv * CellCount );
	vec2 cellSize  = 1.0 / CellCount;
	vec2 cellBase  = cellIndex * cellSize;

	//One quantisation step of tone: the gap between two characters that are
	//neighbours in the ramp. Dithering by exactly this much at full strength is
	//what makes it break up banding without inventing detail.
	float ditherStep = Dither * bayer( ivec2( cellIndex ) ) / max( 1.0, float( GlyphCount - 1 ) );

	float sum0 = 0.0;
	float sum1 = 0.0;
	float sum2 = 0.0;
	float sum3 = 0.0;
	float sum4 = 0.0;
	float sum5 = 0.0;
	vec4 colourSum = vec4( 0.0 );

	for( int row = 0; row < 8; ++row )
	{
		float v = axis( row );
		for( int col = 0; col < 8; ++col )
		{
			float u = axis( col );

			//Sub-cell centre. Row 0 is the bottom, which is also how the glyph
			//bitmaps are stored, so u and v mean the same thing on both sides
			//of the comparison.
			vec2 point = cellBase + ( vec2( float( col ), float( row ) ) + 0.5 ) * cellSize / 8.0;
			vec4 texel = textureLod( CopyTexture, point, SubLod );
			colourSum += texel;

			//Straight colour for the luminance. A dark pixel and a transparent
			//pixel are not the same thing, and reading luminance off the
			//premultiplied value would typeset them identically.
			vec3 straight = texel.rgb / max( texel.a, 1.0 / 255.0 );
			float luma    = dot( straight, vec3( 0.2126, 0.7152, 0.0722 ) );

			float tone = mix( luma, 1.0 - luma, Invert );
			tone = clamp( ( tone - 0.5 ) * Contrast + 0.5, 0.0, 1.0 );
			tone = pow( tone, Gamma );
			tone = clamp( tone + ditherStep, 0.0, 1.0 );

			//Into coverage units, so that the number below is directly
			//comparable with a glyph's own measured ink.
			float x = mix( CoverLow, CoverHigh, tone );

			sum0 += x;
			sum1 += x * u;
			sum2 += x * v;
			sum3 += x * u * v;
			sum4 += x * ( u * u - kMomentC );
			sum5 += x * ( v * v - kMomentC );
		}
	}

	float coverage = sum0 / 64.0;
	vec2 shapeA    = vec2( sum1, sum2 ) / 64.0 * kScaleLinear;
	float shapeB   = sum3 / 64.0 * kScaleCross;
	vec2 shapeC    = vec2( sum4, sum5 ) / 64.0 * kScaleQuad;

	float cellLength = sqrt( dot( shapeA, shapeA ) + shapeB * shapeB + dot( shapeC, shapeC ) );
	float confidence = cellLength < kShapeFloor ? cellLength / kShapeFloor : 1.0;

	//How far the shape term may pull a cell off its correct weight, in coverage.
	//Relative to what this alphabet can express, so the control means the same
	//thing for every character set. Mirror of ShapeAllowance in Match.cpp.
	float allowance = Structure * kShapeAllowance * max( 0.0, CoverHigh - CoverLow );

	//--- the match ---------------------------------------------------------
	int best       = 0;
	float bestCost = 1.0e30;

	for( int i = 0; i < GlyphCount; ++i )
	{
		vec4 a = texelFetch( GlyphTexture, ivec2( i, 0 ), 0 );
		vec4 b = texelFetch( GlyphTexture, ivec2( i, 1 ), 0 );

		float toneError = a.x - coverage;
		float cost      = toneError * toneError;

		if( allowance > 0.0 )
		{
			vec2 glyphA  = a.yz;
			float glyphB = a.w;
			vec2 glyphC  = b.xy;

			float glyphLength = sqrt( dot( glyphA, glyphA ) + glyphB * glyphB + dot( glyphC, glyphC ) );

			//A glyph with no direction of its own scores zero alignment rather
			//than counting as a mismatch -- but it still pays the penalty in a
			//cell that does have a direction, which is what keeps structured
			//cells from coming out blank.
			float alignment = 0.0;
			if( cellLength > 1.0e-6 && glyphLength > 1.0e-6 )
			{
				float d   = dot( shapeA, glyphA ) + shapeB * glyphB + dot( shapeC, glyphC );
				alignment = d / ( cellLength * glyphLength );
			}

			cost += allowance * allowance * confidence * ( 1.0 - alignment );
		}

		if( cost < bestCost )
		{
			bestCost = cost;
			best     = i;
		}
	}

	//The alphabet's atlas slot, not its position in the alphabet: the type pass
	//addresses the atlas and has no idea which characters are in play.
	float slot = texelFetch( GlyphTexture, ivec2( best, 1 ), 0 ).z;

	//Straight colour, weighted by alpha, which is what dividing the summed
	//premultiplied colour by the summed alpha gives. A cell that is mostly
	//transparent takes its colour from the part of it that is not.
	vec3 cellColour = colourSum.rgb / max( colourSum.a, 1.0 / 255.0 );

	fragColor = vec4( cellColour, slot );
}
)";

//---------------------------------------------------------------------------
// Pass 3: type.
//---------------------------------------------------------------------------
const char* const kTypeShader = R"(#version 410 core

uniform sampler2D CellTexture;   //one texel per character. NEAREST, always.
uniform sampler2D AtlasTexture;  //the font
uniform sampler2D CopyTexture;   //the picture, for alpha and for the dry side

uniform vec2 CellCount;
uniform vec2 AtlasSlots;     //slots across, slots down
uniform vec2 AtlasSize;      //texels across, texels down
uniform float CellLod;

uniform vec3 InkColour;
uniform vec3 PaperColour;
uniform float PaperOpacity;
uniform float Tint;          //0 takes the ink colour, 1 takes the picture's
uniform float Mix;

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec2 grid   = uv * CellCount;
	ivec2 cell  = ivec2( clamp( floor( grid ), vec2( 0.0 ), CellCount - 1.0 ) );
	vec2 local  = clamp( grid - vec2( cell ), 0.0, 1.0 );

	vec4 cellData = texelFetch( CellTexture, cell, 0 );
	float slot    = cellData.a;

	//Slot to atlas position. The glyph is inset by one texel inside its slot,
	//and that blank border is what stops a smoothed fetch at the edge of one
	//character picking up the ink of the next.
	vec2 slotIndex = vec2( mod( slot, AtlasSlots.x ), floor( slot / AtlasSlots.x ) );
	vec2 slotSize  = AtlasSize / AtlasSlots;
	vec2 texel     = slotIndex * slotSize + 1.0 + local * 8.0;

	float ink = texture( AtlasTexture, texel / AtlasSize ).r;

	//The picture's own alpha for this cell, averaged over the cell by the mip
	//chain. Transparent parts of the clip stay transparent: an ASCII render of
	//nothing should be nothing, not a field of spaces on black.
	vec2 cellCentre = ( vec2( cell ) + 0.5 ) / CellCount;
	float pictureAlpha = textureLod( CopyTexture, cellCentre, CellLod ).a;

	vec3 colour = mix( PaperColour, mix( InkColour, cellData.rgb, Tint ), ink );
	float alpha = mix( PaperOpacity, 1.0, ink ) * pictureAlpha;

	vec4 typed = vec4( colour * alpha, alpha );//premultiplied, as the host expects
	vec4 plain = texture( CopyTexture, uv );

	vec4 result = mix( plain, typed, Mix );

	//Hold the invariant the engine expects. Mixing two premultiplied colours is
	//already correct, so this only ever trims rounding.
	result.rgb = clamp( result.rgb, vec3( 0.0 ), vec3( result.a ) );

	fragColor = result;
}
)";

} // namespace asciify
