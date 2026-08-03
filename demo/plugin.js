/**
 * Asciify — browser demo.
 *
 * The three shaders below are `kCopyShader`, `kCellShader` and `kTypeShader`
 * from `source/Shaders.cpp`, copied across unedited. The pass structure and
 * every uniform value are a port of `ProcessOpenGL` in `source/Asciify.cpp`;
 * `Alphabet.cpp`, `Controls.cpp`, `Match.cpp` and `Font.cpp`'s
 * `BuildAtlasImage` are ported below.
 *
 * The idea, before the arithmetic: a character cell is a *small picture*, and
 * the glyph that stands in for it is the one whose ink is distributed most like
 * it. Coverage and five moments of the ink, measured on the same 8x8 grid for
 * the cell and for every glyph. Tone is matched absolutely; shape is matched as
 * a **direction** only, because a glyph's ink is binary and a cell's is soft, so
 * their shape vectors are never the same length even when they agree.
 *
 * Two things fall out of that rather than being arranged, and both are visible
 * on this page: Structure at 0 is exactly the classic ramp with no separate code
 * path, and flat cells ignore Structure entirely because the confidence factor
 * is the length of the cell's own shape vector.
 *
 * **The ramp is measured, never written down.** The font in `font.js` is
 * extracted from `source/FontData.cpp` rather than retyped, and the ordering
 * below is computed from those bitmaps — which is why the measured order of the
 * traditional set on this font is `.-:+=*%#@`, with `-` outweighing `:` and `%`
 * lighter than `#`.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture, mipLevels } from './vendor/gl.js';
import { GLYPHS, GLYPH_SIZE, SLOT_SIZE, ATLAS_COLS, ATLAS_ROWS } from './font.js';

const MAX_ALPHABET = ATLAS_COLS * ATLAS_ROWS;
const ATLAS_W = ATLAS_COLS * SLOT_SIZE;
const ATLAS_H = ATLAS_ROWS * SLOT_SIZE;

//===========================================================================
// Ports of source/Match.h and source/Match.cpp
//===========================================================================

/// mean( u^2 ) over the eight sample centres of the 8x8 grid.
const K_MOMENT_C = 0.328125;
/// Each moment divided by its own RMS over the grid, so the five shape terms
/// are on one footing and the angle between two shape vectors means what it
/// looks like it means.
const K_SCALE_LINEAR = 1.745743;
const K_SCALE_CROSS = 3.047619;
const K_SCALE_QUAD = 3.491486;

const axis = (k) => (k + 0.5) / 4 - 1;

/// Measure a glyph's bitmap. Ink is 1, paper is 0. Row 0 is the bottom.
function measureGlyph(bits) {
  const sum = [0, 0, 0, 0, 0, 0];

  for (let row = 0; row < GLYPH_SIZE; row += 1) {
    const v = axis(row);
    for (let col = 0; col < GLYPH_SIZE; col += 1) {
      const u = axis(col);
      const x = (bits[row] >> col) & 1;
      sum[0] += x;
      sum[1] += x * u;
      sum[2] += x * v;
      sum[3] += x * u * v;
      sum[4] += x * (u * u - K_MOMENT_C);
      sum[5] += x * (v * v - K_MOMENT_C);
    }
  }

  const cells = GLYPH_SIZE * GLYPH_SIZE;
  return {
    coverage: sum[0] / cells,
    shape: [
      (sum[1] / cells) * K_SCALE_LINEAR,
      (sum[2] / cells) * K_SCALE_LINEAR,
      (sum[3] / cells) * K_SCALE_CROSS,
      (sum[4] / cells) * K_SCALE_QUAD,
      (sum[5] / cells) * K_SCALE_QUAD,
    ],
  };
}

/// An alphabet of one — or one whose characters all weigh the same, which the
/// box-drawing set very nearly does — would map every tone onto a single point.
function coverageRange(alphabet) {
  if (!alphabet.length) return [0, 1];
  let lowest = alphabet[0].coverage;
  let highest = alphabet[0].coverage;
  for (const m of alphabet) {
    if (m.coverage < lowest) lowest = m.coverage;
    if (m.coverage > highest) highest = m.coverage;
  }
  if (highest - lowest < 1e-4) highest = lowest + 1e-4;
  return [lowest, highest];
}

//===========================================================================
// Ports of source/Font.cpp
//===========================================================================

const SLOT_FOR_CODEPOINT = new Map(GLYPHS.map(([cp], i) => [cp, i]));

/// The atlas as a single-channel image, **bottom row first** so it can be
/// handed to texImage2D unchanged. Cleared to paper, which is what makes the
/// one-texel border round every slot.
function buildAtlasImage() {
  const image = new Uint8Array(ATLAS_W * ATLAS_H);

  GLYPHS.forEach(([, ...bits], slot) => {
    const slotX = slot % ATLAS_COLS;
    const slotY = Math.floor(slot / ATLAS_COLS);

    for (let row = 0; row < GLYPH_SIZE; row += 1) {
      for (let col = 0; col < GLYPH_SIZE; col += 1) {
        if (!((bits[row] >> col) & 1)) continue;
        const x = slotX * SLOT_SIZE + 1 + col;
        const y = slotY * SLOT_SIZE + 1 + row;
        image[y * ATLAS_W + x] = 255;
      }
    }
  });

  return image;
}

//===========================================================================
// Ports of source/Alphabet.cpp
//===========================================================================

const SET_NAMES = [
  'ASCII', 'Letters', 'Digits', 'Symbols', 'Classic ramp',
  'Binary', 'Blocks', 'Box drawing', 'Custom',
];

function slotsFor(set, custom) {
  const slots = [];

  /// Ignore a repeat: somebody weighting a ramp by typing "..::##" is being
  /// perfectly reasonable, but a repeat costs a comparison per cell per frame
  /// and can never win one.
  const add = (codepoint) => {
    const slot = SLOT_FOR_CODEPOINT.get(codepoint);
    if (slot === undefined || slots.includes(slot)) return;
    slots.push(slot);
  };
  const addRange = (first, last) => { for (let c = first; c <= last; c += 1) add(c); };
  const addString = (text) => { for (const ch of text) add(ch.codePointAt(0)); };

  add(0x20); // first, always

  switch (set) {
    case 0: addRange(0x21, 0x7e); break;
    case 1: addRange(0x41, 0x5a); addRange(0x61, 0x7a); break;
    case 2: addRange(0x30, 0x39); break;
    case 3:
      addRange(0x21, 0x2f); addRange(0x3a, 0x40);
      addRange(0x5b, 0x60); addRange(0x7b, 0x7e);
      break;
    // The ramp everybody knows. Kept because it is a recognisable look — not
    // because the ordering means anything here. It is re-measured like every
    // other set, and on this font the measured order is not the traditional one.
    case 4: addString('.:-=+*#%@'); break;
    case 5: addString('01'); break;
    case 6:
      // Listed by code point rather than as literals, exactly as the plugin
      // does: light/medium/dark shade, full block, halves, quadrants.
      [0x2591, 0x2592, 0x2593, 0x2588, 0x2580, 0x2584, 0x258c, 0x2590,
        0x2598, 0x259d, 0x2596, 0x2597, 0x259a, 0x259e].forEach(add);
      break;
    case 7:
      [0x2500, 0x2502, 0x250c, 0x2510, 0x2514, 0x2518, 0x251c, 0x2524,
        0x252c, 0x2534, 0x253c, 0x2571, 0x2572, 0x2573].forEach(add);
      break;
    case 8:
      addString(custom ?? '');
      // Nothing typed, or nothing this font can draw. Falling back to ASCII
      // beats an empty frame: an operator who has mistyped sees a picture that
      // is wrong rather than a picture that is missing.
      if (slots.length <= 1) return slotsFor(0, '');
      break;
    default: return slotsFor(0, '');
  }

  return slots;
}

//===========================================================================
// Ports of source/Controls.cpp
//===========================================================================

const MIN_COLUMNS = 8;
const MAX_COLUMNS = 320;
const TONE_RANGE = 4;

const clamp01 = (v) => Math.min(1, Math.max(0, v));

function columnsFromParam(value) {
  const t = clamp01(value);
  const ratio = MAX_COLUMNS / MIN_COLUMNS;
  return Math.max(MIN_COLUMNS, Math.min(MAX_COLUMNS, Math.round(MIN_COLUMNS * ratio ** t)));
}

/// Centre of the slider is exactly 1, which matters: it is the only position at
/// which the tone curve is doing nothing at all.
const gammaFromParam = (v) => TONE_RANGE ** (1 - 2 * clamp01(v));
const contrastFromParam = (v) => TONE_RANGE ** (2 * clamp01(v) - 1);

/// Mip level whose texels are `pixels` source pixels across.
const lodForFootprint = (pixels) => (pixels <= 1 ? 0 : Math.log2(pixels));

//===========================================================================
// Shaders — verbatim from source/Shaders.cpp
//===========================================================================

const VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
`;

const COPY = `#version 410 core

uniform sampler2D InputTexture;
uniform vec2 MaxUV;       //the part of the input texture that is really picture
uniform vec2 HalfTexel;   //half an input texel, in picture space

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec2 picture = clamp( uv, HalfTexel, vec2( 1.0 ) - HalfTexel );

	//Premultiplied in, premultiplied out. Left that way on purpose: the mip
	//chain built on this texture is a box filter, and averaging premultiplied
	//samples is the correct filter.
	fragColor = texture( InputTexture, picture * MaxUV );
}
`;

const CELL = `#version 410 core

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
	//premultiplied colour by the summed alpha gives.
	vec3 cellColour = colourSum.rgb / max( colourSum.a, 1.0 / 255.0 );

	fragColor = vec4( cellColour, slot );
}
`;

const TYPE = `#version 410 core

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
`;

//===========================================================================
// The renderer — a port of ProcessOpenGL in source/Asciify.cpp
//===========================================================================

function createRenderer(gl, quad) {
  const copyShader = new Program(gl, VERTEX, COPY, 'copy');
  const cellShader = new Program(gl, VERTEX, CELL, 'cell');
  const typeShader = new Program(gl, VERTEX, TYPE, 'type');

  const copyBuffer = new PassBuffer(gl, { filter: 'linear', mip: true });
  const cellBuffer = new PassBuffer(gl, { filter: 'nearest' });

  // ---- the atlas --------------------------------------------------------
  const atlasTexture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, atlasTexture);
  // One byte per texel and a width that is not a multiple of four in every
  // possible future layout, so say so rather than relying on it.
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, ATLAS_W, ATLAS_H, 0, gl.RED, gl.UNSIGNED_BYTE, buildAtlasImage());
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
  // Filters are set per frame from the Glyph Edge control. Wrapping is not:
  // clamping is the only correct answer, and a repeat here would let the last
  // column of the atlas bleed into the first.
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);

  // ---- the alphabet -----------------------------------------------------
  const glyphTexture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, glyphTexture);
  // Measured numbers, addressed by texelFetch. Nothing may be interpolated and
  // nothing may wrap.
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);

  const glyphMoments = GLYPHS.map(([, ...bits]) => measureGlyph(bits));

  let alphabetKey = null;
  let alphabetSize = 0;
  let coverLow = 0;
  let coverHigh = 1;

  function rebuildAlphabet(set, custom) {
    const key = `${set} ${set === 8 ? custom : ''}`;
    if (key === alphabetKey) return;
    alphabetKey = key;

    let slots = slotsFor(set, custom);
    if (slots.length > MAX_ALPHABET) slots = slots.slice(0, MAX_ALPHABET);

    const alphabet = slots.map((slot) => glyphMoments[slot]);
    [coverLow, coverHigh] = coverageRange(alphabet);
    alphabetSize = alphabet.length;

    // Two rows of RGBA: the six measured numbers plus the atlas slot. The
    // alphabet's *position* is never written anywhere — the cell pass converts
    // it here and the type pass never learns which characters were in play.
    const texels = new Float32Array(MAX_ALPHABET * 2 * 4);
    alphabet.forEach((m, i) => {
      const a = i * 4;
      texels[a + 0] = m.coverage;
      texels[a + 1] = m.shape[0];
      texels[a + 2] = m.shape[1];
      texels[a + 3] = m.shape[2];

      const b = (MAX_ALPHABET + i) * 4;
      texels[b + 0] = m.shape[3];
      texels[b + 1] = m.shape[4];
      texels[b + 2] = slots[i];
      texels[b + 3] = 0;
    });

    gl.bindTexture(gl.TEXTURE_2D, glyphTexture);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, MAX_ALPHABET, 2, 0, gl.RGBA, gl.FLOAT, texels);
    gl.bindTexture(gl.TEXTURE_2D, null);
  }

  return {
    render({ input, params, width, height }) {
      rebuildAlphabet(Math.round(params.get('set')), params.get('custom'));

      //---------------------------------------------------------------------
      // The grid. Rows follow from columns and the *output* aspect, so a cell
      // is as square as an integer count allows and the characters are not
      // stretched.
      //---------------------------------------------------------------------
      const columns = columnsFromParam(params.get('columns'));
      const rows = Math.max(1, Math.round((columns * height) / width));

      copyBuffer.ensure(input.width, input.height, gl.RGBA16F);
      cellBuffer.ensure(columns, rows, gl.RGBA16F);

      gl.disable(gl.BLEND);

      //---------------------------------------------------------------------
      // 1. The picture, into a texture of ours, with a mip chain on it.
      //---------------------------------------------------------------------
      copyBuffer.bind();
      copyShader.use();
      bindTexture(gl, 0, input.texture);
      copyShader.setSampler('InputTexture', 0);
      copyShader.set('MaxUV', 1, 1);
      copyShader.set('HalfTexel', 0.5 / input.width, 0.5 / input.height);
      quad.draw();
      copyBuffer.generateMipmap();

      //---------------------------------------------------------------------
      // 2. One character per cell.
      //---------------------------------------------------------------------
      const cellWidthPixels = input.width / columns;
      const cellHeightPixels = input.height / rows;
      // The larger of the two axes, so a cell that is wide and short is
      // averaged over its whole width rather than aliasing along it.
      const subLod = lodForFootprint(Math.max(cellWidthPixels, cellHeightPixels) / 8);
      const cellLod = lodForFootprint(Math.max(cellWidthPixels, cellHeightPixels));

      cellBuffer.bind();
      cellShader.use();
      bindTexture(gl, 0, copyBuffer.texture);
      bindTexture(gl, 1, glyphTexture);
      cellShader.setSampler('CopyTexture', 0);
      cellShader.setSampler('GlyphTexture', 1);
      cellShader.set('CellCount', columns, rows);
      cellShader.set('SubLod', subLod);
      cellShader.setInt('GlyphCount', alphabetSize);

      cellShader.set('Gamma', gammaFromParam(params.get('tone')));
      cellShader.set('Contrast', contrastFromParam(params.get('contrast')));
      cellShader.set('Invert', params.get('invert'));
      cellShader.set('Structure', params.get('structure'));
      cellShader.set('Dither', params.get('dither'));
      cellShader.set('CoverLow', coverLow);
      cellShader.set('CoverHigh', coverHigh);
      quad.draw();

      //---------------------------------------------------------------------
      // 3. Draw them.
      //---------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);

      typeShader.use();

      // Magnified, a character should be a grid of hard pixels, because that is
      // what a character on a screen was. Minified — a hundred columns into a
      // small preview — hard pixels alias into a shimmer, and this is the
      // control for it rather than something guessed at from the cell size.
      const smooth = params.get('edge') > 0.5;
      gl.bindTexture(gl.TEXTURE_2D, atlasTexture);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, smooth ? gl.LINEAR : gl.NEAREST);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, smooth ? gl.LINEAR : gl.NEAREST);
      gl.bindTexture(gl.TEXTURE_2D, null);

      bindTexture(gl, 0, cellBuffer.texture);
      bindTexture(gl, 1, atlasTexture);
      bindTexture(gl, 2, copyBuffer.texture);
      typeShader.setSampler('CellTexture', 0);
      typeShader.setSampler('AtlasTexture', 1);
      typeShader.setSampler('CopyTexture', 2);

      typeShader.set('CellCount', columns, rows);
      typeShader.set('AtlasSlots', ATLAS_COLS, ATLAS_ROWS);
      typeShader.set('AtlasSize', ATLAS_W, ATLAS_H);
      typeShader.set('CellLod', cellLod);

      typeShader.set('InkColour', params.get('inkR'), params.get('inkG'), params.get('inkB'));
      typeShader.set('PaperColour', params.get('paperR'), params.get('paperG'), params.get('paperB'));
      typeShader.set('PaperOpacity', params.get('paperOpacity'));
      typeShader.set('Tint', params.get('tint'));
      typeShader.set('Mix', params.get('mix'));
      quad.draw();

      bindTexture(gl, 2, null);
      bindTexture(gl, 1, null);
      bindTexture(gl, 0, null);
    },
  };
}

//===========================================================================

const pct = (v) => `${Math.round(v * 100)}%`;

mountDemo({
  name: 'Asciify',
  pluginId: 'AS01',
  tagline:
    'A character renderer, not a brightness ramp. A cell is a small picture, and the glyph that stands in for it is the one whose ink is distributed most like it — coverage and five moments, measured on the same grid for the cell and for every character.',
  repo: 'https://github.com/stoatworks-labs/asciify',
  page: 'https://stoatworks-labs.com/software/asciify/',
  video: 'https://www.youtube.com/watch?v=Hzy60YIhKpg',

  needFloat: true,
  showBackdrop: true,

  params: [
    {
      id: 'columns', name: 'Columns', type: 'standard', default: 0.624, group: 'Type',
      display: (v) => `${columnsFromParam(v)}`,
      hint: '8 to 320, geometric — equal slider travel gives an equal ratio.',
    },
    {
      id: 'set', name: 'Characters', type: 'option', default: 0, group: 'Type',
      elements: SET_NAMES,
      hint: 'Every set uses its whole range: tone maps into the measured min and max coverage of whatever is selected.',
    },
    {
      id: 'custom', name: 'Custom Set', type: 'text', default: '@%#*+=-:. ', group: 'Type',
      placeholder: '@%#*+=-:. ',
      hint: 'Only read when Characters is Custom. Left visible in the other modes on purpose.',
    },
    {
      id: 'structure', name: 'Structure', type: 'standard', default: 0.35, group: 'Type',
      display: pct,
      hint: 'How far the match may stray from the correct weight, as a fraction of the alphabet’s tonal range. At 0 this is exactly the classic ramp.',
    },
    {
      id: 'tone', name: 'Tone', type: 'standard', default: 0.5, group: 'Type',
      display: (v) => `γ ${gammaFromParam(v).toFixed(2)}`,
      hint: 'The centre is exactly gamma 1 — the only position where the tone curve does nothing.',
    },
    {
      id: 'contrast', name: 'Contrast', type: 'standard', default: 0.5, group: 'Type',
      display: (v) => `${contrastFromParam(v).toFixed(2)}×`,
    },
    { id: 'invert', name: 'Invert', type: 'boolean', default: 0, group: 'Type' },
    {
      id: 'dither', name: 'Dither', type: 'standard', default: 0.5, group: 'Type',
      display: pct,
      hint: 'Ordered dither by exactly one quantisation step of tone — the gap between two neighbours in the ramp.',
    },

    {
      id: 'tint', name: 'Tint', type: 'standard', default: 0.0, group: 'Colour',
      display: pct,
      hint: '0 takes the ink colour, 1 takes the picture’s own.',
    },
    { id: 'inkR', name: 'Ink', type: 'colour', default: 0.6, group: 'Colour' },
    { id: 'inkG', name: 'Ink_Green', type: 'colour', default: 1.0, group: 'Colour' },
    { id: 'inkB', name: 'Ink_Blue', type: 'colour', default: 0.7, group: 'Colour' },
    { id: 'paperR', name: 'Paper', type: 'colour', default: 0.02, group: 'Colour' },
    { id: 'paperG', name: 'Paper_Green', type: 'colour', default: 0.05, group: 'Colour' },
    { id: 'paperB', name: 'Paper_Blue', type: 'colour', default: 0.03, group: 'Colour' },
    { id: 'paperOpacity', name: 'Paper Opacity', type: 'standard', default: 1.0, group: 'Colour', display: pct },

    {
      id: 'edge', name: 'Glyph Edge', type: 'option', default: 0, group: 'Output',
      elements: ['Crisp', 'Smooth'],
    },
    { id: 'mix', name: 'Mix', type: 'standard', default: 1.0, group: 'Output', display: pct },
  ],

  sources: ['scene', 'detail', 'spot', 'grid', 'ramp', 'bars', 'alpha'],

  presets: {
    'Green terminal (defaults)': {},
    'Classic ramp, no structure': { set: 4, structure: 0, columns: 0.55, inkR: 0.9, inkG: 0.9, inkB: 0.9 },
    'Structure at full': { set: 0, structure: 1, columns: 0.55 },
    'Box drawing — all structure, no tone': { set: 7, structure: 0.8, columns: 0.45 },
    'Blocks — the full tonal range': { set: 6, structure: 0.2, columns: 0.7, tint: 1 },
    'Binary rain': { set: 5, columns: 0.68, structure: 0, inkR: 0.2, inkG: 1, inkB: 0.4 },
    'Paper and ink': {
      inkR: 0.08, inkG: 0.07, inkB: 0.06, paperR: 0.93, paperG: 0.91, paperB: 0.86, columns: 0.6,
    },
  },

  differences: [
    'The plugin’s Custom Set is an FF_TYPE_TEXT parameter, which the SDK supports and Resolume’s own example uses — but how it presents in Arena has never been seen by the author. Here it is an ordinary text field, which is a guess about the host, not a claim about it.',
    'Two of the plugin’s claims are checkable here: drag Structure to 0 and the result is exactly a tone ramp with no separate code path, and a flat area of the picture ignores Structure entirely because a cell with no direction has nothing to be wrong about.',
    'The font is extracted from source/FontData.cpp by demo/tools/extract_font.py rather than redrawn, and the ramp ordering on this page is computed from those bitmaps — so it is the plugin’s measured order, including the parts of it that are not the traditional ramp.',
    'The plugin’s own proof is asctest --match, which renders at exactly one output pixel per glyph pixel and reads every cell back to name the character. That is 43 runs × 960 cells with zero disagreements, and it lives in the repository, not here.',
  ],

  createRenderer,
});
