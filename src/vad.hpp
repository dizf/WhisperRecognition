#pragma once

#if defined(__GNUC__) && defined(_WIN32)
#ifndef _stdcall
#define _stdcall __stdcall
#endif
#ifndef _Frees_ptr_opt_
#define _Frees_ptr_opt_
#endif
#endif

#include <vector>
#include <string>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <onnxruntime_cxx_api.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace vad {

#ifdef _WIN32
inline std::wstring to_wstring(const std::string& str) {
    if (str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}
#endif

class SileroVAD {
private:
    Ort::Env env;
    Ort::Session session;
    Ort::MemoryInfo memory_info;

    std::vector<float> state;
    int64_t sample_rate = 16000;

    const size_t state_size = 2 * 1 * 128; // 256 elements
    std::vector<int64_t> state_shape = { 2, 1, 128 };
    std::vector<int64_t> sr_shape = { 1 };

    const int context_samples = 64; // For 16kHz, 64 samples are added as context
    std::vector<float> context;     // Holds the last 64 samples from the previous chunk

public:
    SileroVAD(const std::string& model_path, int64_t sample_rate = 16000)
        : env(ORT_LOGGING_LEVEL_ERROR, "SileroVAD"),
#ifdef _WIN32
          session(env, to_wstring(model_path).c_str(), Ort::SessionOptions{nullptr}),
#else
          session(env, model_path.c_str(), Ort::SessionOptions{nullptr}),
#endif
          memory_info(Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU)),
          sample_rate(sample_rate) 
    {
        reset_state();
    }

    void reset_state() {
        state.assign(state_size, 0.0f);
        context.assign(context_samples, 0.0f);
    }

    float process_frame(const float* frame_data, size_t frame_size) {
        if (frame_size == 0) return 0.0f;

        // 1. Build new input: first context_samples from context, followed by the current chunk (frame_size).
        size_t eff_size = frame_size + context_samples;
        std::vector<float> input_data(eff_size, 0.0f);
        std::copy(context.begin(), context.end(), input_data.begin());
        std::copy(frame_data, frame_data + frame_size, input_data.begin() + context_samples);

        // 2. Create input tensor
        std::vector<int64_t> input_shape = { 1, static_cast<int64_t>(eff_size) };
        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            memory_info, input_data.data(), input_data.size(), input_shape.data(), input_shape.size()
        );

        // 3. Create sr tensor
        int64_t sr_val = sample_rate;
        Ort::Value sr_tensor = Ort::Value::CreateTensor<int64_t>(
            memory_info, &sr_val, 1, sr_shape.data(), sr_shape.size()
        );

        // 4. Create state tensor
        Ort::Value state_tensor = Ort::Value::CreateTensor<float>(
            memory_info, state.data(), state.size(), state_shape.data(), state_shape.size()
        );

        // 5. Input & output names
        std::vector<const char*> input_names = { "input", "state", "sr" };
        std::vector<const char*> output_names = { "output", "stateN" };

        std::vector<Ort::Value> input_tensors;
        input_tensors.push_back(std::move(input_tensor));
        input_tensors.push_back(std::move(state_tensor));
        input_tensors.push_back(std::move(sr_tensor));

        try {
            // 6. Run session
            auto output_tensors = session.Run(
                Ort::RunOptions{nullptr},
                input_names.data(),
                input_tensors.data(),
                input_tensors.size(),
                output_names.data(),
                output_names.size()
            );

            // 7. Extract speech probability
            float* output_data = output_tensors[0].GetTensorMutableData<float>();
            float speech_prob = output_data[0];

            // 8. Update state
            float* stateN_data = output_tensors[1].GetTensorMutableData<float>();
            std::copy(stateN_data, stateN_data + state_size, state.begin());

            // 9. Update context: copy the last context_samples from input_data
            std::copy(input_data.end() - context_samples, input_data.end(), context.begin());

            return speech_prob;
        } catch (const std::exception& e) {
            std::cerr << "VAD Inference failed: " << e.what() << std::endl;
            return 0.0f;
        }
    }
};

class VADStateTracker {
private:
    SileroVAD vad;
    float threshold;
    size_t min_speech_frames;
    size_t min_silence_frames;

    size_t speech_counter = 0;
    size_t silence_counter = 0;
    bool is_speech_active = false;
    float last_prob = 0.0f;

public:
    VADStateTracker(const std::string& model_path, float threshold = 0.25f, // Manual low threshold for whispering (default is 0.5)
                    size_t min_speech_frames = 2, // ~64ms
                    size_t min_silence_frames = 10) // ~320ms to allow whisper pause
        : vad(model_path), threshold(threshold),
          min_speech_frames(min_speech_frames), min_silence_frames(min_silence_frames) {}

    void reset() {
        vad.reset_state();
        speech_counter = 0;
        silence_counter = 0;
        is_speech_active = false;
        last_prob = 0.0f;
    }

    float get_last_prob() const { return last_prob; }

    // Process a 512-sample (32ms at 16kHz) audio frame
    // Returns:
    //   1: Speech has just STARTED (transition from silence to speech)
    //   2: Speech is CONTINUING (currently in speech state)
    //   3: Speech has just ENDED (transition from speech to silence)
    //   0: Silence (currently in silent state)
    int feed_frame(const float* frame_data, size_t frame_size) {
        float speech_prob = vad.process_frame(frame_data, frame_size);
        last_prob = speech_prob;

        if (speech_prob >= threshold) {
            speech_counter++;
            silence_counter = 0;

            if (!is_speech_active && speech_counter >= min_speech_frames) {
                is_speech_active = true;
                return 1; // Transition to speech
            }
            if (is_speech_active) {
                return 2; // Continuing speech
            }
        } else {
            silence_counter++;
            speech_counter = 0;

            if (is_speech_active && silence_counter >= min_silence_frames) {
                is_speech_active = false;
                return 3; // Transition to silence (ended)
            }
            if (is_speech_active) {
                return 2; // Still considered speech because silence buffer is not reached
            }
        }

        return 0; // Silence
    }

    bool is_speaking() const { return is_speech_active; }
};

} // namespace vad
