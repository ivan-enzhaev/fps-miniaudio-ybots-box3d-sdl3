#pragma once

#include <cglm/cglm.h>

struct App;

bool initAudio(App *app);
void resumeAudioContext();
void updateAudioListener(App *app, const vec3 listenerPos, const vec3 listenerFront, const vec3 listenerUp);
void play3DFootstep(App *app, float x, float y, float z, float volume = 0.8f);
void setMusicVolume(App *app, float volume);
void cleanupAudio(App *app);
