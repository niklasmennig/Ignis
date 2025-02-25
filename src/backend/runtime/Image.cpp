#include "Image.h"
#include "ImageIO.h"
#include "Logger.h"

IG_BEGIN_IGNORE_WARNINGS
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

// We already make use of zlib, so use it here as well
#include <zlib.h>
#define TINYEXR_USE_THREAD (1)
#define TINYEXR_USE_MINIZ (0)
// #define TINYEXR_IMPLEMENTATION // Already included in ImageIO.cpp
#include <tinyexr.h>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
IG_END_IGNORE_WARNINGS

namespace IG {

static inline float srgb_gamma(float c)
{
    if (c <= 0.0031308f)
        return 12.92f * c;
    else
        return 1.055f * std::pow(c, 1 / 2.4f) - 0.055f;
}

static inline float srgb_invgamma(float c)
{
    if (c <= 0.04045f)
        return c / 12.92f;
    else
        return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

static inline uint8 byte_color_to_linear(uint8 c)
{
    return static_cast<uint8>(std::min<uint16>(255, static_cast<uint16>(std::floor(srgb_invgamma(c / 255.0f) * 255))));
}

void Image::applyGammaCorrection(bool inverse, bool sRGB)
{
    IG_ASSERT(isValid(), "Expected valid image");

    if (!sRGB) {
        const float factor = !inverse ? 1 / 2.2f : 2.2f;

        tbb::parallel_for(
            tbb::blocked_range<size_t>(0, width * height),
            [&](tbb::blocked_range<size_t> r) {
                for (size_t k = r.begin(); k < r.end(); ++k) {
                    auto* pix = &pixels[4 * k];
                    for (int i = 0; i < 3; ++i)
                        pix[i] = std::pow(pix[i], factor);
                }
            });
    } else {
        if (!inverse) {
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, width * height),
                [&](tbb::blocked_range<size_t> r) {
                    for (size_t k = r.begin(); k < r.end(); ++k) {
                        auto* pix = &pixels[4 * k];
                        for (int i = 0; i < 3; ++i)
                            pix[i] = srgb_gamma(pix[i]);
                    }
                });
        } else {
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, width * height),
                [&](tbb::blocked_range<size_t> r) {
                    for (size_t k = r.begin(); k < r.end(); ++k) {
                        auto* pix = &pixels[4 * k];
                        for (int i = 0; i < 3; ++i)
                            pix[i] = srgb_invgamma(pix[i]);
                    }
                });
        }
    }
}

void Image::flipY()
{
    const size_t slice = 4 * width;
    for (size_t y = 0; y < height / 2; ++y) {
        float* s1 = &pixels[y * slice];
        float* s2 = &pixels[(height - y - 1) * slice];
        if (s1 != s2)
            std::swap_ranges(s1, s1 + slice, s2);
    }
}

static inline uint32 pack(uint8 r, uint8 g, uint8 b, uint8 a)
{
    return uint32(r) | (uint32(g) << 8) | (uint32(b) << 16) | (uint32(a) << 24);
}

void Image::copyToPackedFormat(std::vector<uint32>& dst) const
{
    dst.resize(width * height);

    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, width * height),
        [&](tbb::blocked_range<size_t> range) {
            for (size_t k = range.begin(); k < range.end(); ++k) {
                uint8 r = static_cast<uint8>(static_cast<uint32>(pixels[4 * k + 0] * 255) & 0xFF);
                uint8 g = static_cast<uint8>(static_cast<uint32>(pixels[4 * k + 1] * 255) & 0xFF);
                uint8 b = static_cast<uint8>(static_cast<uint32>(pixels[4 * k + 2] * 255) & 0xFF);
                uint8 a = static_cast<uint8>(static_cast<uint32>(pixels[4 * k + 3] * 255) & 0xFF);
                dst[k]  = pack(r, g, b, a);
            }
        });
}

inline bool ends_with(std::string const& value, std::string const& ending)
{
    if (ending.size() > value.size())
        return false;
    return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}

bool Image::isPacked(const std::filesystem::path& path)
{
    std::string ext   = path.extension().generic_u8string();
    const bool useExr = ends_with(ext, ".exr");
    const bool useHdr = ends_with(ext, ".hdr");

    return !useExr && !useHdr;
}

bool Image::hasAlphaChannel(const std::filesystem::path& path)
{
    if (isPacked(path))
        return true; // Just assume they have

    int width = 0, height = 0, channels = 0;
    stbi_info(path.generic_u8string().c_str(), &width, &height, &channels);
    return channels == 4;
}

static inline std::string getStringAttribute(const EXRAttribute& attr)
{
    int len;
    memcpy(&len, attr.value, sizeof(len));

    std::string str;
    str.resize(len);
    memcpy(str.data(), attr.value + sizeof(len), len);

    return str;
}

static inline Vector3f getVec3Attribute(const EXRAttribute& attr)
{
    float xyz[3];
    memcpy(xyz, attr.value, 3 * sizeof(float));
    return Vector3f(xyz[0], xyz[1], xyz[2]);
}

