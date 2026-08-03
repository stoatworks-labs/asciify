/**
 * The example content.
 *
 * Every clip on these pages is **generated in the page**, by the shaders below.
 * Nothing is downloaded, nothing is vendored, and there is no footage to
 * licence — which is the reason for doing it this way rather than shipping a
 * few seconds of video. It also means the demo weighs nothing and loops
 * seamlessly, and that a clip can be built to exercise the specific thing a
 * given plugin is for.
 *
 * The clips are deliberately not "nice pictures". Each one is chosen for what
 * it puts in front of a particular effect:
 *
 *   bars      SMPTE-style 75% / 100% bars — the only source where a scope's
 *             reading can be checked against a number you already know
 *   ramp      luma and per-channel ramps, for histogram and false colour
 *   grid      geometry: a square grid, concentric rings and corner marks, so a
 *             warp's shape is readable rather than a matter of taste
 *   detail    fine diagonal detail near the colour subcarrier, which is what
 *             actually produces cross-colour in a composite chain
 *   scene     a moving synthetic scene with real contrast range — the general
 *             "some footage" case
 *   spot      a bright moving object on near-black, for a luminance key
 *   alpha     a shape over genuine transparency, premultiplied on the way out,
 *             so the alpha paths get exercised at all
 *
 * All of them are functions of (uv, Time) alone: any frame renders on its own,
 * so pausing, scrubbing and stepping are exact rather than approximate.
 *
 * Output is **premultiplied**, because that is what Resolume's engine hands an
 * FFGL plugin and several of these plugins unpremultiply on the way in. Getting
 * that wrong would make the demo disagree with the plugin in exactly the place
 * the plugins' own notes warn about.
 */

import { Program, Quad, PassBuffer } from './gl.js';

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

const COMMON = `
uniform float Time;
uniform vec2 Resolution;
in vec2 uv;
out vec4 fragColor;

float hash11( float p )
{
	p = fract( p * 0.1031 );
	p *= p + 33.33;
	return fract( p * ( p + p ) );
}

float hash21( vec2 p )
{
	vec3 p3 = fract( vec3( p.xyx ) * 0.1031 );
	p3 += dot( p3, p3.yzx + 33.33 );
	return fract( ( p3.x + p3.y ) * p3.z );
}

float noise( vec2 p )
{
	vec2 i = floor( p );
	vec2 f = fract( p );
	f = f * f * ( 3.0 - 2.0 * f );
	return mix( mix( hash21( i ), hash21( i + vec2( 1.0, 0.0 ) ), f.x ),
	            mix( hash21( i + vec2( 0.0, 1.0 ) ), hash21( i + vec2( 1.0, 1.0 ) ), f.x ), f.y );
}

float fbm( vec2 p )
{
	float sum = 0.0;
	float amp = 0.5;
	for( int i = 0; i < 5; ++i )
	{
		sum += noise( p ) * amp;
		p *= 2.02;
		amp *= 0.5;
	}
	return sum;
}

/// Premultiply on the way out, which is the invariant the host holds.
vec4 emit( vec3 rgb, float alpha )
{
	rgb = clamp( rgb, vec3( 0.0 ), vec3( 1.0 ) );
	return vec4( rgb * alpha, alpha );
}
`;

