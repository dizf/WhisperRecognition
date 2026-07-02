#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>

namespace audio {

class LinearResampler {
private:
    double source_rate;
    double target_rate;
    double phase = 0.0;
    float last_sample = 0.0f;

public:
    LinearResampler(double source_rate, double target_rate)
        : source_rate(source_rate), target_rate(target_rate) {}

    void resample(const float* input, size_t input_size, std::vector<float>& output) {
        if (input_size == 0) return;

        double step = source_rate / target_rate;

        while (phase < input_size) {
            size_t idx1 = static_cast<size_t>(phase);
            size_t idx2 = idx1 + 1;
            float s1 = input[idx1];
            float s2 = (idx2 < input_size) ? input[idx2] : input[input_size - 1];

            float t = static_cast<float>(phase - idx1);
            float interpolated = s1 + t * (s2 - s1);
            output.push_back(interpolated);

            phase += step;
        }

        phase -= input_size;
        last_sample = input[input_size - 1];
    }

    void reset() {
        phase = 0.0;
        last_sample = 0.0f;
    }
};

template <typename T>
class SafeQueue {
private:
    std::queue<T> queue;
    std::mutex mutex;
    std::condition_variable cv;

public:
    void push(T value) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push(value);
        cv.notify_one();
    }

    bool pop(T& value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (queue.empty()) {
            return false;
        }
        value = std::move(queue.front());
        queue.pop();
        return true;
    }

    bool wait_for_pop(T& value, int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !queue.empty(); })) {
            return false;
        }
        value = std::move(queue.front());
        queue.pop();
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        std::queue<T> empty_queue;
        std::swap(queue, empty_queue);
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.size();
    }
};

class WasapiRecorder {
private:
    std::atomic<bool> is_running{false};
    std::thread capture_thread;
    SafeQueue<std::vector<float>> output_queue;

    void run_capture() {
        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(hr)) {
            std::cerr << "CoInitializeEx failed: " << hr << std::endl;
            return;
        }

