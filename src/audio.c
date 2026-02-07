#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <math.h>
#include "types.h"
#include "bitmath.h"
#include "systems/system.h"
#include "chips/2C02.h"
#include "chips/6502.h"
#include "systems/famicom.h"
#include "graphics.h"
#include "audio.h"

void init_audio(SDL_Instance* g)
{
	SDL_AudioSpec spec;
	spec.channels = 1;
	spec.format = SDL_AUDIO_S16;
	spec.freq = 16000;
	SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
	if (stream == NULL) {
		printf("failed to start audio: %s\n", SDL_GetError());
	} else {
		g->stream = stream;
		//SDL_ResumeAudioStreamDevice(g->stream);
	}
}

Sint16 format(double sample, double amplitude) {
	// 32567 is the maximum value of a 16 bit signed integer (2^15-1)
	return (Sint16)(sample*32567*amplitude);
}

double tone(double hz, unsigned long time) {
	return sin(time * hz * M_PI * 2 / 16000);
}

double square(double hz, unsigned long time) {
	double sine = tone(hz, time);
	return sine > 0.0 ? 1.0 : -1.0;
}

int get_freq(int value)
{
	value = value & 0x0FFF;
	value = reverse_byte_order(value);
	return value;
}

// https://nesdev-wiki.nes.science/wikipages/Pulse_Channel_frequency_chart.xhtml
void apu_process(SDL_Instance* g, Famicom* famicom)
{
	Sint16 samples[16000];
	int minimum_audio = (16000 * sizeof (Sint16));
	if (SDL_GetAudioStreamQueued(g->stream) < minimum_audio) {
		int i;
		for (i = 0; i < SDL_arraysize(samples); i++) {
			int freq1 = format(square(get_freq(famicom->apu.pulse1_timer), i), 0.1);
			int freq2 = format(square(get_freq(famicom->apu.pulse2_timer), i), 0.1);
			//int freq3 = format(tone(famicom->apu.tri_timer / 5, i), 0.1);
			samples[i] = freq1 | freq2;// | freq3;
		}
	} else {
		SDL_ClearAudioStream(g->stream);
	}
	SDL_PutAudioStreamData(g->stream, samples, sizeof (samples));
	//SDL_FlushAudioStream(g->stream);
}