/** SMPTE-style bars: the top two thirds are the bar field, then the pluge strip. */
const BARS = `${COMMON}
uniform float BarLevel;  //0.75 or 1.0

vec3 barColour( int i, float level )
{
	if( i == 0 ) return vec3( level );
	if( i == 1 ) return vec3( level, level, 0.0 );
	if( i == 2 ) return vec3( 0.0, level, level );
	if( i == 3 ) return vec3( 0.0, level, 0.0 );
	if( i == 4 ) return vec3( level, 0.0, level );
	if( i == 5 ) return vec3( level, 0.0, 0.0 );
	return vec3( 0.0, 0.0, level );
}

void main()
{
	//uv.y runs up; the bars are described from the top down.
	float y = 1.0 - uv.y;
	int column = int( clamp( floor( uv.x * 7.0 ), 0.0, 6.0 ) );

	if( y < 0.67 )
	{
		fragColor = emit( barColour( column, BarLevel ), 1.0 );
		return;
	}

	if( y < 0.75 )
	{
		//Reverse bars, at full blue, the way the standard strip runs.
		vec3 c = barColour( 6 - column, BarLevel );
		fragColor = emit( c.bgr * 0.55, 1.0 );
		return;
	}

	//Pluge: black, super-black-ish, black, white patch.
	float x = uv.x;
	vec3 c = vec3( 0.0 );
	if( x < 0.20 )      c = vec3( 0.043 );
	else if( x < 0.35 ) c = vec3( BarLevel );
	else if( x < 0.50 ) c = vec3( 0.0 );
	else if( x < 0.57 ) c = vec3( 0.016 );
	else if( x < 0.64 ) c = vec3( 0.043 );
	else if( x < 0.71 ) c = vec3( 0.070 );
	else                c = vec3( 0.043 );
	fragColor = emit( c, 1.0 );
}
`;

/** Ramps: luma across the top, the three channels below it, steps at the foot. */
const RAMP = `${COMMON}
void main()
{
	float y = 1.0 - uv.y;
	float x = uv.x;

	vec3 c;
	if( y < 0.25 )      c = vec3( x );
	else if( y < 0.40 ) c = vec3( x, 0.0, 0.0 );
	else if( y < 0.55 ) c = vec3( 0.0, x, 0.0 );
	else if( y < 0.70 ) c = vec3( 0.0, 0.0, x );
	else if( y < 0.85 ) c = vec3( floor( x * 11.0 ) / 10.0 );
	else
	{
		//A slow sweep so a histogram has something that moves.
		float t = fract( x + Time * 0.05 );
		c = vec3( 0.5 + 0.5 * cos( 6.2831853 * ( t + vec3( 0.0, 0.33, 0.67 ) ) ) );
	}

	fragColor = emit( c, 1.0 );
}
`;

/** Geometry: grid, rings, diagonals and corner marks. */
const GRID = `${COMMON}
float line( float d, float w )
{
	return 1.0 - smoothstep( 0.0, w, abs( d ) );
}

void main()
{
	float aspect = Resolution.x / max( Resolution.y, 1.0 );
	vec2 p = ( uv - 0.5 ) * vec2( aspect, 1.0 ) * 2.0;
	float px = 2.0 / max( Resolution.y, 1.0 );

	vec3 c = vec3( 0.05, 0.055, 0.07 );

	//Square grid, tenth of the height.
	vec2 g = abs( fract( p * 5.0 ) - 0.5 ) / 5.0;
	float grid = max( line( g.x, px * 1.2 ), line( g.y, px * 1.2 ) );
	c = mix( c, vec3( 0.55, 0.58, 0.62 ), grid * 0.7 );

	//Centre cross, brighter.
	c = mix( c, vec3( 0.95 ), max( line( p.x, px * 1.6 ), line( p.y, px * 1.6 ) ) );

	//Concentric rings: a radial warp keeps them circular, so they are the
	//check that the map really is radial.
	float r = length( p );
	for( int i = 1; i <= 4; ++i )
	{
		c = mix( c, vec3( 0.30, 0.85, 0.95 ), line( r - float( i ) * 0.2, px * 1.4 ) );
	}

	//Diagonals to the corners.
	c = mix( c, vec3( 0.95, 0.72, 0.25 ), line( p.y - p.x / aspect, px * 1.2 ) * 0.8 );
	c = mix( c, vec3( 0.95, 0.72, 0.25 ), line( p.y + p.x / aspect, px * 1.2 ) * 0.8 );

	//Corner blocks, so an overscan or a crop is obvious.
	vec2 corner = abs( p ) - vec2( aspect, 1.0 ) + 0.16;
	if( corner.x > 0.0 && corner.y > 0.0 )
		c = vec3( 0.9, 0.25, 0.35 );

	//A slowly rotating marker, so it is clear the clip is running.
	float a = atan( p.y, p.x ) - Time * 0.35;
	float spoke = smoothstep( 0.02, 0.0, abs( fract( a / 6.2831853 + 0.5 ) - 0.5 ) ) * step( r, 0.18 );
	c = mix( c, vec3( 1.0 ), spoke );

	fragColor = emit( c, 1.0 );
}
`;

