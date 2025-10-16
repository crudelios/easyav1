#include <stdio.h>
#include <easyav1.h>

int main(void)
{
    easyav1_t *easyav1 = easyav1_init_from_filename("video.webm", NULL);
    if (!easyav1) {
        fprintf(stderr, "Failed to initialize easyav1.\n");
        return 1;
    }

    while (easyav1_decode_next(easyav1) == EASYAV1_STATUS_OK) {
        if (easyav1_has_video_frame(easyav1) == EASYAV1_TRUE) {
            const easyav1_video_frame *vf = easyav1_get_video_frame(easyav1);
            printf("Decoded video frame: %ux%u (PTS=%llu)\n",
                   vf->properties.width, vf->properties.height, vf->timestamp);
        }

        const easyav1_audio_frame *af = easyav1_get_audio_frame(easyav1);
        if (af) {
            printf("Decoded audio frame: %u samples (PTS=%llu)\n", af->samples, af->timestamp);
        }
    }

    easyav1_destroy(&easyav1);
    return 0;
}
