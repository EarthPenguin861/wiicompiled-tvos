#pragma once

// Race renderer cuts chosen in the settings bar (hle/render_toggles.cpp). Take effect at once.
namespace RenderToggles {
bool ShadowsEnabled();
void SetShadowsEnabled(bool enabled);
bool ParticlesEnabled();
void SetParticlesEnabled(bool enabled);
} // namespace RenderToggles