/** Fine diagonal detail: what actually produces cross-colour in a composite chain. */
const DETAIL = `${COMMON}
void main()
{
	float aspect = Resolution.x / max( Resolution.y, 1.0 );
	vec2 p = ( uv - 0.5 ) * vec2( aspect, 1.0 );

	//Frequency sweep across the frame: coarse on the left, fine on the right,
	//so the point where the chain stops resolving is visible as a place rather
	//than as a judgement.
	float freq = mix( 20.0, 420.0, uv.x * uv.x );
	float herring = sin( ( p.x * cos( 0.6 ) + p.y * sin( 0.6 ) ) * freq );
	float bars = sin( p.y * 180.0 );

	float y = 1.0 - uv.y;
	float v;
	if( y < 0.45 )      v = 0.5 + 0.42 * herring;
	else if( y < 0.62 ) v = 0.5 + 0.42 * bars;
	else if( y < 0.78 ) v = 0.5 + 0.42 * sin( p.x * freq );
	else                v = fbm( uv * 26.0 + Time * 0.1 );

	vec3 c = vec3( v );

	//One saturated patch, so chroma has something to smear.
	if( y > 0.30 && y < 0.45 && uv.x > 0.55 && uv.x < 0.85 )
		c = vec3( 0.75, 0.12, 0.18 ) * ( 0.6 + 0.6 * herring );

	fragColor = emit( c, 1.0 );
}
`;

/** A moving synthetic scene: sky, terrain, sun and drifting cloud. */
const SCENE = `${COMMON}
void main()
{
	float aspect = Resolution.x / max( Resolution.y, 1.0 );
	vec2 p = vec2( ( uv.x - 0.5 ) * aspect, uv.y - 0.5 );
	float drift = Time * 0.035;

	//Sky, dark at the top.
	vec3 c = mix( vec3( 0.30, 0.20, 0.34 ), vec3( 0.04, 0.05, 0.13 ), clamp( uv.y * 1.2 - 0.1, 0.0, 1.0 ) );

	//Sun, low and bright: the highlight everything downstream reacts to.
	vec2 sun = vec2( -0.22 + 0.10 * sin( Time * 0.11 ), -0.02 );
	float sd = length( ( p - sun ) * vec2( 1.0, 1.1 ) );
	c += vec3( 1.00, 0.62, 0.30 ) * smoothstep( 0.30, 0.0, sd ) * 0.55;
	c = mix( c, vec3( 1.0, 0.93, 0.80 ), smoothstep( 0.075, 0.055, sd ) );

	//Cloud banks, drifting.
	float cloud = fbm( vec2( p.x * 2.2 + drift, p.y * 3.4 + 3.0 ) );
	float band = smoothstep( 0.05, 0.32, uv.y - 0.42 ) * smoothstep( 0.95, 0.62, uv.y );
	c = mix( c, vec3( 0.86, 0.72, 0.72 ), clamp( ( cloud - 0.45 ) * 2.4, 0.0, 1.0 ) * band * 0.75 );

	//Terrain: two ridges, the near one darker, with a hard silhouette edge.
	float far = 0.30 + 0.055 * fbm( vec2( p.x * 1.6 + 11.0, 2.0 ) ) * 3.0;
	float near = 0.20 + 0.075 * fbm( vec2( p.x * 3.1 + drift * 0.4 + 41.0, 7.0 ) ) * 3.0;

	if( uv.y < far )  c = mix( c, vec3( 0.10, 0.09, 0.16 ), 0.92 );
	if( uv.y < near )
	{
		float texture = fbm( vec2( p.x * 26.0, uv.y * 40.0 ) );
		c = mix( vec3( 0.030, 0.035, 0.055 ), vec3( 0.075, 0.070, 0.085 ), texture );
	}

	//A light travelling along the ridge, so there is a moving hard edge.
	float lampX = fract( Time * 0.06 ) * 2.0 - 1.0;
	float ld = length( ( p - vec2( lampX * aspect * 0.9, near - 0.5 + 0.02 ) ) * vec2( 1.0, 2.0 ) );
	c += vec3( 0.95, 0.85, 0.55 ) * smoothstep( 0.045, 0.0, ld );

	fragColor = emit( c, 1.0 );
}
`;

