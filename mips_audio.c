#include "mips_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// SDL2 Audio Callback - mixes all active virtual PSP audio channels
static void sdl2_audio_callback(void *userdata, uint8_t *stream, int len) {
    MIPS_AudioSystem *audio = (MIPS_AudioSystem *)userdata;
    memset(stream, 0, len);

    int num_samples = len / sizeof(int16_t); // Stereo PCM16 stream has 2 samples per frame
    int16_t *out_buf = (int16_t *)stream;

    SDL_LockMutex(audio->mutex);

    // We mix active channels into the output stream
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        MIPS_AudioChannel *chan = &audio->channels[i];
        if (!chan->active || chan->queue_avail <= 0) continue;
        
        // Volume scale factors (normalized from 0x8000 maximum PSP volume to float)
        float vol_l = (float)chan->left_volume / 32768.0f;
        float vol_r = (float)chan->right_volume / 32768.0f;
        
        for (int s = 0; s < num_samples; s += 2) {
            if (chan->queue_avail < (chan->format == 1 ? 2 : 1)) break;
            
            int16_t sample_l = 0;
            int16_t sample_r = 0;
            
            if (chan->format == 1) { // Stereo input
                sample_l = chan->queue[chan->queue_read_ptr++];
                if (chan->queue_read_ptr >= chan->queue_capacity) chan->queue_read_ptr = 0;
                sample_r = chan->queue[chan->queue_read_ptr++];
                if (chan->queue_read_ptr >= chan->queue_capacity) chan->queue_read_ptr = 0;
                chan->queue_avail -= 2;
            } else { // Mono input (expand to stereo)
                int16_t m = chan->queue[chan->queue_read_ptr++];
                if (chan->queue_read_ptr >= chan->queue_capacity) chan->queue_read_ptr = 0;
                sample_l = m;
                sample_r = m;
                chan->queue_avail -= 1;
            }
            
            // Mix with panning and clamp to 16-bit limits
            int32_t mixed_l = out_buf[s] + (int32_t)(sample_l * vol_l);
            int32_t mixed_r = out_buf[s + 1] + (int32_t)(sample_r * vol_r);
            
            if (mixed_l > 32767) mixed_l = 32767;
            else if (mixed_l < -32768) mixed_l = -32768;
            
            if (mixed_r > 32767) mixed_r = 32767;
            else if (mixed_r < -32768) mixed_r = -32768;
            
            out_buf[s] = (int16_t)mixed_l;
            out_buf[s + 1] = (int16_t)mixed_r;
        }
    }

    SDL_UnlockMutex(audio->mutex);
}

int mips_audio_init(MIPS_AudioSystem *audio) {
    if (audio == NULL) return -1;
    memset(audio, 0, sizeof(MIPS_AudioSystem));

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "[SDL Audio Error] Failed to initialize audio: %s\n", SDL_GetError());
        return -2;
    }

    audio->mutex = SDL_CreateMutex();
    if (audio->mutex == NULL) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return -3;
    }

    // Configure standard 44.1kHz Stereo PCM16 device
    audio->spec.freq = AUDIO_SAMPLE_RATE;
    audio->spec.format = AUDIO_S16SYS; // Signed 16-bit PCM native endian
    audio->spec.channels = 2;          // Stereo
    audio->spec.samples = 1024;        // Latency sizing
    audio->spec.callback = sdl2_audio_callback;
    audio->spec.userdata = audio;

    audio->device = SDL_OpenAudioDevice(NULL, 0, &audio->spec, NULL, 0);
    if (audio->device == 0) {
        fprintf(stderr, "[SDL Audio Error] Failed to open audio device: %s\n", SDL_GetError());
        SDL_DestroyMutex(audio->mutex);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return -4;
    }

    // Allocate channel buffers
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        MIPS_AudioChannel *chan = &audio->channels[i];
        chan->active = false;
        chan->queue_capacity = AUDIO_SAMPLE_RATE * 2; // Buffer up to 2 seconds of audio
        chan->queue = (int16_t *)calloc(chan->queue_capacity, sizeof(int16_t));
        chan->queue_write_ptr = 0;
        chan->queue_read_ptr = 0;
        chan->queue_avail = 0;
    }

    // Start playback immediately
    SDL_PauseAudioDevice(audio->device, 0);

    audio->initialized = true;
    printf("[Audio GLES] SDL2 Multi-channel Mixer initialized successfully (44100Hz Stereo).\n");
    return 0;
}

