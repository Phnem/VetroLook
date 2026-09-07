// Spectral EXR read/write example using TinyEXR V1 API
// Based on JCGT 2021 paper: https://jcgt.org/published/0010/03/01/
// Reference implementation: https://github.com/afichet/spectral-exr

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"

// Sample wavelengths for visible spectrum (400-700nm in 10nm steps)
static const float kVisibleWavelengths[] = {
  400.0f, 410.0f, 420.0f, 430.0f, 440.0f, 450.0f, 460.0f, 470.0f, 480.0f, 490.0f,
  500.0f, 510.0f, 520.0f, 530.0f, 540.0f, 550.0f, 560.0f, 570.0f, 580.0f, 590.0f,
  600.0f, 610.0f, 620.0f, 630.0f, 640.0f, 650.0f, 660.0f, 670.0f, 680.0f, 690.0f,
  700.0f
};
static const int kNumWavelengths = sizeof(kVisibleWavelengths) / sizeof(kVisibleWavelengths[0]);

// Simple blackbody spectrum approximation (Planck's law)
// Returns spectral radiance at given wavelength and temperature
float BlackbodySpectrum(float wavelength_nm, float temperature_K) {
  const double h = 6.62607015e-34;  // Planck constant
  const double c = 299792458.0;      // Speed of light
  const double k = 1.380649e-23;     // Boltzmann constant

  double lambda = wavelength_nm * 1e-9;  // Convert to meters
  double lambda5 = lambda * lambda * lambda * lambda * lambda;

  double numerator = 2.0 * h * c * c;
  double denominator = lambda5 * (exp((h * c) / (lambda * k * temperature_K)) - 1.0);

  // Scale to reasonable range and return
  return static_cast<float>(numerator / denominator * 1e-12);
}

// Create a simple test spectral image
bool CreateSpectralImage(const char* filename, int width, int height,
                         int spectrum_type, float temperature_K) {
  printf("Creating spectral EXR: %s (%dx%d, %d wavelengths)\n",
         filename, width, height, kNumWavelengths);

  // Initialize header
  EXRHeader header;
  InitEXRHeader(&header);

  // Set spectral attributes
  int ret = EXRSetSpectralAttributes(&header, spectrum_type,
    spectrum_type == TINYEXR_SPECTRUM_REFLECTIVE ? "reflectance" : "W.m^-2.sr^-1.nm^-1");
  if (ret != TINYEXR_SUCCESS) {
    printf("Failed to set spectral attributes\n");
    return false;
  }

  // Set up channels - one per wavelength
  header.num_channels = kNumWavelengths;
  header.channels = static_cast<EXRChannelInfo*>(
      malloc(sizeof(EXRChannelInfo) * header.num_channels));
  header.pixel_types = static_cast<int*>(malloc(sizeof(int) * header.num_channels));
  header.requested_pixel_types = static_cast<int*>(malloc(sizeof(int) * header.num_channels));

  // Create channel names in sorted order (EXR requires alphabetical order)
  std::vector<std::pair<std::string, int>> channel_names;
  for (int i = 0; i < kNumWavelengths; i++) {
    char name[64];
    if (spectrum_type == TINYEXR_SPECTRUM_REFLECTIVE) {
      EXRReflectiveChannelName(name, sizeof(name), kVisibleWavelengths[i]);
    } else {
      EXRSpectralChannelName(name, sizeof(name), kVisibleWavelengths[i], 0);
    }
    channel_names.push_back({name, i});
  }

  // Sort alphabetically (required by EXR format)
  std::sort(channel_names.begin(), channel_names.end());

  // Create mapping from sorted order to wavelength order
  std::vector<int> wavelength_order(kNumWavelengths);
  for (int i = 0; i < kNumWavelengths; i++) {
    wavelength_order[i] = channel_names[i].second;
  }

  for (int i = 0; i < kNumWavelengths; i++) {
    strncpy(header.channels[i].name, channel_names[i].first.c_str(), 255);
    header.channels[i].name[255] = '\0';
    header.channels[i].pixel_type = TINYEXR_PIXELTYPE_FLOAT;
    header.channels[i].x_sampling = 1;
    header.channels[i].y_sampling = 1;
    header.channels[i].p_linear = 0;

    header.pixel_types[i] = TINYEXR_PIXELTYPE_FLOAT;
    header.requested_pixel_types[i] = TINYEXR_PIXELTYPE_HALF;  // Save as half for smaller file
  }

  // Set image dimensions
  header.data_window.min_x = 0;
  header.data_window.min_y = 0;
  header.data_window.max_x = width - 1;
  header.data_window.max_y = height - 1;
  header.display_window = header.data_window;
  header.pixel_aspect_ratio = 1.0f;
  header.screen_window_center[0] = 0.0f;
  header.screen_window_center[1] = 0.0f;
  header.screen_window_width = 1.0f;
  header.line_order = 0;
  header.compression_type = TINYEXR_COMPRESSIONTYPE_ZIP;

  // Generate spectral data
  std::vector<std::vector<float>> channel_data(kNumWavelengths);
  for (int i = 0; i < kNumWavelengths; i++) {
    channel_data[i].resize(width * height);
  }

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int idx = y * width + x;

      // Vary temperature across the image for visual interest
      float local_temp = temperature_K * (0.5f + static_cast<float>(x) / width);

      // Intensity falloff from center
      float cx = (x - width / 2.0f) / (width / 2.0f);
      float cy = (y - height / 2.0f) / (height / 2.0f);
      float intensity = 1.0f - sqrtf(cx * cx + cy * cy) * 0.5f;
      if (intensity < 0.0f) intensity = 0.0f;

      for (int w = 0; w < kNumWavelengths; w++) {
        if (spectrum_type == TINYEXR_SPECTRUM_REFLECTIVE) {
          // For reflective, create a gradient based on wavelength
          float normalized_w = static_cast<float>(w) / kNumWavelengths;
          channel_data[w][idx] = intensity * (0.2f + 0.8f * normalized_w);
        } else {
          // For emissive, use blackbody spectrum
          channel_data[w][idx] = BlackbodySpectrum(kVisibleWavelengths[w], local_temp) * intensity;
        }
      }
    }
  }

  // Set up image pointers in sorted channel order
  EXRImage image;
  InitEXRImage(&image);
  image.width = width;
  image.height = height;
  image.num_channels = kNumWavelengths;

  std::vector<float*> image_ptrs(kNumWavelengths);
  for (int i = 0; i < kNumWavelengths; i++) {
    // Map from sorted order back to wavelength order
    image_ptrs[i] = channel_data[wavelength_order[i]].data();
  }
  image.images = reinterpret_cast<unsigned char**>(image_ptrs.data());

  // Save to file
  const char* err = nullptr;
  ret = SaveEXRImageToFile(&image, &header, filename, &err);

  // Cleanup - FreeEXRHeader frees channels, pixel_types, requested_pixel_types, and custom_attributes
  FreeEXRHeader(&header);

  if (ret != TINYEXR_SUCCESS) {
    printf("Failed to save EXR: %s\n", err ? err : "unknown error");
    if (err) FreeEXRErrorMessage(err);
    return false;
  }

  printf("Successfully saved spectral EXR: %s\n", filename);
  return true;
}