/** A bright moving object on near-black: the case a luminance key is for. */
const SPOT = `${COMMON}
void main()
{
	float aspect = Resolution.x / max( Resolution.y, 1.0 );
	vec2 p = vec2( ( uv.x - 0.5 ) * aspect, uv.y - 0.5 );

	//Near-black ground with a little structure, so a key with too low a
	//threshold shows itself instead of looking clean.
	vec3 c = vec3( 0.020, 0.024, 0.030 ) + fbm( uv * 9.0 + Time * 0.05 ) * 0.045;

	//Three blobs at different brightnesses, moving on their own paths — so
	//Threshold has something to separate rather than one all-or-nothing shape.
	const int N = 3;
	for( int i = 0; i < N; ++i )
	{
		float fi = float( i );
		float speed = 0.19 + fi * 0.07;
		vec2 centre = vec2( 0.42 * aspect * sin( Time * speed + fi * 2.1 ),
		                    0.30 * cos( Time * speed * 0.83 + fi * 1.3 ) );
		float d = length( p - centre );
		float body = smoothstep( 0.20 + fi * 0.02, 0.02, d );
		vec3 tint = i == 0 ? vec3( 1.00, 0.96, 0.90 )
		          : i == 1 ? vec3( 0.42, 0.78, 1.00 )
		                   : vec3( 1.00, 0.55, 0.28 );
		c += tint * body * ( 0.95 - fi * 0.28 );
	}

	//A soft gradient wash, so mid-luminance exists and Softness is readable.
	c += vec3( 0.10, 0.12, 0.16 ) * smoothstep( 0.8, -0.2, uv.y );

	fragColor = emit( c, 1.0 );
}
`;

/** A shape over real transparency, so the alpha paths are exercised. */
const ALPHA = `${COMMON}
void main()
{
	float aspect = Resolution.x / max( Resolution.y, 1.0 );
	vec2 p = vec2( ( uv.x - 0.5 ) * aspect, uv.y - 0.5 );

	//A rounded rectangle that drifts, plus a ring, both with soft alpha edges.
	vec2 q = abs( p - vec2( 0.18 * sin( Time * 0.23 ), 0.10 * cos( Time * 0.31 ) ) ) - vec2( 0.34, 0.20 );
	float box = length( max( q, vec2( 0.0 ) ) ) + min( max( q.x, q.y ), 0.0 ) - 0.07;

	float ring = abs( length( p + vec2( 0.30 * cos( Time * 0.17 ), 0.0 ) ) - 0.22 ) - 0.035;

	float a = max( smoothstep( 0.012, -0.012, box ), smoothstep( 0.010, -0.010, ring ) );

	vec3 c = mix( vec3( 0.20, 0.85, 0.95 ), vec3( 0.98, 0.80, 0.30 ), clamp( uv.x, 0.0, 1.0 ) );
	c *= 0.55 + 0.55 * fbm( uv * 7.0 - Time * 0.08 );

	fragColor = emit( c, a );
}
`;

export const SOURCES = [
  { id: 'scene', name: 'Synthetic scene', hint: 'Moving picture with a full contrast range — the general case.', fragment: SCENE },
  { id: 'bars', name: 'Colour bars', hint: 'The only clip whose correct reading on a scope is a number you already know.', fragment: BARS, uniforms: { BarLevel: 0.75 } },
  { id: 'bars100', name: 'Colour bars 100%', hint: 'Same field at full amplitude.', fragment: BARS, uniforms: { BarLevel: 1.0 } },
  { id: 'ramp', name: 'Ramps and steps', hint: 'Luma and per-channel ramps, an 11-step staircase and a moving sweep.', fragment: RAMP },
  { id: 'grid', name: 'Geometry card', hint: 'Grid, rings, diagonals and corner blocks — for reading the shape of a warp.', fragment: GRID },
  { id: 'detail', name: 'Fine detail', hint: 'A frequency sweep into the region that produces cross-colour.', fragment: DETAIL },
  { id: 'spot', name: 'Lights on black', hint: 'Bright objects on near-black, at three different levels.', fragment: SPOT },
  { id: 'alpha', name: 'Shape on transparency', hint: 'Genuine alpha, premultiplied on the way out.', fragment: ALPHA },
];

