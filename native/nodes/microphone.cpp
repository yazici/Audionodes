#include "microphone.hpp"
#include <iostream>

namespace audionodes {

void Microphone::callback(void *userdata, Uint8 *_stream, int len) {
  Microphone *node = (Microphone*) userdata;
  if (!node->mark_connected) return;
  float *stream = (float*)_stream;
  len /= sizeof(float);
  for (int i = 0; i < len; i++) {
    node->q.push(stream[i]);
  }
}

Microphone::Microphone() :
    Node({}, {SocketType::audio}, {})
{
  SDL_AudioSpec want, have;

  want.freq = RATE;
  want.format = AUDIO_F32SYS;
  want.channels = 1;
  want.samples = N;
  want.callback = callback;
  want.userdata = this;
  
  dev = SDL_OpenAudioDevice(NULL, true, &want, &have, 0);

  if (dev == 0) {
    std::cerr << "Failed to open microphone: " << SDL_GetError() << std::endl;
  }
  SDL_PauseAudioDevice(dev, 0);
}

Microphone::~Microphone() {
  SDL_CloseAudioDevice(dev);
}

void Microphone::connect_callback() {
  q.clear();
}

void Microphone::process(NodeInputWindow &input) {
  AudioData::PolyWriter output(output_window[0], 1);
  for (size_t j = 0; j < N; ++j) {
    if (!q.empty()) {
      output[0][j] = q.pop();
    } else {
      output[0][j] = 0;
    }
  }
}

}
