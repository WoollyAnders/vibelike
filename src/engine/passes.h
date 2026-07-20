#pragma once
// The frame is composed in fixed passes. Ground through UI, in order:
//
//   Ground        floor diamonds (immediate, no overlap)
//   GroundDecals  flat-on-sand overlays: ripples, churn, highlights, shadows
//   Walls   \
//   Objects  |    populate ONE y-sorted scene list (iso occlusion demands
//   Entities/     walls/props/actors interleave by depth), flushed once
//   Effects       above-scene sprites: carryall shadows, flashes, markers
//   Particles     screen-space dust/sparks (engine/particles.h)
//   Lighting      per-tile light composition (M7; colormap remap hook)
//   Weather       sandstorm haze, palette weather (M5/M7 hook)
//   UI            HUD panel, banners, modals, death screen
//
// game_render() is organized as one function per pass; adding a rain of new
// visuals means picking the right pass, never re-ordering draw calls.

namespace passes {
enum class Pass : unsigned char {
    Ground, GroundDecals, Walls, Objects, Entities,
    Effects, Particles, Lighting, Weather, UI, Count,
};
}
