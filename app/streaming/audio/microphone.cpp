#include "microphone.h"

#include <Limelight.h>
#include <opus.h>

#include <cstring>

// Speech needs little; in-band FEC recovers a single lost packet at the host,
// which conceals longer gaps.
static const int k_Bitrate = 32000;
static const int k_ExpectedLossPercent = 10;

MicrophoneStream::MicrophoneStream()
    : m_Device(0),
      m_Encoder(nullptr),
      m_FrameFill(0),
      m_AudioSubsystemInitialized(false),
      m_SendFailures(0)
{
}

MicrophoneStream::~MicrophoneStream()
{
    stop();
}

// PipeWire's echo-cancel module names its source "Echo-Cancel Source";
// PulseAudio's module-echo-cancel names it "... (echo cancelled with ...)".
static bool isEchoCancelSource(const char* name)
{
    const QString lower = QString::fromUtf8(name).toLower();
    return lower.contains("echo-cancel") || lower.contains("echo cancel");
}

bool MicrophoneStream::start(EchoCancellation echoCancellation)
{
    if (isRunning()) {
        return true;
    }
    m_Error.clear();

    // The microphone holds its own reference on the audio subsystem, so a
    // playback renderer that is recreated mid-stream cannot close it.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        m_Error = QString::fromUtf8(SDL_GetError());
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Microphone: SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s",
                     SDL_GetError());
        return false;
    }
    m_AudioSubsystemInitialized = true;

    int error = OPUS_OK;
    m_Encoder = opus_encoder_create(k_SampleRate, 1, OPUS_APPLICATION_VOIP, &error);
    if (m_Encoder == nullptr) {
        m_Error = QString("Opus encoder error %1").arg(error);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Microphone: opus_encoder_create() failed: %d",
                     error);
        stop();
        return false;
    }
    opus_encoder_ctl(m_Encoder, OPUS_SET_BITRATE(k_Bitrate));
    opus_encoder_ctl(m_Encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(m_Encoder, OPUS_SET_PACKET_LOSS_PERC(k_ExpectedLossPercent));
    opus_encoder_ctl(m_Encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    const char* deviceName = nullptr;
    if (echoCancellation != EchoCancellation::Off) {
        for (int i = 0; i < SDL_GetNumAudioDevices(1); i++) {
            const char* name = SDL_GetAudioDeviceName(i, 1);
            if (name != nullptr && isEchoCancelSource(name)) {
                deviceName = name;
                break;
            }
        }
        if (deviceName != nullptr) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Microphone: recording from \"%s\", the echo-cancel source",
                        deviceName);
        }
        else {
            // Windows applies a communications device's own processing; on
            // Linux, PipeWire's or PulseAudio's echo-cancel module adds a source.
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Microphone: no echo-cancel source; sending the default microphone without echo cancellation");
        }
    }

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = k_SampleRate;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    want.samples = k_FrameSamples;
    want.callback = captureCallback;
    want.userdata = this;

    m_FrameFill = 0;
    m_SendFailures = 0;
    // No allowed changes: SDL converts the device's format to this one.
    m_Device = SDL_OpenAudioDevice(deviceName, 1, &want, &have, 0);
    if (m_Device == 0) {
        m_Error = QString::fromUtf8(SDL_GetError());
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Microphone: cannot open the capture device: %s",
                     SDL_GetError());
        stop();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Microphone: sending to the host (%d Hz mono, %d kb/s Opus)",
                k_SampleRate, k_Bitrate / 1000);
    SDL_PauseAudioDevice(m_Device, 0);
    return true;
}

void MicrophoneStream::stop()
{
    if (m_Device != 0) {
        // Waits for a running callback, so the encoder is free afterwards.
        SDL_CloseAudioDevice(m_Device);
        m_Device = 0;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Microphone: stopped");
    }
    if (m_Encoder != nullptr) {
        opus_encoder_destroy(m_Encoder);
        m_Encoder = nullptr;
    }
    if (m_AudioSubsystemInitialized) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        m_AudioSubsystemInitialized = false;
    }
}

void MicrophoneStream::captureCallback(void* userdata, Uint8* stream, int len)
{
    auto me = static_cast<MicrophoneStream*>(userdata);
    me->capture(reinterpret_cast<const float*>(stream), len / (int)sizeof(float));
}

void MicrophoneStream::capture(const float* samples, int count)
{
    // Whole 10 ms frames, whatever period the device delivers.
    while (count > 0) {
        const int take = SDL_min(count, k_FrameSamples - m_FrameFill);
        memcpy(&m_Frame[m_FrameFill], samples, take * sizeof(float));
        m_FrameFill += take;
        samples += take;
        count -= take;
        if (m_FrameFill == k_FrameSamples) {
            sendFrame();
            m_FrameFill = 0;
        }
    }
}

void MicrophoneStream::sendFrame()
{
    unsigned char packet[1275];
    const opus_int32 length = opus_encode_float(m_Encoder, m_Frame, k_FrameSamples, packet, sizeof(packet));
    if (length <= 0) {
        return;
    }
    if (LiSendMicrophoneOpusData(packet, length) < 0) {
        // The connection is stopping, or the network is down; the stream
        // ends either way, so a few log lines are enough.
        if (m_SendFailures++ < 3) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Microphone: sending a packet failed");
        }
    }
}
