# VetroLook Smart Gallery model

TinyNeXt-T six-class classifier, exported to ONNX and evaluated through Windows ML.
Classes: photo, screenshot, document, icon_ui_asset, artwork, other.
Inference runs locally; no gallery images are uploaded. Original files are not changed.

Evaluation used 6,652 eligible reviewed samples (mixed human and publisher-accepted vision review); 19 uncertain samples were excluded. Photo recall: 97.4868%. Photo visibility recall under the shipped filtering policy: 99.9521%. Real-photo false-hidden rate: 2 / 4,178 = 0.04787%. One-sided 95% upper bound: 0.15061%.

The original target upper bound was 0.1%, so the original scientific quality gate remains **failed**. This exact model/policy is distributed with explicit publisher risk acceptance. The release does not claim the original gate passed. Uncertain and unreadable images remain visible. User Show/Hide corrections override automatic filtering, and the first batch requires explicit in-app application.

Model SHA-256: db197e99c866970811f7c4da597b31d32345f92ce1d45252d85f1fb3558bdfa5
Policy SHA-256: 5dbd2f1410b8c8645a5bd49f86933607a122121b1afbe6447235dc40edf7de81
Evaluation SHA-256: 5a3084a564dd1dbb58564812c3b44e95f003d8f3e6152b919628692ef75ab678

The model was trained for this project. No training dataset, personal image paths, review journals or gallery cache are included in this release.
