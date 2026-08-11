/// @file X3DAudioEngine.h
/// @brief X3DAudio spatialization: 3D audio panning, distance attenuation, and Doppler.
///
/// Reimplements X3DAudioCalculate by computing per-channel panning matrices, inverse
/// distance rolloff, and Doppler pitch shifts from listener/emitter positions and
/// velocities. Outputs matrix coefficients for up to 18 source channels × 2 output
/// channels, plus reverb/LFE levels and low-pass filter coefficients. Used by games
/// that position audio sources in 3D space via X3DAudio. The fixed output
/// matrix supports up to two destination channels; larger requests are
/// represented using that same two-channel layout.

#pragma once

#include <cstdint>
#include <metalsharp/Platform.h>

namespace metalsharp {

struct Audio3DVector {
    float x, y, z;
};

struct Audio3DListener {
    Audio3DVector position;
    Audio3DVector front;
    Audio3DVector top;
    Audio3DVector velocity;
    float distanceScalar;
    float dopplerScalar;
    float panningAngle;
};

struct Audio3DEmitter {
    Audio3DVector position;
    Audio3DVector front;
    Audio3DVector top;
    Audio3DVector velocity;
    float innerRadius;
    float innerRadiusAngle;
    uint32_t channelCount;
    float channelRadius;
    float curveDistanceScaler;
    float dopplerScaler;
};

struct Audio3DOutput {
    float matrixCoefficients[18 * 2];
    float emitterToListenerDistance;
    float emitterVelocityComponent;
    float listenerVelocityComponent;
    float dopplerFactor;
    float reverbLFELevel;
    float reverbLevel;
    float lfeLevel;
    float lpFDirectCoefficient;
    float lpFReverbCoefficient;
};

class X3DAudioEngine {
  public:
    static X3DAudioEngine& instance();

    void init(uint32_t speakerChannelMask);
    void shutdown();

    void calculate(const Audio3DListener& listener, const Audio3DEmitter& emitter, uint32_t flags,
                   uint32_t dstChannelCount, Audio3DOutput& output);

    void setDistanceCurve(float nearDist, float farDist, float rolloff);
    void setDopplerFactor(float factor);

    float computePan(const Audio3DListener& listener, const Audio3DEmitter& emitter);
    float computeDistanceAttenuation(float distance);
    float computeDoppler(const Audio3DListener& listener, const Audio3DEmitter& emitter);

  private:
    X3DAudioEngine() = default;

    bool m_initialized = false;
    uint32_t m_speakerChannelMask = 0x3;
    float m_nearDistance = 0.1f;
    float m_farDistance = 100.0f;
    float m_rolloffFactor = 1.0f;
    float m_dopplerFactor = 1.0f;
    float m_speedOfSound = 343.0f;
};

} // namespace metalsharp
