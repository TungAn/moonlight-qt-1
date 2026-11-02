#pragma once

#include "SDL_compat.h"

#include <QString>

struct OpusEncoder;

// Sends the local microphone to the host's microphone stream (Apollo's
// microphone passthrough, see moonlight-common-c docs/microphone.md), as
// Nightjar does: mono 48 kHz Opus in 10 ms VoIP packets with in-band FEC, one
// datagram per packet, sent from the capture callback as it is recorded.
//
// The capture device is open only while the microphone is unmuted, so the
// system's recording indicator shows exactly when audio leaves the client.
class MicrophoneStream
{
public:
    enum class EchoCancellation {
        Automatic,
        On,
        Off,
    };

    MicrophoneStream();
    ~MicrophoneStream();

    // Opens the capture device and starts sending. With echo cancellation
    // wanted, records from the system's echo-cancelled source when there is
    // one (PipeWire's or PulseAudio's echo-cancel module on Linux). Returns
    // false with a reason in error() when the microphone cannot start.
    bool start(EchoCancellation echoCancellation);

    // Closes the capture device; nothing more is sent.
    void stop();

    bool isRunning() const { return m_Device != 0; }

    // Whether an SDL_AUDIODEVICEREMOVED event is about this stream's device.
    bool isDevice(SDL_AudioDeviceID device) const { return m_Device != 0 && device == m_Device; }

    QString error() const { return m_Error; }

    static constexpr int k_SampleRate = 48000;
    static constexpr int k_FrameSamples = k_SampleRate / 100;

private:
    static void captureCallback(void* userdata, Uint8* stream, int len);
    void capture(const float* samples, int count);
    void sendFrame();

    SDL_AudioDeviceID m_Device;
    OpusEncoder* m_Encoder;
    float m_Frame[k_FrameSamples];
    int m_FrameFill;
    bool m_AudioSubsystemInitialized;
    int m_SendFailures;
    QString m_Error;
};