        IMMDeviceEnumerator* pEnumerator = nullptr;
        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), NULL,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
            (void**)&pEnumerator
        );
        if (FAILED(hr)) {
            std::cerr << "CoCreateInstance IMMDeviceEnumerator failed: " << hr << std::endl;
            CoUninitialize();
            return;
        }

        IMMDevice* pDevice = nullptr;
        hr = pEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &pDevice);
        pEnumerator->Release();
        if (FAILED(hr)) {
            std::cerr << "GetDefaultAudioEndpoint failed: " << hr << std::endl;
            CoUninitialize();
            return;
        }

        IAudioClient* pAudioClient = nullptr;
        hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pAudioClient);
        pDevice->Release();
        if (FAILED(hr)) {
            std::cerr << "Activate IAudioClient failed: " << hr << std::endl;
            CoUninitialize();
            return;
        }

        WAVEFORMATEX* pwfx = nullptr;
        hr = pAudioClient->GetMixFormat(&pwfx);
        if (FAILED(hr)) {
            std::cerr << "GetMixFormat failed: " << hr << std::endl;
            pAudioClient->Release();
            CoUninitialize();
            return;
        }

        std::cout << "WASAPI Recording Device Mix Format:" << std::endl;
        std::cout << "  Sample Rate: " << pwfx->nSamplesPerSec << " Hz" << std::endl;
        std::cout << "  Channels: " << pwfx->nChannels << std::endl;
        std::cout << "  Bits Per Sample: " << pwfx->wBitsPerSample << std::endl;

        hr = pAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            0,
            10000000, // 1 second buffer
            0,
            pwfx,
            NULL
        );
        if (FAILED(hr)) {
            std::cerr << "Initialize IAudioClient failed: " << hr << std::endl;
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            CoUninitialize();
            return;
        }

        IAudioCaptureClient* pCaptureClient = nullptr;
        hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
        if (FAILED(hr)) {
            std::cerr << "GetService IAudioCaptureClient failed: " << hr << std::endl;
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            CoUninitialize();
            return;
        }

        hr = pAudioClient->Start();
        if (FAILED(hr)) {
            std::cerr << "Start IAudioClient failed: " << hr << std::endl;
            pCaptureClient->Release();
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            CoUninitialize();
            return;
        }

        LinearResampler resampler(pwfx->nSamplesPerSec, 16000.0);
        bool is_float = (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        bool is_extensible = (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE);

        if (is_extensible) {
            WAVEFORMATEXTENSIBLE* pExt = (WAVEFORMATEXTENSIBLE*)pwfx;
            if (pExt->SubFormat.Data1 == 0x00000003) {
                is_float = true;
            } else if (pExt->SubFormat.Data1 == 0x00000001) {
                is_float = false;
            }
        }

        std::cout << "  Format Type: " << (is_float ? "IEEE Float" : "PCM") << std::endl;
        std::cout << "WASAPI Audio Capture Thread started successfully." << std::endl;

        std::vector<float> native_mono_buffer;
        std::vector<float> resampled_buffer;

        while (is_running) {
            UINT32 packetLength = 0;
            hr = pCaptureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;

            if (packetLength == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            BYTE* pData = nullptr;
            UINT32 numFramesAvailable = 0;
            DWORD flags = 0;

            hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);
            if (FAILED(hr)) break;

            if (numFramesAvailable > 0) {
                native_mono_buffer.clear();
                native_mono_buffer.reserve(numFramesAvailable);

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    native_mono_buffer.assign(numFramesAvailable, 0.0f);
                } else {
                    if (is_float) {
                        float* pFloatData = (float*)pData;
                        std::vector<double> sum_sq(pwfx->nChannels, 0.0);
                        for (UINT32 i = 0; i < numFramesAvailable; ++i) {
                            for (WORD c = 0; c < pwfx->nChannels; ++c) {
                                float val = pFloatData[i * pwfx->nChannels + c];
                                sum_sq[c] += val * val;
                            }
                        }
                        WORD best_channel = 0;
                        for (WORD c = 1; c < pwfx->nChannels; ++c) {
                            if (sum_sq[c] > sum_sq[best_channel]) {
                                best_channel = c;
                            }
                        }
                        for (UINT32 i = 0; i < numFramesAvailable; ++i) {
                            // Pick the strongest raw mic channel when array enhancement is disabled.
                            native_mono_buffer.push_back(pFloatData[i * pwfx->nChannels + best_channel]);
                        }
                    } else if (pwfx->wBitsPerSample == 16) {
                        int16_t* pIntData = (int16_t*)pData;
                        std::vector<double> sum_sq(pwfx->nChannels, 0.0);
                        for (UINT32 i = 0; i < numFramesAvailable; ++i) {
                            for (WORD c = 0; c < pwfx->nChannels; ++c) {
                                float val = (float)pIntData[i * pwfx->nChannels + c] / 32768.0f;
                                sum_sq[c] += val * val;
                            }
                        }
                        WORD best_channel = 0;
                        for (WORD c = 1; c < pwfx->nChannels; ++c) {
                            if (sum_sq[c] > sum_sq[best_channel]) {
                                best_channel = c;
                            }
                        }
                        for (UINT32 i = 0; i < numFramesAvailable; ++i) {
                            native_mono_buffer.push_back((float)pIntData[i * pwfx->nChannels + best_channel] / 32768.0f);
                        }
                    }
                }

                if (!native_mono_buffer.empty()) {
                    resampled_buffer.clear();
                    resampler.resample(native_mono_buffer.data(), native_mono_buffer.size(), resampled_buffer);
                    if (!resampled_buffer.empty()) {
                        output_queue.push(resampled_buffer);
                    }
                }
            }

            hr = pCaptureClient->ReleaseBuffer(numFramesAvailable);
            if (FAILED(hr)) break;
        }

        pAudioClient->Stop();
        pCaptureClient->Release();
        CoTaskMemFree(pwfx);
        pAudioClient->Release();
        CoUninitialize();
        std::cout << "WASAPI Audio Capture Thread stopped." << std::endl;
    }

public:
    WasapiRecorder() = default;
    ~WasapiRecorder() {
        stop();
    }

    bool start() {
        if (is_running) return true;
        is_running = true;
        capture_thread = std::thread(&WasapiRecorder::run_capture, this);
        return true;
    }

    void stop() {
        if (!is_running) return;
        is_running = false;
        if (capture_thread.joinable()) {
            capture_thread.join();
        }
    }

    SafeQueue<std::vector<float>>& get_queue() {
        return output_queue;
    }
};

} // namespace audio
