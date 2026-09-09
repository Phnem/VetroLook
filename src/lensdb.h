#pragma once
// Vetro Look, GPL-3.0-or-later.
// Lens identification and correction profiles, from the Lensfun database.
//
// The Lensfun *library* is not linked: it requires GLib, which no other part
// of this application needs and which cannot be built here without pulling in
// a second package manager (see LICENSE_AUDIT.md). What is used is the
// Lensfun *database* — 1500 calibrated lenses and 1000 camera bodies,
// CC-BY-SA 3.0, shipped beside the executable — read here directly, with
// Lensfun's own published correction models.
#include <cstdint>
#include <string>
#include <vector>

// What is known about the lens a photograph was taken with, and which
// corrections its calibration can actually support. Absent calibration is
// reported as absent: an "Auto" control must never look enabled for a lens
// nobody has measured.
struct LensCorrectionInfo{
 bool ready=false;                    // the database answered (matched or not)
 bool matched=false;                  // a lens record was found

 std::wstring cameraMatch;            // as Lensfun spells it
 std::wstring lensMatch;
 std::wstring mount;
 double cropFactor=0;                 // of the matched camera, else of the lens record

 bool distortion=false;               // calibration present for this focal length
 bool vignetting=false;               // ... and this aperture
 bool tca=false;
 bool geometry=false;                 // lens type known, so a projection change is possible

 // Coefficients already interpolated for the frame's focal length and
 // aperture. Models are Lensfun's: "ptlens" | "poly3" | "poly5" for
 // distortion, "pa" for vignetting, "poly3" for transverse chromatic
 // aberration.
 std::wstring distortionModel;double distortionA=0,distortionB=0,distortionC=0;
 std::wstring vignettingModel;double vignetteK1=0,vignetteK2=0,vignetteK3=0;
 std::wstring tcaModel;double tcaRed=1,tcaBlue=1;

 std::wstring note;                   // why nothing matched, when nothing did
};

// Starts loading the database on a background thread. Idempotent; the first
// call is what actually reads the files. Never blocks the caller.
void LensDbStart();
bool LensDbReady();
size_t LensDbLensCount();
size_t LensDbCameraCount();

// Looks a frame up. Returns ready=false while the database is still loading,
// so a caller can ask again rather than block. `focal` in mm, `aperture` as an
// f-number; zero for either means "unknown", which limits which calibrations
// can be reported as available.
LensCorrectionInfo LensLookup(const std::wstring& cameraMake,const std::wstring& cameraModel,
                              const std::wstring& lens,double focal,double aperture);