void mips_audio_free(MIPS_AudioSystem *audio) {
    if (audio == NULL || !audio->initialized) return;

    SDL_PauseAudioDevice(audio->device, 1);
    SDL_CloseAudioDevice(audio->device);
    SDL_DestroyMutex(audio->mutex);

    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        MIPS_AudioChannel *chan = &audio->channels[i];
        if (chan->queue) {
            free(chan->queue);
            chan->queue = NULL;
        }
    }

    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    audio->initialized = false;
    printf("[Audio GLES] Audio subsystem shut down cleanly.\n");
}

int mips_audio_reserve_channel(MIPS_AudioSystem *audio, int channel, int sample_count, int format) {
    if (audio == NULL || !audio->initialized) return -1;
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return -2;

    SDL_LockMutex(audio->mutex);
    MIPS_AudioChannel *chan = &audio->channels[channel];
    chan->active = true;
    chan->format = format;
    chan->sample_count = sample_count;
    chan->left_volume = 0x8000;  // Maximum default volume
    chan->right_volume = 0x8000;
    chan->queue_write_ptr = 0;
    chan->queue_read_ptr = 0;
    chan->queue_avail = 0;
    SDL_UnlockMutex(audio->mutex);

    printf("[HLE Audio] Channel %d reserved successfully (Format: %s, Max Samples: %d).\n",
           channel, format == 1 ? "Stereo" : "Mono", sample_count);
    return channel;
}

int mips_audio_release_channel(MIPS_AudioSystem *audio, int channel) {
    if (audio == NULL || !audio->initialized) return -1;
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return -2;

    // Sychronous wait strategy: let the background SDL thread consume remaining PCM frames
    while (1) {
        SDL_LockMutex(audio->mutex);
        int remaining = audio->channels[channel].queue_avail;
        SDL_UnlockMutex(audio->mutex);
        
        if (remaining <= 0) {
            break;
        }
        // Wait 5 milliseconds before checking again (ALSA/SDL plays in chunks)
        SDL_Delay(5);
    }

    SDL_LockMutex(audio->mutex);
    audio->channels[channel].active = false;
    SDL_UnlockMutex(audio->mutex);

    printf("[HLE Audio] Channel %d released cleanly.\n", channel);
    return 0;
}

int mips_audio_output_panned(MIPS_AudioSystem *audio, int channel, int left_vol, int right_vol, const int16_t *samples, int length) {
    if (audio == NULL || !audio->initialized || samples == NULL || length <= 0) return -1;
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return -2;

    SDL_LockMutex(audio->mutex);
    MIPS_AudioChannel *chan = &audio->channels[channel];
    if (!chan->active) {
        SDL_UnlockMutex(audio->mutex);
        return -3;
    }

    chan->left_volume = left_vol;
    chan->right_volume = right_vol;

    int free_space = chan->queue_capacity - chan->queue_avail;
    int write_len = length;
    if (write_len > free_space) {
        write_len = free_space; // Drop overflow frames to keep sync
    }

    for (int i = 0; i < write_len; i++) {
        chan->queue[chan->queue_write_ptr++] = samples[i];
        if (chan->queue_write_ptr >= chan->queue_capacity) {
            chan->queue_write_ptr = 0;
        }
    }
    chan->queue_avail += write_len;

    SDL_UnlockMutex(audio->mutex);
    return write_len;
}
