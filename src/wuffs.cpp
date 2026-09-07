#include "image.h"
#define WUFFS_IMPLEMENTATION
#define WUFFS_CONFIG__STATIC_FUNCTIONS
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__AUX__BASE
#define WUFFS_CONFIG__MODULE__AUX__IMAGE
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__GIF
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__ZLIB
#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ENABLE_ALLOWLIST
#define WUFFS_CONFIG__DST_PIXEL_FORMAT__ALLOW_BGRA_PREMUL
#include "../../wuffs/release/c/wuffs-v0.4.c"
// Decode directly into the final CPU buffer; reject large dimensions before allocation.
struct DirectPixels : wuffs_aux::DecodeImageCallbacks {
 std::shared_ptr<Image> image=std::make_shared<Image>();
 AllocPixbufResult AllocPixbuf(const wuffs_base__image_config& config,bool) override {
  image->w=config.pixcfg.width();image->h=config.pixcfg.height();
  if(!image->w||!image->h||uint64_t(image->w)*image->h>100000000)return AllocPixbufResult("Image exceeds pixel budget");
  image->pixels.resize(size_t(image->w)*image->h*4);
  wuffs_base__pixel_buffer pixels;
  auto status=pixels.set_from_slice(&config.pixcfg,wuffs_base__make_slice_u8(image->pixels.data(),image->pixels.size()));
  if(!status.is_ok())return AllocPixbufResult(status.message());
  return AllocPixbufResult(wuffs_aux::MemOwner(nullptr,&free),pixels);
 }
};
std::shared_ptr<Image> DecodeWuffs(const std::vector<uint8_t>& data) {
 DirectPixels cb;
 wuffs_aux::sync_io::MemoryInput input(data.data(),data.size());
 auto result=wuffs_aux::DecodeImage(cb,input);
 if(!result.error_message.empty())return {};
 cb.image->codec=L"Wuffs";return cb.image;
}
