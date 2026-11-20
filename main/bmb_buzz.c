// SPDX-License-Identifier: Apache-2.0
// One-shot speaker alert. A 40 ms-chunk renderer task would be overkill for a
// single beep: this writes a prepared 8 kHz tone buffer synchronously (about
// 320 ms) from the UI task, which is acceptable for a once-per-print alert.

#include "bmb_buzz.h"

#ifdef ESP_PLATFORM

#include "bsp/esp_mosaico.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

#define BMB_BEEP_HZ 880
#define BMB_BEEP_MS 320
#define BMB_BEEP_SAMPLES (BMB_BEEP_MS * 8)
#define BMB_BEEP_VOLUME 55

static const char *TAG = "bmb_buzz";
static esp_codec_dev_handle_t s_speaker;
static bool s_failed;

void bmb_buzz_once(void)
{
    static int16_t pcm[BMB_BEEP_SAMPLES];
    static bool prepared;
    if (!prepared) {
        for (int i = 0; i < BMB_BEEP_SAMPLES; ++i) {
            float t = (float)i / 8000.0f;
            float env = 1.0f;
            if (i < 80) {
                env = (float)i / 80.0f;
            } else if (i > BMB_BEEP_SAMPLES - 240) {
                env = (float)(BMB_BEEP_SAMPLES - i) / 240.0f;
            }
            pcm[i] = (int16_t)(sinf(2.0f * 3.1415926f * BMB_BEEP_HZ * t) * env *
                               32767.0f * 0.6f);
        }
        prepared = true;
    }
    if (s_failed) {
        return;
    }
    if (!s_speaker) {
        s_speaker = bsp_audio_codec_speaker_init();
        if (!s_speaker) {
            ESP_LOGE(TAG, "speaker init failed");
            s_failed = true;
            return;
        }
        esp_codec_dev_set_out_vol(s_speaker, BMB_BEEP_VOLUME);
        esp_codec_dev_sample_info_t info = {
            .sample_rate = 8000,
            .channel = 1,
            .bits_per_sample = 16,
        };
        if (esp_codec_dev_open(s_speaker, &info) != ESP_OK) {
            ESP_LOGE(TAG, "speaker open failed");
            s_failed = true;
            return;
        }
    }
    esp_codec_dev_write(s_speaker, pcm, sizeof(pcm));
}
#else
void bmb_buzz_once(void) {}
#endif
