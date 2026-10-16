#include "audio_manager.h"
#include "app.h"
#include "file_utils.h"
#include <SDL3/SDL.h>

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#undef STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#include <vector>

static ma_engine s_engine;
static bool s_audioInitialized = false;

// Background music decoder & sound in memory
static ma_decoder s_musicDecoder;
static ma_sound s_music;
static void *s_musicFileData = nullptr;
static bool s_musicLoaded = false;

// Footstep sound decoder & memory
static const int MAX_FOOTSTEP_SOUNDS = 12;
static ma_audio_buffer s_footstepBuffers[MAX_FOOTSTEP_SOUNDS];
static ma_sound s_footstepPool[MAX_FOOTSTEP_SOUNDS];
static bool s_footstepsLoaded[MAX_FOOTSTEP_SOUNDS] = { false };
static void *s_footstepPcmData = nullptr;
static int s_nextFootstepIdx = 0;

bool initAudio(App *app)
{
    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.listenerCount = 1;

    ma_result res = ma_engine_init(&engineConfig, &s_engine);
    if (res != MA_SUCCESS)
    {
        SDL_Log("miniaudio: Failed to initialize audio engine (error: %d)", res);
        return false;
    }
    s_audioInitialized = true;

    ma_device *pDevice = ma_engine_get_device(&s_engine);
    if (pDevice && pDevice->pContext)
    {
        SDL_Log("miniaudio: Active backend is %s", ma_get_backend_name(pDevice->pContext->backend));
    }

    // -------------------------------------------------------------------------
    // Background Music (Loaded via SDL3 from APK / Disk into Memory)
    // -------------------------------------------------------------------------
    const char *musicPath = getAssetPath("assets/audio/Snowfall (Looped ver.).ogg");
    size_t musicFileSize = 0;
    s_musicFileData = SDL_LoadFile(musicPath, &musicFileSize);

    if (s_musicFileData && musicFileSize > 0)
    {
        ma_decoder_config decoderConfig = ma_decoder_config_init_default();
        res = ma_decoder_init_memory(s_musicFileData, musicFileSize, &decoderConfig, &s_musicDecoder);
        if (res == MA_SUCCESS)
        {
            res = ma_sound_init_from_data_source(&s_engine, &s_musicDecoder, 0, nullptr, &s_music);
            if (res == MA_SUCCESS)
            {
                // Explicitly disable 3D spatialization for background music so it stays 2D stereo
                ma_sound_set_spatialization_enabled(&s_music, MA_FALSE);
                ma_sound_set_looping(&s_music, MA_TRUE);
                ma_sound_set_volume(&s_music, app->musicVolume);
                ma_sound_start(&s_music);
                s_musicLoaded = true;
                SDL_Log("miniaudio: Background music decoded from memory and started successfully.");
            }
            else
            {
                SDL_Log("miniaudio: Failed to init sound from music decoder (error: %d)", res);
                ma_decoder_uninit(&s_musicDecoder);
            }
        }
        else
        {
            SDL_Log("miniaudio: Failed to init music memory decoder (error: %d)", res);
        }
    }
    else
    {
        SDL_Log("miniaudio: Could not read music file from assets: %s", musicPath);
    }

    // -------------------------------------------------------------------------
    // 3D Footstep Sound (Loaded via SDL3 from APK / Disk into Memory)
    // -------------------------------------------------------------------------
    const char *footstepPath = getAssetPath("assets/audio/DIRT - Hard Walk 1.wav");
    size_t footstepFileSize = 0;
    void *footstepRawFile = SDL_LoadFile(footstepPath, &footstepFileSize);

    if (footstepRawFile && footstepFileSize > 0)
    {
        // Decode the entire WAV file once into raw PCM memory
        ma_decoder footstepDecoder;
        res = ma_decoder_init_memory(footstepRawFile, footstepFileSize, nullptr, &footstepDecoder);
        if (res == MA_SUCCESS)
        {
            ma_uint64 totalFrames = 0;
            ma_decoder_get_length_in_pcm_frames(&footstepDecoder, &totalFrames);

            ma_format format = footstepDecoder.outputFormat;
            ma_uint32 channels = footstepDecoder.outputChannels;
            ma_uint32 sampleRate = footstepDecoder.outputSampleRate;

            size_t bytesPerFrame = ma_get_bytes_per_frame(format, channels);
            s_footstepPcmData = SDL_malloc(totalFrames * bytesPerFrame);

            ma_uint64 framesRead = 0;
            ma_decoder_read_pcm_frames(&footstepDecoder, s_footstepPcmData, totalFrames, &framesRead);
            ma_decoder_uninit(&footstepDecoder);

            // Create memory audio buffers for each voice in the pool
            for (int i = 0; i < MAX_FOOTSTEP_SOUNDS; ++i)
            {
                ma_audio_buffer_config bufConfig = ma_audio_buffer_config_init(
                    format,
                    channels,
                    framesRead,
                    s_footstepPcmData,
                    nullptr);
                bufConfig.sampleRate = sampleRate;

                res = ma_audio_buffer_init(&bufConfig, &s_footstepBuffers[i]);
                if (res == MA_SUCCESS)
                {
                    res = ma_sound_init_from_data_source(&s_engine, &s_footstepBuffers[i], 0, nullptr, &s_footstepPool[i]);
                    if (res == MA_SUCCESS)
                    {
                        s_footstepsLoaded[i] = true;
                        ma_sound_set_spatialization_enabled(&s_footstepPool[i], MA_TRUE);
                        ma_sound_set_attenuation_model(&s_footstepPool[i], ma_attenuation_model_inverse);
                        ma_sound_set_min_distance(&s_footstepPool[i], 1.0f);
                        ma_sound_set_max_distance(&s_footstepPool[i], 40.0f);
                        ma_sound_set_rolloff(&s_footstepPool[i], 1.0f);
                    }
                }
            }
            SDL_Log("miniaudio: 3D Footstep memory pool initialized with %d voices.", MAX_FOOTSTEP_SOUNDS);
        }
        else
        {
            SDL_Log("miniaudio: Failed to decode footstep sound from memory (error: %d)", res);
        }

        SDL_free(footstepRawFile);
    }
    else
    {
        SDL_Log("miniaudio: Could not read footstep file from assets: %s", footstepPath);
    }

    return true;
}

