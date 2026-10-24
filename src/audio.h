// Sound: the web version's synthesised effects (filtered noise bursts and
// pitch-swept oscillators through a compressor) and the StarCraft soundtrack
// (AAC, decoded with Windows Media Foundation), mixed by miniaudio.
#pragma once

namespace audio {
void init(float volume, bool muted, bool musicOn);   // returns at once: the device opens on its own thread (silent if none, logged)
void shutdown();
void play(const char* name);          // an effect by name (rate-limited like the web version)
void setVolume(float v);              // 0..1; a non-zero volume also unmutes
void setMuted(bool m);
float volume();
bool muted();
// Music scenes: "title" · "battle" (both loop) · "victory" · "defeat"; a scene already playing keeps going
void music(const char* scene);
void setMusicOn(bool on);
bool musicOn();
void setSuspended(bool s);            // window minimised: the music pauses
void setRain(float level);            // the rain's sound, 0 (none) .. 1 (a downpour); it eases to a new level
}
