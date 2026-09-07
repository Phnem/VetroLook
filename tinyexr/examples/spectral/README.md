# Spectral EXR Example

This example demonstrates reading and writing spectral EXR files using the TinyEXR V1 API.

## Spectral EXR Format

Based on the JCGT 2021 paper and spectral-exr reference implementation:
- Paper: https://jcgt.org/published/0010/03/01/
- Reference: https://github.com/afichet/spectral-exr

### Spectrum Types

| Type | Channel Format | Description |
|------|---------------|-------------|
| Emissive | `S{stokes}.{wavelength}nm` | Radiance/irradiance spectra |
| Reflective | `T.{wavelength}nm` | Transmittance/reflectance spectra |
| Polarised | `S0-S3.{wavelength}nm` | Stokes vector spectra |

### Wavelength Format

Wavelengths use European decimal convention (comma as separator):
- `S0.550,000000nm` for 550nm
- `T.400,500000nm` for 400.5nm

### Required Attributes

- `spectralLayoutVersion`: Always "1.0"
- `emissiveUnits` or `ROOT/units`: Physical units string

## Building

```bash
make
```

## Usage

### Write a spectral EXR

```bash
# Create emissive spectrum (blackbody at 6500K)
./spectral_example write output.exr emissive 256 256 6500

# Create reflective spectrum
./spectral_example write output.exr reflective 256 256
```

### Read a spectral EXR

```bash
./spectral_example read input.exr
```

### Run roundtrip test

```bash
make test
```

### Create sample files

```bash
make samples
```

## API Functions Used

### Detection
- `IsSpectralEXR()` - Check if file is spectral
- `EXRGetSpectrumType()` - Get spectrum type (emissive/reflective/polarised)
- `EXRIsSpectralChannel()` - Check if channel name follows spectral format

### Channel Naming
- `EXRSpectralChannelName()` - Create emissive channel name
- `EXRReflectiveChannelName()` - Create reflective channel name
- `EXRParseSpectralChannelWavelength()` - Extract wavelength from name
- `EXRGetStokesComponent()` - Get Stokes component (0-3)

### Metadata
- `EXRSetSpectralAttributes()` - Set spectralLayoutVersion and units
- `EXRGetSpectralUnits()` - Get units attribute
- `EXRGetWavelengths()` - Extract sorted wavelengths from header

## Example Output

```
Loading spectral EXR: test_emissive.exr
Confirmed: File is a spectral EXR
Spectrum type: emissive
Spectral units: W.m^-2.sr^-1.nm^-1
Number of wavelengths: 31
Wavelength range: 400.0 nm - 700.0 nm
Wavelengths: 400, 410, 420, 430, 440, 450, 460, 470, 480, 490,
            500, 510, 520, 530, 540, 550, 560, 570, 580, 590,
            600, 610, 620, 630, 640, 650, 660, 670, 680, 690,
            700 nm

Channels (31 total):
  [0] S0.400,000000nm (wavelength=400.0nm, stokes=S0)
  [1] S0.410,000000nm (wavelength=410.0nm, stokes=S0)
  ...
```
