#pragma once

#include <FFGLSDK.h>

namespace asciify
{
/**
    An off-screen buffer for one stage of the chain.

    Three things on top of the SDK's FFGLFBO.

    **It reallocates only when it has to.** Ensure() is called every frame and
    is a no-op in the overwhelming majority of them. The cell buffer changes
    size whenever the Columns control moves, which an operator may well be
    dragging, so this path has to be cheap.

    **It actually frees its colour texture.** `ffglex::FFGLFBO::Release()`
    deletes the framebuffer and the depth renderbuffer, then tests
    `depthBufferID` a second time where it plainly meant `colorTextureID` --
    so the colour texture is leaked on every release (SDK b1afaf9,
    `FFGLFBO.cpp`). One leak would not matter; dragging Columns across its
    range reallocates the cell buffer a few hundred times.

    **It owns its filtering.** The base class sets GL_LINEAR on both filters and
    that is wrong for both of this plugin's buffers, in opposite directions:
    the copy needs a mip chain to average a cell in one fetch, and the cell
    buffer needs GL_NEAREST because its alpha channel is a glyph index rather
    than a quantity -- interpolate it across a cell boundary and the result
    addresses a third character that neither cell chose.
*/
class PassBuffer : public ffglex::FFGLFBO
{
public:
	enum class Sampling
	{
		Nearest,  ///< for data. No filtering of any kind, no mip chain.
		Mipmapped ///< for pictures. Trilinear, with GenerateMipmaps() per frame.
	};

	~PassBuffer();

	/// Allocate at this size and format, reusing the existing buffer if it
	/// already matches. Newly allocated buffers are cleared: a buffer whose
	/// contents are undefined is not "a bit of noise on the first frame", it is
	/// whatever texture memory the driver handed back.
	bool Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint format, Sampling sampling );

	/// Rebuild the mip chain from level 0. Call after rendering into a
	/// Sampling::Mipmapped buffer and before anything samples it; a stale chain
	/// does not look like an error, it looks like the wrong footage.
	void GenerateMipmaps();

	/// Release everything, including the colour texture the SDK forgets.
	void Destroy();

	bool IsValid() const
	{
		return GetGLID() != 0;
	}

private:
	Sampling sampling = Sampling::Nearest;
};

} // namespace asciify
