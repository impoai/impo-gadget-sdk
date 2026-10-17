/* speaker.play_url: a public https MP3 or WAV, at most 1.5 MB, played through gadget_sound. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Starts downloading and returns at once; -1 with *why when refused. */
int gadget_play_url(const char *url, const char **why);
const char *gadget_play_url_last(void);   /* "" before the first */

#ifdef __cplusplus
}
#endif