static inline int getIntAttribute(const EXRAttribute& attr)
{
    return *reinterpret_cast<const int*>(attr.value);
}

Image Image::load(const std::filesystem::path& path, ImageMetaData* metaData)
{
    std::string ext   = path.extension().generic_u8string();
    const bool useExr = ends_with(ext, ".exr");
    const bool useHdr = ends_with(ext, ".hdr");

    Image img;

    if (useExr) {
        EXRVersion exr_version;
        int ret = ParseEXRVersionFromFile(&exr_version, path.generic_u8string().c_str());
        if (ret != 0)
            throw ImageLoadException("Could not extract exr version information", path);

        EXRHeader exr_header;
        InitEXRHeader(&exr_header);

        const char* err = nullptr;
        ret             = ParseEXRHeaderFromFile(&exr_header, &exr_version, path.generic_u8string().c_str(), &err);
        if (ret != 0) {
            std::string _err = err;
            FreeEXRErrorMessage(err);
            throw ImageLoadException(_err, path);
        }

        // Load meta data if necessary
        if (metaData) {
            for (int i = 0; i < exr_header.num_custom_attributes; ++i) {
                const auto& attr = exr_header.custom_attributes[i];
                if (strcmp(attr.name, "igCameraType") == 0 && strcmp(attr.type, "string") == 0)
                    metaData->CameraType = getStringAttribute(attr);
                else if (strcmp(attr.name, "igTechniqueType") == 0 && strcmp(attr.type, "string") == 0)
                    metaData->TechniqueType = getStringAttribute(attr);
                else if (strcmp(attr.name, "igCameraEye") == 0 && strcmp(attr.type, "v3f") == 0)
                    metaData->CameraEye = getVec3Attribute(attr);
                else if (strcmp(attr.name, "igCameraUp") == 0 && strcmp(attr.type, "v3f") == 0)
                    metaData->CameraUp = getVec3Attribute(attr);
                else if (strcmp(attr.name, "igCameraDir") == 0 && strcmp(attr.type, "v3f") == 0)
                    metaData->CameraDir = getVec3Attribute(attr);
                else if (strcmp(attr.name, "igSPP") == 0 && strcmp(attr.type, "int") == 0)
                    metaData->SamplePerPixel = getIntAttribute(attr);
            }
        }

        // Make sure exr loads full floating point
        for (int i = 0; i < exr_header.num_channels; i++) {
            if (exr_header.pixel_types[i] == TINYEXR_PIXELTYPE_HALF)
                exr_header.requested_pixel_types[i] = TINYEXR_PIXELTYPE_FLOAT;
        }

        EXRImage exr_image;
        InitEXRImage(&exr_image);
        ret = LoadEXRImageFromFile(&exr_image, &exr_header, path.generic_u8string().c_str(), &err);
        if (ret != TINYEXR_SUCCESS) {
            std::string _err = err;
            FreeEXRErrorMessage(err);
            FreeEXRHeader(&exr_header);
            throw ImageLoadException(_err, path);
        }

        img.width  = exr_image.width;
        img.height = exr_image.height;

        // Extract channel information
        int idxR = -1;
        int idxG = -1;
        int idxB = -1;
        int idxA = -1;
        int idxY = -1;

        int channels = 0;
        for (int c = 0; c < exr_header.num_channels; ++c) {
            const std::string name = to_lowercase(std::string(exr_header.channels[c].name));

            if (name == "a" || name == "default.a")
                idxA = c;
            else if (name == "r" || name == "default.r")
                idxR = c;
            else if (name == "g" || name == "default.g")
                idxG = c;
            else if (name == "b" || name == "default.b")
                idxB = c;
            else if (name == "y" || name == "default.y")
                idxY = c;
            ++channels;
        }

        img.pixels.reset(new float[img.width * img.height * 4]);

        // TODO: Tiled
        if (channels == 1) {
            int idx = idxY != -1 ? idxY : idxA;
            for (size_t i = 0; i < img.width * img.height; ++i) {
                float g               = reinterpret_cast<float**>(exr_image.images)[idx][i];
                img.pixels[i * 4 + 0] = g;
                img.pixels[i * 4 + 1] = g;
                img.pixels[i * 4 + 2] = g;
                img.pixels[i * 4 + 3] = 1;
            }
        } else {
            if (idxR == -1 || idxG == -1 || idxB == -1) {
                FreeEXRHeader(&exr_header);
                FreeEXRImage(&exr_image);
                return {}; // TODO: Error message
            }

            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, img.width * img.height),
                [&](tbb::blocked_range<size_t> r) {
                    for (size_t i = r.begin(); i < r.end(); ++i) {
                        img.pixels[4 * i + 0] = reinterpret_cast<float**>(exr_image.images)[idxR][i];
                        img.pixels[4 * i + 1] = reinterpret_cast<float**>(exr_image.images)[idxG][i];
                        img.pixels[4 * i + 2] = reinterpret_cast<float**>(exr_image.images)[idxB][i];
                        if (idxA != -1)
                            img.pixels[4 * i + 3] = reinterpret_cast<float**>(exr_image.images)[idxA][i];
                        else
                            img.pixels[4 * i + 3] = 1;
                    }
                });
        }

        FreeEXRHeader(&exr_header);
        FreeEXRImage(&exr_image);
    } else {
        stbi_set_unpremultiply_on_load(1);
        stbi_set_flip_vertically_on_load(0);

        int width = 0, height = 0, channels = 0;
        float* data = stbi_loadf(path.generic_u8string().c_str(), &width, &height, &channels, 0);

        if (data == nullptr)
            throw ImageLoadException("Could not load image", path);

        img.width  = width;
        img.height = height;
        img.pixels.reset(new float[img.width * img.height * 4]);

        switch (channels) {
        case 0:
            return Image();
        case 1: // Gray
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, img.width * img.height),
                [&](tbb::blocked_range<size_t> r) {
                    for (size_t i = r.begin(); i < r.end(); ++i) {
                        float g               = data[i];
                        img.pixels[i * 4 + 0] = g;
                        img.pixels[i * 4 + 1] = g;
                        img.pixels[i * 4 + 2] = g;
                        img.pixels[i * 4 + 3] = 1;
                    }
                });
            break;
        case 3: // RGB
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, img.width * img.height),
                [&](tbb::blocked_range<size_t> r) {
                    for (size_t i = r.begin(); i < r.end(); ++i) {
                        img.pixels[i * 4 + 0] = data[i * 3 + 0];
                        img.pixels[i * 4 + 1] = data[i * 3 + 1];
                        img.pixels[i * 4 + 2] = data[i * 3 + 2];
                        img.pixels[i * 4 + 3] = 1;
                    }
                });
            break;
        case 4: // RGBA
            std::memcpy(img.pixels.get(), data, sizeof(float) * 4 * img.width * img.height);
            break;
        default:
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, img.width * img.height),
                [&](tbb::blocked_range<size_t> r) {
                    for (size_t i = r.begin(); i < r.end(); ++i) {
                        img.pixels[i * 4 + 0] = data[i * channels + 1];
                        img.pixels[i * 4 + 1] = data[i * channels + 2];
                        img.pixels[i * 4 + 2] = data[i * channels + 3];
                        img.pixels[i * 4 + 3] = data[i * channels + 0];
                    }
                });
            break;
        }
        stbi_image_free(data);

        // Linearize data
    }

    // Do not flip hdr images (which are fixed to -Y N +X M resolution by stb)
    if (!useHdr)
        img.flipY();
    return img;
}

