// Native engine for the Krita plugin: stages 1 and 2 of the hybrid pipeline
// (U-Net Cleaner + ShapeClassifier) over TorchScript, exposed as a plain C ABI
// DLL. No pybind11 and no dependency on a Python version: the plugin loads it
// at runtime through its C ABI.
//
// Normalization identical to training (hybrid_vectorizer.to_network_input):
//   float32 = 1 - gray/255  (stroke=1, background=0), resized to 128x128.
// Interpolations chosen to reproduce cv2.INTER_AREA (downscale) and
// cv2.INTER_LINEAR (upscale) used in training and Python inference.

#ifdef _WIN32
#define HALINE_API extern "C" __declspec(dllexport)
#else
#define HALINE_API extern "C"
#endif

#include <torch/torch.h>
#include <torch/script.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {
constexpr int kWorkSize = 128;
constexpr double kMaskThreshold = 0.4;
constexpr int64_t kMinStrokePixels = 10;
constexpr double kMaxStrokeFraction = 0.25;
constexpr int kNumClasses = 6;

torch::jit::Module g_cleaner;
torch::jit::Module g_classifier;
bool g_loaded = false;
std::string g_error;

void setError(const std::string& message) { g_error = message; }

// uint8 [H,W] (light background, dark stroke) -> float32 [1,1,128,128], stroke=1.
torch::Tensor toNetworkInput(const uint8_t* gray, int width, int height) {
    auto input = torch::from_blob(
                     const_cast<uint8_t*>(gray), {height, width},
                     torch::TensorOptions().dtype(torch::kUInt8))
                     .to(torch::kFloat32)
                     .unsqueeze(0)
                     .unsqueeze(0);
    if (width != kWorkSize || height != kWorkSize) {
        // adaptive avg_pool with an integer factor == exact cv2.INTER_AREA.
        input = torch::adaptive_avg_pool2d(input, {kWorkSize, kWorkSize});
    }
    return 1.0 - input / 255.0;
}

// Classic 256-bin Otsu in C++ (equivalent to OpenCV's THRESH_OTSU), the
// deterministic fallback when the network output is degenerate.
int otsuThreshold(const uint8_t* gray, int64_t count) {
    double hist[256] = {0.0};
    for (int64_t i = 0; i < count; ++i) {
        hist[gray[i]] += 1.0;
    }
    double sum = 0.0, sumB = 0.0;
    double best = -1.0;
    int bestT = 127;
    long wB = 0;
    for (int i = 0; i < 256; ++i) {
        sum += hist[i] * i;
    }
    for (int t = 0; t < 256; ++t) {
        wB += static_cast<long>(hist[t]);
        if (wB == 0) {
            continue;
        }
        const long wF = count - wB;
        if (wF == 0) {
            break;
        }
        sumB += hist[t] * t;
        const double mB = sumB / wB;
        const double mF = (sum - sumB) / wF;
        const double between = static_cast<double>(wB) * wF * (mB - mF) * (mB - mF);
        if (between > best) {
            best = between;
            bestT = t;
        }
    }
    return bestT;
}
}  // namespace

// Copy (not ownership) of the Python contract:
//   haline_clean_stroke  -> stage 1; returns 0 = network, 1 = fallback, <0 = error.
//   haline_classify      -> stage 2; returns shape_id 0..5 or <0 on error.
//   haline_process       -> stages 1+2 in a single call (fewer border crossings
//                          per stroke; out_clean is optional).
// Buffers: contiguous [height, width] grayscale images, light background/dark stroke.

HALINE_API int32_t haline_load(const char* cleaner_path, const char* classifier_path) {
    try {
        torch::set_num_threads(4);
        g_cleaner = torch::jit::load(std::string(cleaner_path));
        g_cleaner.eval();
        if (classifier_path && classifier_path[0] != '\0') {
            g_classifier = torch::jit::load(std::string(classifier_path));
            g_classifier.eval();
        }
        g_loaded = true;
        setError("");
        return 0;
    } catch (const std::exception& exc) {
        g_loaded = false;
        setError(std::string("haline_load: ") + exc.what());
        return -1;
    }
}

HALINE_API const char* haline_last_error() { return g_error.c_str(); }

HALINE_API int32_t haline_clean_stroke(const uint8_t* gray,
                                       int32_t width,
                                       int32_t height,
                                       uint8_t* out_clean) {
    if (!g_loaded || !gray || !out_clean || width <= 0 || height <= 0) {
        setError("haline_clean_stroke: engine not loaded or invalid arguments");
        return -1;
    }
    try {
        torch::NoGradGuard no_grad;
        const torch::Tensor input = toNetworkInput(gray, width, height);
        torch::Tensor prob = g_cleaner.forward({input}).toTensor().sigmoid()[0][0];
        prob = at::upsample_bilinear2d(prob.unsqueeze(0).unsqueeze(0),
                                       {height, width}, false)[0][0];

        auto foreground = (prob > kMaskThreshold).to(torch::kUInt8).mul(255);
        const int64_t strokePixels = foreground.sum().item<int64_t>() / 255;
        const bool degenerate =
            strokePixels < kMinStrokePixels ||
            strokePixels > static_cast<int64_t>(width) * height * kMaxStrokeFraction;

        torch::Tensor result;
        const int32_t usedFallback = degenerate ? 1 : 0;
        if (degenerate) {
            const int threshold = otsuThreshold(gray, static_cast<int64_t>(width) * height);
            auto source = torch::from_blob(const_cast<uint8_t*>(gray), {height, width});
            result = (source <= threshold).to(torch::kUInt8).mul(255);
        } else {
            result = foreground;
        }
        result = 255 - result;  // black stroke on white background
        std::memcpy(out_clean, result.data_ptr<uint8_t>(),
                    static_cast<size_t>(width) * height);
        return usedFallback;
    } catch (const std::exception& exc) {
        setError(std::string("haline_clean_stroke: ") + exc.what());
        return -2;
    }
}

HALINE_API int32_t haline_classify(const uint8_t* clean,
                                   int32_t width,
                                   int32_t height,
                                   float* out_probs) {
    if (!g_loaded || !clean || width <= 0 || height <= 0) {
        setError("haline_classify: engine not loaded or invalid arguments");
        return -1;
    }
    try {
        torch::NoGradGuard no_grad;
        const torch::Tensor input = toNetworkInput(clean, width, height);
        torch::Tensor logits = g_classifier.forward({input}).toTensor()[0];
        const int64_t shapeId = logits.argmax().item<int64_t>();
        if (out_probs) {
            const torch::Tensor probs = torch::softmax(logits, 0);
            std::memcpy(out_probs, probs.data_ptr<float>(), sizeof(float) * kNumClasses);
        }
        return static_cast<int32_t>(shapeId);
    } catch (const std::exception& exc) {
        setError(std::string("haline_classify: ") + exc.what());
        return -2;
    }
}

HALINE_API int32_t haline_process(const uint8_t* gray,
                                  int32_t width,
                                  int32_t height,
                                  uint8_t* out_clean,
                                  float* out_probs) {
    if (!g_loaded || !gray || !out_clean || width <= 0 || height <= 0) {
        setError("haline_process: invalid arguments");
        return -1;
    }
    const int32_t fallback = haline_clean_stroke(gray, width, height, out_clean);
    if (fallback < 0) {
        return fallback;
    }
    const int32_t shapeId = haline_classify(out_clean, width, height, out_probs);
    if (shapeId < 0) {
        setError("haline_process: " + g_error);
        return shapeId;
    }
    return shapeId;
}
