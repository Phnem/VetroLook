# VetroLook 2.4.0 verification

- Release executable reports FileVersion and ProductVersion 2.4.0.
- First-run native language chooser: English and Russian selections persist; repeated startup skips the question; missing/invalid preferences trigger it.
- Close, Alt+F4 and Escape do not dismiss an unanswered question.
- Nine first-run UI checks passed using isolated registry keys. The real user's language preference was unchanged by these tests.
- Twenty native Smart Gallery worker checks passed: staged visibility, completion, application persistence, manual correction priority, cached restart, file changes while queued and model replacement.
- Four gallery UI checks passed: popup to toast, completion expansion, Later dismissal and application fade.
- MSI administrative extraction and portable ZIP inspection matched all 66 application payload files to the tested build, including model, policy, authorization marker and model card.
- The clean uninstaller's embedded script matched its source. It was inspected by CAB extraction and was not executed.
- Release asset SHA-256 checksums are published in SHA256SUMS.txt.

The original model validation limitation is documented in models/MODEL_CARD.md. These interface and packaging checks do not change the scientific model gate.

To run the first-run UI checks from the repository, install pywinauto and Pillow into a Python environment, then run:

```powershell
python tests/language-onboarding-ui.py --exe dist/VetroLook.exe --output language-ui-results
```

The output directory must be new. The test uses only HKCU\Software\VetroLook\OnboardingTests\... and removes the keys it creates.
