#pragma once

/**
    Host parameters are 0..1; these are what they mean.

    Every parameter this plugin declares is a plain FF_TYPE_STANDARD float in
    0..1, including the ones that stand for a count of columns or a gamma
    exponent. That is not a style preference. `CFFGLPluginManager::SetParamInfo`
    clamps a standard default into 0..1 *before* returning, and `SetParamRange`
    can only be called afterwards because it finds the parameter by ID -- so a
    parameter declared in columns cannot declare a default in columns, and 80
    would silently become 1. The conversions live here instead, in one file that
    both the plugin and the test harness use, so there is only ever one answer
    to what a slider position means.
*/
namespace asciify
{
/// 8 to 320 characters across, logarithmically. Geometric because the
/// interesting range is at the coarse end -- the difference between 20 columns
/// and 40 is a different picture, the difference between 280 and 300 is not.
int ColumnsFromParam( float value );

/// 4 down to 0.25, with 1 at the centre of the slider. Applied to luminance
/// before the tone is quantised, so this decides which end of the alphabet the
/// midtones land in.
float GammaFromParam( float value );

/// 0.25 to 4 about mid grey, with 1 at the centre of the slider.
float ContrastFromParam( float value );

} // namespace asciify
