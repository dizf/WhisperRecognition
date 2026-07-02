#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

namespace dsp {

constexpr float PI = 3.14159265358979323846f;

class HighPassFilter {
private:
    float x1 = 0.0f;
    float x2 = 0.0f;
    float y1 = 0.0f;
    float y2 = 0.0f;
    float b0 = 0.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;

public:
    HighPassFilter(float cut_off, float sample_rate) {
        float ff = cut_off / sample_rate;
        float ita = 1.0f / std::tan(ff * PI);
        float q = 0.70710678118f; // Butterworth Q factor (1/sqrt(2))

        float d = ita * ita + ita / q + 1.0f;
        b0 = ita * ita / d;
        b1 = -2.0f * b0;
        b2 = b0;
        a1 = 2.0f * (ita * ita - 1.0f) / d;
        a2 = -(ita * ita - ita / q + 1.0f) / d;
    }

    float process(float sample) {
        float out = b0 * sample + b1 * x1 + b2 * x2 + a1 * y1 + a2 * y2;

        x2 = x1;
        x1 = sample;
        y2 = y1;
        y1 = out;

        return out;
    }

    void process_slice(std::vector<float>& samples) {
        for (float& sample : samples) {
            sample = process(sample);
        }
    }

    void process_array(float* samples, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            samples[i] = process(samples[i]);
        }
    }
};

class SimpleAgc {
private:
    float target_rms = 0.0f;
    float max_gain = 30.0f; // Default: limit to 30.0f (~30dB) to prevent noise explosion
    float min_gain = 1.0f;   // Do not attenuate below original volume
    float current_gain = 1.0f;
    float attack_coeff = 0.0f;
    float release_coeff = 0.0f;
    float silence_threshold = 0.0003f; // Default: noise gate below -70dB RMS

public:
    SimpleAgc(float target_rms, float sample_rate, float max_gain = 30.0f, float silence_threshold = 0.0003f) 
        : target_rms(target_rms), max_gain(max_gain), silence_threshold(silence_threshold) {
        // 10ms attack time
        attack_coeff = std::exp(-1.0f / (0.01f * sample_rate));
        // 200ms release time
        release_coeff = std::exp(-1.0f / (0.20f * sample_rate));
    }

    void process_block(std::vector<float>& samples) {
        if (samples.empty()) {
            return;
        }

        // Calculate RMS of the current block
        float sum_sq = 0.0f;
        for (float s : samples) {
            sum_sq += s * s;
        }
        float rms = std::sqrt(sum_sq / samples.size());

        // Apply silent gate threshold (noise gate) to prevent whispering from being blocked or noise amplified
        if (rms < silence_threshold) {
            // Slow decay towards 1.0 gain
            current_gain = current_gain * release_coeff + 1.0f * (1.0f - release_coeff);
            for (float& s : samples) {
                s *= current_gain;
            }
            return;
        }

        // Calculate target gain for this block
        float target_gain = target_rms / rms;
        if (target_gain < min_gain) target_gain = min_gain;
        if (target_gain > max_gain) target_gain = max_gain;

        // Smoothly adjust gain sample-by-sample to avoid clicking artifacts
        for (float& s : samples) {
            float coeff = (target_gain < current_gain) ? attack_coeff : release_coeff;
            current_gain = current_gain * coeff + target_gain * (1.0f - coeff);
            s *= current_gain;
        }
    }

    void process_array(float* samples, size_t count) {
        if (count == 0) return;

        float sum_sq = 0.0f;
        for (size_t i = 0; i < count; ++i) {
            sum_sq += samples[i] * samples[i];
        }
        float rms = std::sqrt(sum_sq / count);

        // Apply silent gate threshold (noise gate)
        if (rms < silence_threshold) {
            current_gain = current_gain * release_coeff + 1.0f * (1.0f - release_coeff);
            for (size_t i = 0; i < count; ++i) {
                samples[i] *= current_gain;
            }
            return;
        }

        float target_gain = target_rms / rms;
        if (target_gain < min_gain) target_gain = min_gain;
        if (target_gain > max_gain) target_gain = max_gain;

        for (size_t i = 0; i < count; ++i) {
            float coeff = (target_gain < current_gain) ? attack_coeff : release_coeff;
            current_gain = current_gain * coeff + target_gain * (1.0f - coeff);
            samples[i] *= current_gain;
        }
    }
};

} // namespace dsp
