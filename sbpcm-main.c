#include <stdio.h>
#include "sbpcm.h"

int main(int argc, char **argv)
{
    enum sbpcm_result status;
    if (argc != 2) {
        fprintf(stderr, "Usage: sbpcm FILE.WAV\n");
        return 2;
    }
    status = sbpcm_play_file(argv[1]);
    fprintf(status == SBPCM_OK ? stdout : stderr, "%s\n", sbpcm_result_text(status));
    return status;
}
