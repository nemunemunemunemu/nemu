#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include "types.h"
#include "bitmath.h"
#include "systems/system.h"
#include "chips/2C02.h"
#include "chips/6502.h"
#include "systems/famicom.h"
#include "graphics.h"
#include "audio.h"
#include "math.h"

void init_audio(SDL_Instance* g)
{
	SDL_AudioSpec spec;
	spec.channels = 1;
	spec.format = SDL_AUDIO_F32;
	spec.freq = 8000;
	SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
	if (stream == NULL) {
		printf("failed to start audio: %s\n", SDL_GetError());
	} else {
		g->stream = stream;
		SDL_ResumeAudioStreamDevice(g->stream);
	}
}
// https://nesdev-wiki.nes.science/wikipages/Pulse_Channel_frequency_chart.xhtml

void apu_process(SDL_Instance* g, Famicom* famicom)
{
	float samples[512];
	int current_sine_sample = 0;
	const int minimum_audio = (8000 * sizeof (float)) / 2;
	if (SDL_GetAudioStreamQueued(g->stream) < minimum_audio) {
		int i;
		for (i = 0; i < SDL_arraysize(samples); i++) {
			const int freq = 440;
			const float phase = current_sine_sample * freq / 8000.0f;
			samples[i] = SDL_sinf(phase * 2 * SDL_PI_F);
			current_sine_sample++;
		}
	}
        current_sine_sample %= 8000;
        SDL_PutAudioStreamData(g->stream, samples, sizeof (samples));
}
