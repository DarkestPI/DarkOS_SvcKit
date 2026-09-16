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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

int main() {
  using namespace std::chrono_literals;

  darkos::media::MediaPipelineConfig config;
  config.video.capture.width = 160;
  config.video.capture.height = 120;
  config.video.capture.fps = 15;
  config.video.encoder.bitrateBps = 128'000;
  config.video.encoder.gop = 15;
  config.audio.capture.sampleRate = 16'000;
  config.audio.capture.channelCount = 1;
  config.audio.capture.framesPerBuffer = 320;
  config.audio.encoder.codec = darkos::media::AudioCodec::G711A;

  std::string error;

  auto pipeline = darkos::media::createMediaPipeline(config, error);
  if (pipeline == nullptr) {
    std::fprintf(stderr, "createMediaPipeline failed: %s\n", error.c_str());
    return 1;
  }

  auto firstVideoSink = darkos::media::createVideoProbeSink();
  auto secondVideoSink = darkos::media::createVideoProbeSink();
  auto audioSink = darkos::media::createAudioProbeSink();
  darkos::media::SinkId firstVideoSinkId = 0;
  darkos::media::SinkId secondVideoSinkId = 0;
  darkos::media::SinkId audioSinkId = 0;
  const darkos::media::MediaQueueConfig sinkQueue{
      4, darkos::media::BackpressurePolicy::DropOldest};
  if (pipeline->addVideoSink(firstVideoSink, sinkQueue, firstVideoSinkId) !=
          0 ||
      pipeline->addVideoSink(secondVideoSink, sinkQueue, secondVideoSinkId) !=
          0 ||
      pipeline->addAudioSink(audioSink, sinkQueue, audioSinkId) != 0) {
    std::fprintf(stderr, "adding sinks failed\n");
    return 1;
  }

  if (pipeline->start() != 0 ||
      pipeline->state() != darkos::media::PipelineState::Running) {
    std::fprintf(stderr, "MediaPipeline::start failed\n");
    return 1;
  }
  if (pipeline->start() != -EALREADY) {
    std::fprintf(stderr, "duplicate start did not return -EALREADY\n");
    return 1;
  }

  if (firstVideoSink->waitForPackets(3, 3'000) != 0 ||
      secondVideoSink->waitForPackets(3, 3'000) != 0 ||
      audioSink->waitForPackets(3, 3'000) != 0) {
    std::fprintf(stderr, "timed out waiting for A/V fanout packets\n");
    return 1;
  }
  const darkos::media::PipelineStats stats = pipeline->stats();
  if (stats.encodedVideoPackets < 3 || stats.encodedAudioPackets < 3) {
    std::fprintf(stderr, "pipeline stats did not count encoded packets\n");
    return 1;
  }
  if (pipeline->stop() != 0 || pipeline->running() ||
      pipeline->state() != darkos::media::PipelineState::Stopped) {
    std::fprintf(stderr, "MediaPipeline::stop failed\n");
    return 1;
  }
  if (pipeline->stop() != 0) {
    std::fprintf(stderr, "duplicate stop was not idempotent\n");
    return 1;
  }

  const auto stoppedVideo = firstVideoSink->snapshot();
  const auto stoppedSecondVideo = secondVideoSink->snapshot();
  const auto stoppedAudio = audioSink->snapshot();
  if (stoppedVideo.packetCount < 3 || stoppedVideo.byteCount == 0 ||
      stoppedSecondVideo.packetCount < 3 || stoppedSecondVideo.byteCount == 0 ||
      stoppedAudio.packetCount < 3 || stoppedAudio.byteCount == 0 ||
      stoppedAudio.lastCodec != darkos::media::AudioCodec::G711A ||
      stoppedAudio.lastSampleRate != 16'000 ||
      stoppedAudio.lastChannelCount != 1) {
    std::fprintf(stderr, "probe sink packet contract violated\n");
    return 1;
  }
  std::this_thread::sleep_for(150ms);
  if (firstVideoSink->snapshot().packetCount != stoppedVideo.packetCount ||
      secondVideoSink->snapshot().packetCount !=
          stoppedSecondVideo.packetCount ||
      audioSink->snapshot().packetCount != stoppedAudio.packetCount) {
    std::fprintf(stderr, "owned packet or stop contract violated\n");
    return 1;
  }

  darkos::media::MediaEvent event;
  bool sawStopped = false;
  while (pipeline->waitEvent(event, 0) == 0)
    sawStopped =
        sawStopped || event.state == darkos::media::PipelineState::Stopped;
  if (!sawStopped) {
    std::fprintf(stderr, "pipeline did not publish stopped state event\n");
    return 1;
  }

  darkos::media::AudioCaptureConfig audioFormat;
  audioFormat.sampleRate = 16'000;
  audioFormat.channelCount = 1;
  audioFormat.framesPerBuffer = 320;
  std::vector<std::int16_t> pcm(audioFormat.framesPerBuffer);
  for (std::size_t index = 0; index < pcm.size(); ++index)
    pcm[index] = static_cast<std::int16_t>((index * 173u) % 20'000u);
  auto pcmBuffer = darkos::media::copyMediaBuffer(
      pcm.data(), pcm.size() * sizeof(pcm.front()));
  const darkos::media::AudioFrame pcmFrame{
      std::move(pcmBuffer),        123'000'000,
      audioFormat.sampleRate,      audioFormat.channelCount,
      audioFormat.framesPerBuffer, audioFormat.sampleFormat};

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
    darkos::media::AudioPacketPtr encodedAudio;
    darkos::media::AudioFramePtr decodedAudio;
    if (encoder->encode(pcmFrame, encodedAudio) != 0 ||
        encodedAudio == nullptr || encodedAudio->buffer->size() != pcm.size() ||
        decoder->decode(*encodedAudio, decodedAudio) != 0 ||
        decodedAudio == nullptr || decodedAudio->frameCount != pcm.size() ||
        decodedAudio->buffer->size() != pcmFrame.buffer->size()) {
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
