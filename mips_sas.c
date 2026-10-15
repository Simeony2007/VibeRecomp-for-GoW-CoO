#include "mips_hle.h"

#define SCE_SAS_ERROR_INVALID_VOICE 0x80420010u
#define SCE_SAS_ERROR_BAD_ADDRESS 0x80420005u
#define SCE_SAS_ERROR_INVALID_PARAMETER 0x80420014u
#define SCE_SAS_ERROR_VOICE_PAUSED 0x80420016u
#define SAS_VOICE_COUNT 32u
#define SAS_SAMPLES_PER_VAG_FRAME 28u

uint32_t mips_sas_set_voice(MIPS_SasState *sas, uint32_t voice,
                            uint32_t sample_addr, int32_t sample_size,
                            uint32_t loop) {
    if (sas == NULL || !sas->initialized) return SCE_SAS_ERROR_BAD_ADDRESS;
    if (voice >= SAS_VOICE_COUNT) return SCE_SAS_ERROR_INVALID_VOICE;
    if (sample_size == 0 || (sample_size > 0 && (sample_size & 0xF) != 0))
        return SCE_SAS_ERROR_INVALID_PARAMETER;
    if (loop > 1) return 0x80420015u;

    MIPS_SasVoice *state = &sas->voices[voice];
    state->configured = true;
    state->sample_addr = sample_addr;
    state->sample_size = sample_size > 0 ? (uint32_t)sample_size : 0;
    state->loop = loop != 0;
    state->playing = state->on;
    state->samples_remaining = state->playing
        ? ((uint64_t)state->sample_size / 16) * SAS_SAMPLES_PER_VAG_FRAME
        : 0;
    return 0;
}

uint32_t mips_sas_key_on(MIPS_SasState *sas, uint32_t voice) {
    if (sas == NULL || !sas->initialized) return SCE_SAS_ERROR_BAD_ADDRESS;
    if (voice >= SAS_VOICE_COUNT) return SCE_SAS_ERROR_INVALID_VOICE;

    MIPS_SasVoice *state = &sas->voices[voice];
    if (state->paused || state->on) return SCE_SAS_ERROR_VOICE_PAUSED;
    if (!state->configured) return 0;
    if (state->sample_size == 0) return 0;
    state->playing = true;
    state->on = true;
    state->samples_remaining = ((uint64_t)state->sample_size / 16) *
                               SAS_SAMPLES_PER_VAG_FRAME;
    return 0;
}

void mips_sas_advance(MIPS_SasState *sas) {
    if (sas == NULL || !sas->initialized) return;

    for (uint32_t i = 0; i < SAS_VOICE_COUNT; ++i) {
        MIPS_SasVoice *voice = &sas->voices[i];
        if (!voice->playing || voice->loop) continue;
        if (voice->samples_remaining <= sas->grain_size) {
            voice->samples_remaining = 0;
            voice->playing = false;
        } else {
            voice->samples_remaining -= sas->grain_size;
        }
    }
}

uint32_t mips_sas_get_end_flag(const MIPS_SasState *sas) {
    if (sas == NULL || !sas->initialized) return SCE_SAS_ERROR_BAD_ADDRESS;

    uint32_t end_flag = 0;
    for (uint32_t i = 0; i < SAS_VOICE_COUNT; ++i) {
        if (!sas->voices[i].playing) end_flag |= 1u << i;
    }
    return end_flag;
}
