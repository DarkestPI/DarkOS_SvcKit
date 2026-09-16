#include <media_audio_codec.h>
#include <media_audio_filter.h>
#include <media_audio_sink.h>
#include <media_audio_source.h>
#include <media_factory.h>
#include <media_manager.h>
#include <media_muxer.h>
#include <media_pipeline.h>
#include <media_video_codec.h>
#include <media_video_filter.h>
#include <media_video_sink.h>
#include <media_video_source.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

int main() {
  using namespace std::chrono_literals;

  darkos::media::MediaPipelineConfig config;
  config.video.capture.width = 160;
  config.video.capture.height = 120;
  config.video.capture.fps = 15;
  config.video.encoder.bitrateBps = 128'000;
  config.video.encoder.gop = 15;
  config.audio.sampleRate = 16'000;
  config.audio.channelCount = 1;
  config.audio.framesPerBuffer = 320;

  std::mutex mutex;
  std::condition_variable framesAvailable;
  unsigned frames = 0;
  unsigned audioFrames = 0;
  bool invalidPacket = false;
  bool invalidAudioFrame = false;
  std::string error;

  auto pipeline = darkos::media::createMediaPipeline(
      config,
      [&](const darkos::media::EncodedPacketView &packet) {
        std::lock_guard<std::mutex> lock(mutex);
        if (packet.data == nullptr || packet.size == 0)
          invalidPacket = true;
        ++frames;
        framesAvailable.notify_one();
      },
      [&](const darkos::media::AudioFrameView &frame) {
        std::lock_guard<std::mutex> lock(mutex);
        if (frame.data == nullptr || frame.size == 0 ||
            frame.sampleRate != 16'000 || frame.channelCount != 1 ||
            frame.frameCount != 320)
          invalidAudioFrame = true;
        ++audioFrames;
        framesAvailable.notify_one();
      },
      error);
  if (pipeline == nullptr) {
    std::fprintf(stderr, "createMediaPipeline failed: %s\n", error.c_str());
    return 1;
  }
  if (pipeline->start() != 0) {
    std::fprintf(stderr, "MediaPipeline::start failed\n");
    return 1;
  }
  if (pipeline->start() != -EALREADY) {
    std::fprintf(stderr, "duplicate start did not return -EALREADY\n");
    return 1;
  }

  {
    std::unique_lock<std::mutex> lock(mutex);
    if (!framesAvailable.wait_for(
            lock, 3s, [&] { return frames >= 3 && audioFrames >= 3; })) {
      std::fprintf(stderr, "timed out waiting for audio/video frames\n");
      return 1;
    }
  }
  if (pipeline->stop() != 0 || pipeline->running()) {
    std::fprintf(stderr, "MediaPipeline::stop failed\n");
    return 1;
  }
  if (pipeline->stop() != 0) {
    std::fprintf(stderr, "duplicate stop was not idempotent\n");
    return 1;
  }

  unsigned stoppedAt;
  unsigned audioStoppedAt;
  {
    std::lock_guard<std::mutex> lock(mutex);
    stoppedAt = frames;
    audioStoppedAt = audioFrames;
  }
  std::this_thread::sleep_for(150ms);
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (invalidPacket || invalidAudioFrame || frames != stoppedAt ||
        audioFrames != audioStoppedAt) {
      std::fprintf(stderr,
                   "audio/video callback contract violated after stop\n");
      return 1;
    }
  }

  darkos::media::AudioCaptureConfig audioFormat;
  audioFormat.sampleRate = 16'000;
  audioFormat.channelCount = 1;
  audioFormat.framesPerBuffer = 320;
  std::vector<std::int16_t> pcm(audioFormat.framesPerBuffer);
  for (std::size_t index = 0; index < pcm.size(); ++index)
    pcm[index] = static_cast<std::int16_t>((index * 173u) % 20'000u);
  const darkos::media::AudioFrameView pcmFrame{
      reinterpret_cast<const std::uint8_t *>(pcm.data()),
      pcm.size() * sizeof(pcm.front()),
      123'000'000,
      audioFormat.sampleRate,
      audioFormat.channelCount,
      audioFormat.framesPerBuffer,
      audioFormat.sampleFormat};

  for (const darkos::media::AudioCodec codec :
       {darkos::media::AudioCodec::G711A, darkos::media::AudioCodec::G711U}) {
    darkos::media::AudioEncoderConfig encoderConfig;
    encoderConfig.codec = codec;
    auto encoder =
        darkos::media::createAudioEncoder(encoderConfig, audioFormat, error);
    auto decoder = darkos::media::createAudioDecoder(codec, audioFormat, error);
    if (encoder == nullptr || decoder == nullptr || encoder->start() != 0 ||
        decoder->start() != 0) {
      std::fprintf(stderr, "create G.711 codec failed: %s\n", error.c_str());
      return 1;
    }
    darkos::media::EncodedAudioPacketView encodedAudio;
    darkos::media::AudioFrameView decodedAudio;
    if (encoder->encode(pcmFrame, encodedAudio) != 0 ||
        encodedAudio.size != pcm.size() ||
        decoder->decode(encodedAudio, decodedAudio) != 0 ||
        decodedAudio.frameCount != pcm.size() ||
        decodedAudio.size != pcmFrame.size) {
      std::fprintf(stderr, "G.711 codec contract violated\n");
      return 1;
    }
    encoder->stop();
    decoder->stop();
  }

  darkos::media::AudioEncoderConfig unsupportedConfig;
  unsupportedConfig.codec = darkos::media::AudioCodec::Aac;
  if (darkos::media::createAudioEncoder(unsupportedConfig, audioFormat,
                                        error) != nullptr) {
    std::fprintf(stderr, "unsupported AAC encoder was accepted\n");
    return 1;
  }
  return 0;
}
