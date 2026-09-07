// TinyEXR V3 WASM Benchmark Bindings
// Emscripten bindings for V3 API performance benchmarking

#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <vector>
#include <string>
#include <cstring>
#include <random>
#include <cmath>

// Include V3 C API
#include "tinyexr_c.h"

// Include tinyexr implementation (for miniz integration)
#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"

using namespace emscripten;

// Forward declarations
class EncodedImage;
class DecodedImage;

// Compression type names
static const char* compression_names[] = {
    "NONE", "RLE", "ZIPS", "ZIP", "PIZ", "PXR24", "B44", "B44A", "DWAA", "DWAB"
};

// ============================================================================
// EncodedImage - Result of encoding
// ============================================================================
class EncodedImage {
public:
    EncodedImage() : success_(false), encodeTimeMs_(0.0) {}

    bool ok() const { return success_; }
    std::string error() const { return error_; }

    val getBytes() const {
        if (data_.empty()) return val::null();
        return val(typed_memory_view(data_.size(), data_.data()));
    }

    size_t size() const { return data_.size(); }
    double encodeTimeMs() const { return encodeTimeMs_; }

    // Internal setters
    void setData(const uint8_t* data, size_t size) {
        data_.assign(data, data + size);
    }
    void setSuccess(bool s) { success_ = s; }
    void setError(const std::string& e) { error_ = e; success_ = false; }
    void setEncodeTime(double ms) { encodeTimeMs_ = ms; }

private:
    std::vector<uint8_t> data_;
    bool success_;
    std::string error_;
    double encodeTimeMs_;
};

// ============================================================================
// DecodedImage - Result of decoding
// ============================================================================
class DecodedImage {
public:
    DecodedImage() : width_(0), height_(0), numChannels_(0),
                     success_(false), decodeTimeMs_(0.0) {}

    bool ok() const { return success_; }
    std::string error() const { return error_; }

    val getBytes() const {
        if (data_.empty()) return val::null();
        return val(typed_memory_view(data_.size(), data_.data()));
    }

    int width() const { return width_; }
    int height() const { return height_; }
    int numChannels() const { return numChannels_; }
    double decodeTimeMs() const { return decodeTimeMs_; }

    // Internal setters
    void setData(std::vector<float>&& data) { data_ = std::move(data); }
    void setDimensions(int w, int h, int c) { width_ = w; height_ = h; numChannels_ = c; }
    void setSuccess(bool s) { success_ = s; }
    void setError(const std::string& e) { error_ = e; success_ = false; }
    void setDecodeTime(double ms) { decodeTimeMs_ = ms; }

private:
    std::vector<float> data_;
    int width_;
    int height_;
    int numChannels_;
    bool success_;
    std::string error_;
    double decodeTimeMs_;
};

// ============================================================================
// RandomImageData - Holds generated random image
// ============================================================================
class RandomImageData {
public:
    RandomImageData() : width_(0), height_(0) {}
    RandomImageData(int w, int h, std::vector<float>&& data)
        : width_(w), height_(h), data_(std::move(data)) {}

    int width() const { return width_; }
    int height() const { return height_; }
    size_t size() const { return data_.size() * sizeof(float); }
    const float* data() const { return data_.data(); }

    val getBytes() const {
        if (data_.empty()) return val::null();
        return val(typed_memory_view(data_.size(), data_.data()));
    }

private:
    int width_;
    int height_;
    std::vector<float> data_;
};

// ============================================================================
// V3BenchmarkContext - Main context wrapper
// ============================================================================
class V3BenchmarkContext {
public:
    V3BenchmarkContext() : ctx_(nullptr) {
        ExrContextCreateInfo info = {};
        info.api_version = TINYEXR_C_API_VERSION;
        info.flags = EXR_CONTEXT_SINGLE_THREADED; // WASM is single-threaded
        info.max_threads = 1;

        ExrResult res = exr_context_create(&info, &ctx_);
        if (res != EXR_SUCCESS) {
            ctx_ = nullptr;
        }
    }

    ~V3BenchmarkContext() {
        if (ctx_) {
            exr_context_destroy(ctx_);
        }
    }

    // Check if context is valid
    bool isValid() const { return ctx_ != nullptr; }

