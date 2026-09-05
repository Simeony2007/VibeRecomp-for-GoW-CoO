#ifndef MIPS_AUDIO_H
#define MIPS_AUDIO_H

#include "mips_cpu.h"
#include <SDL2/SDL.h>
#include <stdbool.h>

#define AUDIO_MAX_CHANNELS  8
#define AUDIO_SAMPLE_RATE   44100
#define AUDIO_BUFFER_SIZE   4096

typedef struct {
    bool active;
    int format;             // 0: Mono, 1: Stereo
    int sample_count;       // Number of samples per block (e.g. 512 or 1024)
    int left_volume;        // 0 to 0x8000 (standard PSP volume range)
    int right_volume;       // 0 to 0x8000
    
    // Circular buffer for queuing incoming PCM16 samples
    int16_t *queue;
    int queue_write_ptr;
    int queue_read_ptr;
    int queue_capacity;
    int queue_avail;
} MIPS_AudioChannel;

typedef struct {
    MIPS_AudioChannel channels[AUDIO_MAX_CHANNELS];
    SDL_AudioSpec spec;
    SDL_AudioDeviceID device;
    SDL_mutex *mutex;
    bool initialized;
} MIPS_AudioSystem;

// Initialize the SDL2 audio mixer
int mips_audio_init(MIPS_AudioSystem *audio);

// Free audio resources
void mips_audio_free(MIPS_AudioSystem *audio);

// Reserves a channel with specific sample limits and layout (Mono vs Stereo)
int mips_audio_reserve_channel(MIPS_AudioSystem *audio, int channel, int sample_count, int format);

// Releases a reserved channel back to the pool
int mips_audio_release_channel(MIPS_AudioSystem *audio, int channel);

// Pushes PCM16 stereo samples to the reserved channel queue
int mips_audio_output_panned(MIPS_AudioSystem *audio, int channel, int left_vol, int right_vol, const int16_t *samples, int length);

#endif // MIPS_AUDIO_H
