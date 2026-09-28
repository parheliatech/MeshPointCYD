#pragma once
#include <Arduino.h>

// Alert sounds through the board's ES8311 codec + speaker amp. Tones are synthesized on the fly.
namespace audio {

enum class Sound : uint8_t {
    ChannelMessage,  // soft two-note chime
    DirectMessage,   // brighter rising four-note arpeggio
    NewNode,         // sonar-like rising sweep
};

// Call after panel::begin() (shares its I2C bus). Returns false if the codec didn't respond.
bool begin();
// Queue a sound; honours the mute/volume settings. Safe to call from any task.
void play(Sound s);
// Same, but ignores mute (for the Settings preview buttons).
void preview(Sound s);

}  // namespace audio
