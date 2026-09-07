# WebP

WebP is a still image format, described in the [WebP Container
Specification](https://developers.google.com/speed/webp/docs/riff_container).
It is a container for [VP8 Lossy](https://www.ietf.org/rfc/rfc6386.txt) or [VP8
Lossless](https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification).

The ["Making Software - Image
Compression"](https://www.makingsoftware.com/chapters/image-compression) book
chapter also has a good overview of a number of image formats, including WebP.

There are two WebP variants: lossy (based on VP8) and lossless (based on what's
essentially an unrelated design, other than "WebP" branding). Wuffs' VP8
implementation is in a separate package, as the WebM moving image format also
uses VP8. There's no Wuffs WebM package yet but there might be in the future.
