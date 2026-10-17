/* speaker.say: speech from Impo (POST /tts on the link), played as it downloads. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Starts fetching and playing; returns at once. -1 when not connected. */
int gadget_speech_say(const char *text);

#ifdef __cplusplus
}
#endif