    // Encode image from RandomImageData
    EncodedImage encodeImageFromData(const RandomImageData& imageData, int compressionType) {
        return encodeImageInternal(
            imageData.width(), imageData.height(), 4,
            imageData.data(), imageData.size(),
            compressionType
        );
    }

    // Internal encoding implementation
    EncodedImage encodeImageInternal(int width, int height, int numChannels,
                                      const float* pixelData, size_t dataSize,
                                      int compressionType) {
        EncodedImage result;

        if (!ctx_) {
            result.setError("Context not initialized");
            return result;
        }

        size_t expectedSize = (size_t)width * height * numChannels * sizeof(float);
        if (dataSize != expectedSize) {
            result.setError("Invalid pixel data size: expected " +
                           std::to_string(expectedSize) + ", got " + std::to_string(dataSize));
            return result;
        }

        double startTime = emscripten_get_now();

        // Create data sink to memory
        ExrDataSink sink = {};
        void* outputData = nullptr;
        size_t outputSize = 0;

        ExrResult res = exr_data_sink_to_memory(ctx_, &sink, &outputData, &outputSize);
        if (res != EXR_SUCCESS) {
            result.setError(std::string("Failed to create sink: ") + exr_result_to_string(res));
            return result;
        }

        // Create encoder
        ExrEncoderCreateInfo encInfo = {};
        encInfo.sink = sink;

        ExrEncoder encoder = nullptr;
        res = exr_encoder_create(ctx_, &encInfo, &encoder);
        if (res != EXR_SUCCESS) {
            result.setError(std::string("Failed to create encoder: ") + exr_result_to_string(res));
            return result;
        }

        // Set up channel info (RGBA or RGB)
        std::vector<ExrWriteChannelInfo> channels(numChannels);
        const char* channelNames[] = {"R", "G", "B", "A"};
        for (int i = 0; i < numChannels && i < 4; i++) {
            channels[i].name = channelNames[i];
            channels[i].pixel_type = EXR_PIXEL_FLOAT;
            channels[i].x_sampling = 1;
            channels[i].y_sampling = 1;
            channels[i].p_linear = 0;
        }

        // Create write image
        ExrWriteImageCreateInfo imgInfo = {};
        imgInfo.width = width;
        imgInfo.height = height;
        imgInfo.num_channels = numChannels;
        imgInfo.channels = channels.data();
        imgInfo.compression = compressionType;
        imgInfo.compression_level = 6; // Default ZIP level
        imgInfo.flags = 0; // Scanline mode
        imgInfo.tile_size_x = 0;
        imgInfo.tile_size_y = 0;

        ExrWriteImage writeImage = nullptr;
        res = exr_write_image_create(encoder, &imgInfo, &writeImage);
        if (res != EXR_SUCCESS) {
            exr_encoder_destroy(encoder);
            result.setError(std::string("Failed to create write image: ") + exr_result_to_string(res));
            return result;
        }

        // Create command buffer
        ExrCommandBufferCreateInfo cmdInfo = {};
        cmdInfo.encoder = encoder;

        ExrCommandBuffer cmd = nullptr;
        res = exr_command_buffer_create(ctx_, &cmdInfo, &cmd);
        if (res != EXR_SUCCESS) {
            exr_write_image_destroy(writeImage);
            exr_encoder_destroy(encoder);
            result.setError(std::string("Failed to create command buffer: ") + exr_result_to_string(res));
            return result;
        }

        // Begin recording
        res = exr_command_buffer_begin(cmd);
        if (res != EXR_SUCCESS) {
            exr_command_buffer_destroy(cmd);
            exr_write_image_destroy(writeImage);
            exr_encoder_destroy(encoder);
            result.setError(std::string("Failed to begin command buffer: ") + exr_result_to_string(res));
            return result;
        }

        // Write all scanlines
        ExrScanlineWrite writeReq = {};
        writeReq.image = writeImage;
        writeReq.y_start = 0;
        writeReq.num_lines = height;
        writeReq.input.data = (void*)pixelData;
        writeReq.input.size = dataSize;
        writeReq.input.offset = 0;
        writeReq.input_layout = EXR_LAYOUT_INTERLEAVED;
        writeReq.input_pixel_type = EXR_PIXEL_FLOAT;

        res = exr_cmd_write_scanlines(cmd, &writeReq);
        if (res != EXR_SUCCESS) {
            exr_command_buffer_destroy(cmd);
            exr_write_image_destroy(writeImage);
            exr_encoder_destroy(encoder);
            result.setError(std::string("Failed to write scanlines: ") + exr_result_to_string(res));
            return result;
        }

        res = exr_command_buffer_end(cmd);
        if (res != EXR_SUCCESS) {
            exr_command_buffer_destroy(cmd);
            exr_write_image_destroy(writeImage);
            exr_encoder_destroy(encoder);
            result.setError(std::string("Failed to end command buffer: ") + exr_result_to_string(res));
            return result;
        }

        // Submit
        ExrSubmitInfo submitInfo = {};
        submitInfo.command_buffer_count = 1;
        submitInfo.command_buffers = &cmd;

        res = exr_submit_write(encoder, &submitInfo);
        if (res != EXR_SUCCESS) {
            exr_command_buffer_destroy(cmd);
            exr_write_image_destroy(writeImage);
            exr_encoder_destroy(encoder);
            result.setError(std::string("Failed to submit write: ") + exr_result_to_string(res));
            return result;
        }

        // Finalize
        res = exr_encoder_finalize(encoder);
        if (res != EXR_SUCCESS) {
            exr_command_buffer_destroy(cmd);
            exr_write_image_destroy(writeImage);
            exr_encoder_destroy(encoder);
            result.setError(std::string("Failed to finalize: ") + exr_result_to_string(res));
            return result;
        }

        // Copy result
        result.setData((const uint8_t*)outputData, outputSize);
        result.setSuccess(true);
        result.setEncodeTime(emscripten_get_now() - startTime);

        // Cleanup
        exr_command_buffer_destroy(cmd);
        exr_write_image_destroy(writeImage);
        exr_encoder_destroy(encoder);

        return result;
    }