void resumeAudioContext()
{
    if (s_audioInitialized)
    {
        ma_engine_start(&s_engine);
    }
}

void updateAudioListener(App *app, const vec3 listenerPos, const vec3 listenerFront, const vec3 listenerUp)
{
    if (!s_audioInitialized)
        return;

    ma_engine_listener_set_position(&s_engine, 0, listenerPos[0], listenerPos[1], listenerPos[2]);
    ma_engine_listener_set_direction(&s_engine, 0, listenerFront[0], listenerFront[1], listenerFront[2]);
    ma_engine_listener_set_world_up(&s_engine, 0, listenerUp[0], listenerUp[1], listenerUp[2]);
}

void play3DFootstep(App *app, float x, float y, float z, float volume)
{
    if (!s_audioInitialized)
        return;

    int idx = s_nextFootstepIdx;
    s_nextFootstepIdx = (s_nextFootstepIdx + 1) % MAX_FOOTSTEP_SOUNDS;

    if (!s_footstepsLoaded[idx])
        return;

    ma_sound_set_position(&s_footstepPool[idx], x, y, z);
    ma_sound_set_volume(&s_footstepPool[idx], volume);
    ma_sound_seek_to_pcm_frame(&s_footstepPool[idx], 0);
    ma_sound_start(&s_footstepPool[idx]);
}

void setMusicVolume(App *app, float volume)
{
    if (s_audioInitialized && s_musicLoaded)
    {
        ma_sound_set_volume(&s_music, volume);
    }
}

void cleanupAudio(App *app)
{
    if (!s_audioInitialized)
        return;

    if (s_musicLoaded)
    {
        ma_sound_stop(&s_music);
        ma_sound_uninit(&s_music);
        ma_decoder_uninit(&s_musicDecoder);
        if (s_musicFileData)
        {
            SDL_free(s_musicFileData);
            s_musicFileData = nullptr;
        }
        s_musicLoaded = false;
    }

    for (int i = 0; i < MAX_FOOTSTEP_SOUNDS; ++i)
    {
        if (s_footstepsLoaded[i])
        {
            ma_sound_stop(&s_footstepPool[i]);
            ma_sound_uninit(&s_footstepPool[i]);
            ma_audio_buffer_uninit(&s_footstepBuffers[i]);
            s_footstepsLoaded[i] = false;
        }
    }

    if (s_footstepPcmData)
    {
        SDL_free(s_footstepPcmData);
        s_footstepPcmData = nullptr;
    }

    ma_engine_stop(&s_engine);
    ma_engine_uninit(&s_engine);
    s_audioInitialized = false;
}