// Load and analyze a spectral EXR image
bool LoadSpectralImage(const char* filename) {
  printf("\nLoading spectral EXR: %s\n", filename);

  // Check if it's a spectral EXR
  int ret = IsSpectralEXR(filename);
  if (ret != TINYEXR_SUCCESS) {
    printf("File is not a spectral EXR (no spectralLayoutVersion attribute)\n");
    return false;
  }
  printf("Confirmed: File is a spectral EXR\n");

  // Parse version
  EXRVersion version;
  ret = ParseEXRVersionFromFile(&version, filename);
  if (ret != TINYEXR_SUCCESS) {
    printf("Failed to parse EXR version\n");
    return false;
  }

  // Parse header
  EXRHeader header;
  InitEXRHeader(&header);
  const char* err = nullptr;

  ret = ParseEXRHeaderFromFile(&header, &version, filename, &err);
  if (ret != TINYEXR_SUCCESS) {
    printf("Failed to parse header: %s\n", err ? err : "unknown error");
    if (err) FreeEXRErrorMessage(err);
    return false;
  }

  // Get spectrum type
  int spectrum_type = EXRGetSpectrumType(&header);
  const char* type_str = "unknown";
  switch (spectrum_type) {
    case TINYEXR_SPECTRUM_REFLECTIVE: type_str = "reflective"; break;
    case TINYEXR_SPECTRUM_EMISSIVE: type_str = "emissive"; break;
    case TINYEXR_SPECTRUM_POLARISED: type_str = "polarised"; break;
  }
  printf("Spectrum type: %s\n", type_str);

  // Get units
  const char* units = EXRGetSpectralUnits(&header);
  if (units) {
    printf("Spectral units: %s\n", units);
  }

  // Get wavelengths
  float wavelengths[256];
  int num_wavelengths = EXRGetWavelengths(&header, wavelengths, 256);
  printf("Number of wavelengths: %d\n", num_wavelengths);

  if (num_wavelengths > 0) {
    printf("Wavelength range: %.1f nm - %.1f nm\n",
           wavelengths[0], wavelengths[num_wavelengths - 1]);

    // Print all wavelengths
    printf("Wavelengths: ");
    for (int i = 0; i < num_wavelengths; i++) {
      printf("%.0f", wavelengths[i]);
      if (i < num_wavelengths - 1) printf(", ");
      if ((i + 1) % 10 == 0 && i < num_wavelengths - 1) printf("\n            ");
    }
    printf(" nm\n");
  }

  // Print channel info
  printf("\nChannels (%d total):\n", header.num_channels);
  for (int i = 0; i < header.num_channels && i < 10; i++) {
    float wl = EXRParseSpectralChannelWavelength(header.channels[i].name);
    int stokes = EXRGetStokesComponent(header.channels[i].name);

    printf("  [%d] %s", i, header.channels[i].name);
    if (wl > 0) printf(" (wavelength=%.1fnm", wl);
    if (stokes >= 0) printf(", stokes=S%d", stokes);
    if (wl > 0) printf(")");
    printf("\n");
  }
  if (header.num_channels > 10) {
    printf("  ... and %d more channels\n", header.num_channels - 10);
  }

  // Load the image data
  EXRImage image;
  InitEXRImage(&image);

  // Request float output for all channels
  for (int i = 0; i < header.num_channels; i++) {
    header.requested_pixel_types[i] = TINYEXR_PIXELTYPE_FLOAT;
  }

  ret = LoadEXRImageFromFile(&image, &header, filename, &err);
  if (ret != TINYEXR_SUCCESS) {
    printf("Failed to load image: %s\n", err ? err : "unknown error");
    if (err) FreeEXRErrorMessage(err);
    FreeEXRHeader(&header);
    return false;
  }

  printf("\nImage dimensions: %d x %d\n", image.width, image.height);

  // Sample some pixel values
  printf("\nSample values at center pixel (%d, %d):\n",
         image.width / 2, image.height / 2);

  int center_idx = (image.height / 2) * image.width + (image.width / 2);
  for (int i = 0; i < header.num_channels && i < 5; i++) {
    float* channel = reinterpret_cast<float*>(image.images[i]);
    printf("  %s: %g\n", header.channels[i].name, channel[center_idx]);
  }
  if (header.num_channels > 5) {
    printf("  ... (%d more channels)\n", header.num_channels - 5);
  }

  // Cleanup
  FreeEXRImage(&image);
  FreeEXRHeader(&header);

  printf("\nSuccessfully loaded and analyzed spectral EXR\n");
  return true;
}