    // Decode EXR data from EncodedImage
    DecodedImage decodeImageFromEncoded(const EncodedImage& encoded) {
        DecodedImage result;

        if (!ctx_) {
            result.setError("Context not initialized");
            return result;
        }

        if (!encoded.ok()) {
            result.setError("Input encoded image is invalid");
            return result;
        }

        // Get the internal data
        val jsBytes = encoded.getBytes();
        if (jsBytes.isNull()) {
            result.setError("No data in encoded image");
            return result;
        }

        // Get pointer to the data directly from the encoded image
        // Since we're in the same address space, we can access the vector directly
        // But embind doesn't give us direct access, so we need to use the encoded data
        result.setError("Use decodeFromEncodedData instead");
        return result;
    }

    // Decode EXR data from internal buffer (called from JS with the data)
    DecodedImage decodeFromBuffer(uintptr_t dataPtr, size_t dataSize) {
        DecodedImage result;

        if (!ctx_) {
            result.setError("Context not initialized");
            return result;
        }

        const void* exrData = reinterpret_cast<const void*>(dataPtr);

        double startTime = emscripten_get_now();

        // Create data source from memory
        ExrDataSource source = {};
        ExrResult res = exr_data_source_from_memory(exrData, dataSize, &source);
        if (res != EXR_SUCCESS) {
            result.setError(std::string("Failed to create source: ") + exr_result_to_string(res));
            return result;
        }

        // Create decoder
        ExrDecoderCreateInfo decInfo = {};
        decInfo.source = source;

        ExrDecoder decoder = nullptr;
        res = exr_decoder_create(ctx_, &decInfo, &decoder);
        if (res != EXR_SUCCESS) {
            result.setError(std::string("Failed to create decoder: ") + exr_result_to_string(res));
            return result;
        }

        // Parse header
        ExrImage image = nullptr;
        res = exr_decoder_parse_header(decoder, &image);
        if (res != EXR_SUCCESS) {
            exr_decoder_destroy(decoder);
            result.setError(std::string("Failed to parse header: ") + exr_result_to_string(res));
            return result;
        }

        // Get image info
        ExrImageInfo imgInfo = {};
        res = exr_image_get_info(image, &imgInfo);
        if (res != EXR_SUCCESS) {
            exr_image_destroy(image);
            exr_decoder_destroy(decoder);
            result.setError(std::string("Failed to get image info: ") + exr_result_to_string(res));
            return result;
        }

        int width = imgInfo.width;
        int height = imgInfo.height;
        int numChannels = imgInfo.num_channels;

        result.setDimensions(width, height, numChannels);

        // Get first part
        ExrPart part = nullptr;
        res = exr_image_get_part(image, 0, &part);
        if (res != EXR_SUCCESS) {
            exr_image_destroy(image);
            exr_decoder_destroy(decoder);
            result.setError(std::string("Failed to get part: ") + exr_result_to_string(res));
            return result;
        }

        // Allocate output buffer
        size_t bufferSize = (size_t)width * height * numChannels * sizeof(float);
        std::vector<float> outputData(width * height * numChannels);

        // Create command buffer
        ExrCommandBufferCreateInfo cmdInfo = {};
        cmdInfo.decoder = decoder;

        ExrCommandBuffer cmd = nullptr;
        res = exr_command_buffer_create(ctx_, &cmdInfo, &cmd);
        if (res != EXR_SUCCESS) {
            exr_part_destroy(part);
            exr_image_destroy(image);
            exr_decoder_destroy(decoder);
            result.setError(std::string("Failed to create command buffer: ") + exr_result_to_string(res));
            return result;
        }

        // Begin recording
        res = exr_command_buffer_begin(cmd);
        if (res != EXR_SUCCESS) {
            exr_command_buffer_destroy(cmd);
            exr_part_destroy(part);
            exr_image_destroy(image);
            exr_decoder_destroy(decoder);
            result.setError(std::string("Failed to begin command buffer: ") + exr_result_to_string(res));
            return result;
        }

        // Request full image
        ExrFullImageRequest fullReq = {};
        fullReq.part = part;
        fullReq.output.data = outputData.data();
        fullReq.output.size = bufferSize;
        fullReq.output.offset = 0;
        fullReq.channels_mask = 0; // All channels
        fullReq.output_pixel_type = EXR_PIXEL_FLOAT;
        fullReq.output_layout = EXR_LAYOUT_INTERLEAVED;
        fullReq.target_level = 0;

        res = exr_cmd_request_full_image(cmd, &fullReq);
        if (res != EXR_SUCCESS) {
            exr_command_buffer_destroy(cmd);
            exr_part_destroy(part);
            exr_image_destroy(image);
            exr_decoder_destroy(decoder);
            result.setError(std::string("Failed to request image: ") + exr_result_to_string(res));
            return result;
        }

        res = exr_command_buffer_end(cmd);
        if (res != EXR_SUCCESS) {
            exr_command_buffer_destroy(cmd);
            exr_part_destroy(part);
            exr_image_destroy(image);
            exr_decoder_destroy(decoder);
            result.setError(std::string("Failed to end command buffer: ") + exr_result_to_string(res));
            return result;
        }

        // Submit
        ExrSubmitInfo submitInfo = {};
        submitInfo.command_buffer_count = 1;
        submitInfo.command_buffers = &cmd;

        res = exr_submit(decoder, &submitInfo);
        if (res != EXR_SUCCESS) {
            exr_command_buffer_destroy(cmd);
            exr_part_destroy(part);
            exr_image_destroy(image);
            exr_decoder_destroy(decoder);
            result.setError(std::string("Failed to submit: ") + exr_result_to_string(res));
            return result;
        }

        // Wait for completion
        res = exr_decoder_wait_idle(decoder);
        if (res != EXR_SUCCESS) {
            exr_command_buffer_destroy(cmd);
            exr_part_destroy(part);
            exr_image_destroy(image);
            exr_decoder_destroy(decoder);
            result.setError(std::string("Failed to wait: ") + exr_result_to_string(res));
            return result;
        }

        result.setData(std::move(outputData));
        result.setSuccess(true);
        result.setDecodeTime(emscripten_get_now() - startTime);

        // Cleanup
        exr_command_buffer_destroy(cmd);
        exr_part_destroy(part);
        exr_image_destroy(image);
        exr_decoder_destroy(decoder);

        return result;
    }