/**
 * Renders the chosen clip into a texture the plugin pass can read, exactly as
 * the host hands an FFGL effect its input.
 *
 * A user-supplied still or video takes the same path — it is uploaded into the
 * same texture and never leaves the page.
 */
export class SourceRenderer {
  constructor(gl, quad = null) {
    this.gl = gl;
    this.quad = quad ?? new Quad(gl);
    this.ownsQuad = quad === null;
    this.programs = new Map();
    this.buffer = new PassBuffer(gl, { filter: 'linear' });
    this.userTexture = null;
    this.userMedia = null;
    this.userSize = [0, 0];
  }

  program(source) {
    if (!this.programs.has(source.id)) {
      this.programs.set(source.id, new Program(this.gl, VERTEX, `#version 410 core\n${source.fragment}`, `source:${source.id}`));
    }
    return this.programs.get(source.id);
  }

  /** Adopt a user's own image or video. Stays in the page; nothing is uploaded anywhere. */
  async adopt(file) {
    const gl = this.gl;
    this.releaseUser();

    if (file.type.startsWith('video/')) {
      const video = document.createElement('video');
      video.src = URL.createObjectURL(file);
      video.loop = true;
      video.muted = true;
      video.playsInline = true;
      await video.play();
      this.userMedia = video;
      this.userSize = [video.videoWidth, video.videoHeight];
    } else {
      const bitmap = await createImageBitmap(file, { imageOrientation: 'flipY', premultiplyAlpha: 'premultiply' });
      this.userMedia = bitmap;
      this.userSize = [bitmap.width, bitmap.height];
    }

    this.userTexture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.userTexture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.bindTexture(gl.TEXTURE_2D, null);
    return this.userSize;
  }

  releaseUser() {
    if (this.userMedia instanceof HTMLVideoElement) {
      URL.revokeObjectURL(this.userMedia.src);
      this.userMedia.pause();
    } else if (this.userMedia && 'close' in this.userMedia) {
      this.userMedia.close();
    }
    if (this.userTexture) this.gl.deleteTexture(this.userTexture);
    this.userMedia = null;
    this.userTexture = null;
  }

  /**
   * @returns {{texture: WebGLTexture, width: number, height: number}} the input
   *          the plugin pass reads, premultiplied, uv origin bottom-left.
   */
  render(source, width, height, time) {
    const gl = this.gl;

    if (source === 'user' && this.userTexture) {
      gl.bindTexture(gl.TEXTURE_2D, this.userTexture);
      // flipY on upload for video; the ImageBitmap was decoded flipped already.
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, this.userMedia instanceof HTMLVideoElement);
      gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, this.userMedia);
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
      gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
      gl.bindTexture(gl.TEXTURE_2D, null);
      return { texture: this.userTexture, width: this.userSize[0], height: this.userSize[1] };
    }

    this.buffer.ensure(width, height, gl.RGBA8);
    this.buffer.bind();

    const program = this.program(source).use();
    program.set('Time', time);
    program.set('Resolution', width, height);
    for (const [name, value] of Object.entries(source.uniforms ?? {})) {
      program.set(name, value);
    }

    gl.disable(gl.BLEND);
    this.quad.draw();

    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    return { texture: this.buffer.texture, width: this.buffer.width, height: this.buffer.height };
  }

  dispose() {
    this.releaseUser();
    for (const p of this.programs.values()) p.dispose();
    this.programs.clear();
    this.buffer.dispose();
    if (this.ownsQuad) this.quad.dispose();
  }
}