void PrintUsage(const char* prog) {
  printf("Spectral EXR Example - TinyEXR V1 API\n");
  printf("Based on JCGT 2021 spectral EXR format\n\n");
  printf("Usage:\n");
  printf("  %s write <output.exr> [emissive|reflective] [width] [height] [temperature]\n", prog);
  printf("    Create a test spectral EXR file\n");
  printf("    - spectrum type: 'emissive' (default) or 'reflective'\n");
  printf("    - width: image width (default: 256)\n");
  printf("    - height: image height (default: 256)\n");
  printf("    - temperature: blackbody temperature in K (default: 5500, only for emissive)\n\n");
  printf("  %s read <input.exr>\n", prog);
  printf("    Load and analyze a spectral EXR file\n\n");
  printf("  %s roundtrip\n", prog);
  printf("    Create test files, then read them back (demonstration)\n");
}

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage(argv[0]);
    return 1;
  }

  const char* command = argv[1];

  if (strcmp(command, "write") == 0) {
    if (argc < 3) {
      printf("Error: output filename required\n");
      PrintUsage(argv[0]);
      return 1;
    }

    const char* output = argv[2];
    int spectrum_type = TINYEXR_SPECTRUM_EMISSIVE;
    int width = 256;
    int height = 256;
    float temperature = 5500.0f;

    if (argc > 3) {
      if (strcmp(argv[3], "reflective") == 0) {
        spectrum_type = TINYEXR_SPECTRUM_REFLECTIVE;
      }
    }
    if (argc > 4) width = atoi(argv[4]);
    if (argc > 5) height = atoi(argv[5]);
    if (argc > 6) temperature = static_cast<float>(atof(argv[6]));

    if (!CreateSpectralImage(output, width, height, spectrum_type, temperature)) {
      return 1;
    }

  } else if (strcmp(command, "read") == 0) {
    if (argc < 3) {
      printf("Error: input filename required\n");
      PrintUsage(argv[0]);
      return 1;
    }

    if (!LoadSpectralImage(argv[2])) {
      return 1;
    }

  } else if (strcmp(command, "roundtrip") == 0) {
    printf("=== Spectral EXR Roundtrip Test ===\n\n");

    // Create and read emissive spectrum
    printf("--- Test 1: Emissive Spectrum (blackbody at 6500K) ---\n");
    if (!CreateSpectralImage("test_emissive.exr", 128, 128,
                             TINYEXR_SPECTRUM_EMISSIVE, 6500.0f)) {
      return 1;
    }
    if (!LoadSpectralImage("test_emissive.exr")) {
      return 1;
    }

    printf("\n--- Test 2: Reflective Spectrum ---\n");
    if (!CreateSpectralImage("test_reflective.exr", 128, 128,
                             TINYEXR_SPECTRUM_REFLECTIVE, 0.0f)) {
      return 1;
    }
    if (!LoadSpectralImage("test_reflective.exr")) {
      return 1;
    }

    printf("\n=== All tests passed! ===\n");

  } else {
    printf("Unknown command: %s\n", command);
    PrintUsage(argv[0]);
    return 1;
  }

  return 0;
}