    // Generate random HDR image data
    static RandomImageData generateRandomImage(int width, int height, int seed) {
        const int numChannels = 4; // RGBA
        std::vector<float> data(width * height * numChannels);

        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);

        // HDR exposure range: 2^-6 to 2^6
        const float minEV = -6.0f;
        const float maxEV = 6.0f;

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                size_t idx = ((size_t)y * width + x) * numChannels;

                // Base exposure varies with X (gradient)
                float baseEV = minEV + (maxEV - minEV) * ((float)x / width);

                for (int c = 0; c < numChannels; c++) {
                    if (c == 3) {
                        // Alpha: 0.0 to 1.0
                        data[idx + c] = dist(rng);
                    } else {
                        // Color channels: HDR values with noise
                        float noise = dist(rng) * 2.0f;
                        float ev = baseEV + noise + (c * 0.5f);
                        data[idx + c] = std::pow(2.0f, ev) * dist(rng);
                    }
                }
            }
        }

        return RandomImageData(width, height, std::move(data));
    }

    // Get compression type name
    static std::string getCompressionName(int type) {
        if (type >= 0 && type <= 9) {
            return compression_names[type];
        }
        return "UNKNOWN";
    }

    // Get WASM memory info
    static val getMemoryInfo() {
        val obj = val::object();

        // Get Emscripten heap size
        size_t heapSize = EM_ASM_INT({
            return HEAPU8.length;
        });

        obj.set("heapTotal", (double)heapSize);
        obj.set("heapUsed", (double)heapSize); // Approximate

        return obj;
    }