void Image::loadAsPacked(const std::filesystem::path& path, std::vector<uint32>& dst, size_t& width, size_t& height)
{
    std::string ext   = path.extension().generic_u8string();
    const bool useExr = ends_with(ext, ".exr");
    const bool useHdr = ends_with(ext, ".hdr");

    if (useExr || useHdr)
        throw ImageLoadException("Can not load EXR or HDR as packed", path);

    stbi_set_unpremultiply_on_load(1);
    stbi_set_flip_vertically_on_load(1);

    int width2 = 0, height2 = 0, channels = 0;
    stbi_uc* data = stbi_load(path.generic_u8string().c_str(), &width2, &height2, &channels, 4);

    width  = static_cast<size_t>(width2);
    height = static_cast<size_t>(height2);

    if (data == nullptr)
        throw ImageLoadException("Could not load image", path);

    dst.resize(width * height);

    // Pack data and map to linear space
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, width * height),
        [&](tbb::blocked_range<size_t> r) {
            for (size_t i = r.begin(); i < r.end(); ++i) {
                dst[i] = pack(byte_color_to_linear(data[i * 4 + 0]), byte_color_to_linear(data[i * 4 + 1]), byte_color_to_linear(data[i * 4 + 2]), data[i * 4 + 3]);
            }
        });

    stbi_image_free(data);
}

bool Image::save(const std::filesystem::path& path)
{
    return save(path, pixels.get(), width, height);
}

bool Image::save(const std::filesystem::path& path, const float* rgba, size_t width, size_t height, bool skip_alpha)
{
    std::string ext = path.extension().generic_u8string();
    bool useExr     = ends_with(ext, ".exr");

    // We only support .exr output
    if (!useExr) {
        IG_LOG(L_ERROR) << "Ignis only supports EXR format as output!" << std::endl;
        return false;
    }

    size_t channels = skip_alpha ? 3 : 4;
    std::vector<std::vector<float>> images(channels);
    for (size_t i = 0; i < channels; ++i)
        images[i].resize(width * height);

    // Split into layers
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, width * height),
        [&](tbb::blocked_range<size_t> r) {
            for (size_t i = r.begin(); i < r.end(); ++i) {
                for (size_t j = 0; j < channels; ++j)
                    images[j][i] = rgba[4 * i + j];
            }
        });

    std::vector<const float*> image_ptrs(channels);
    for (size_t i = 0; i < channels; ++i)
        image_ptrs[i] = &(images[channels - i - 1].at(0));

    return ImageIO::save(path, width, height, image_ptrs,
                         skip_alpha ? std::vector<std::string>{ "B", "G", "R" } : std::vector<std::string>{ "A", "B", "G", "R" });
}

} // namespace IG