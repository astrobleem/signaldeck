#ifndef SBPCM_H
#define SBPCM_H

enum sbpcm_result {
    SBPCM_OK = 0,
    SBPCM_CANCELED,
    SBPCM_FILE,
    SBPCM_FORMAT,
    SBPCM_DEVICE,
    SBPCM_CONFIG,
    SBPCM_IO
};

/* Runs in a playback child or standalone. SIGINT/SIGTERM cancel and halt. */
enum sbpcm_result sbpcm_play_file(const char *path);
const char *sbpcm_result_text(enum sbpcm_result result);

#endif