private:
    ExrContext ctx_;
};

// ============================================================================
// Emscripten Bindings
// ============================================================================
EMSCRIPTEN_BINDINGS(tinyexr_v3_benchmark) {
    class_<EncodedImage>("EncodedImage")
        .function("ok", &EncodedImage::ok)
        .function("error", &EncodedImage::error)
        .function("getBytes", &EncodedImage::getBytes)
        .function("size", &EncodedImage::size)
        .function("encodeTimeMs", &EncodedImage::encodeTimeMs);

    class_<DecodedImage>("DecodedImage")
        .function("ok", &DecodedImage::ok)
        .function("error", &DecodedImage::error)
        .function("getBytes", &DecodedImage::getBytes)
        .function("width", &DecodedImage::width)
        .function("height", &DecodedImage::height)
        .function("numChannels", &DecodedImage::numChannels)
        .function("decodeTimeMs", &DecodedImage::decodeTimeMs);

    class_<RandomImageData>("RandomImageData")
        .function("width", &RandomImageData::width)
        .function("height", &RandomImageData::height)
        .function("size", &RandomImageData::size)
        .function("getBytes", &RandomImageData::getBytes);

    class_<V3BenchmarkContext>("V3BenchmarkContext")
        .constructor<>()
        .function("isValid", &V3BenchmarkContext::isValid)
        .function("encodeImageFromData", &V3BenchmarkContext::encodeImageFromData)
        .function("decodeFromBuffer", &V3BenchmarkContext::decodeFromBuffer)
        .class_function("generateRandomImage", &V3BenchmarkContext::generateRandomImage)
        .class_function("getCompressionName", &V3BenchmarkContext::getCompressionName)
        .class_function("getMemoryInfo", &V3BenchmarkContext::getMemoryInfo);

    // Compression type constants
    constant("COMPRESSION_NONE", (int)EXR_COMPRESSION_NONE);
    constant("COMPRESSION_RLE", (int)EXR_COMPRESSION_RLE);
    constant("COMPRESSION_ZIPS", (int)EXR_COMPRESSION_ZIPS);
    constant("COMPRESSION_ZIP", (int)EXR_COMPRESSION_ZIP);
    constant("COMPRESSION_PIZ", (int)EXR_COMPRESSION_PIZ);
    constant("COMPRESSION_PXR24", (int)EXR_COMPRESSION_PXR24);
    constant("COMPRESSION_B44", (int)EXR_COMPRESSION_B44);
    constant("COMPRESSION_B44A", (int)EXR_COMPRESSION_B44A);
}
