// HD texture packs (1.0's S26): the community's packs for the emulators (Azahar's and Citra's layout and hashes: a folder
// of tex1_<w>x<h>_<hash>_<format>.png files, with or without a pack.json) drawn in place of the game's own textures.
//
// The pack is read in place from the player's folder (the app passes it, MH4N_TEXTURE_PACKS; otherwise <--user>/load/
// textures/, Azahar's own place): each folder under it with texture files is one pack (load/textures/<title id>/ as Azahar
// has them, or the pack's own folder), read by Azahar's rules (hdpack/pack.h, ScanPack: the file names, pack.json's options
// and its "textures" map). A texture's custom hash is the one Azahar names its files by (gpu/texture/hash.h: the old hash
// of the decoded texture, or the new one of its bytes when the pack's pack.json says use_new_hash), taken once for each
// content the texture cache sees, and a texture the pack has gets an image of the pack file's size with its pixels.
//
// Only PNG colour maps are drawn (most packs are PNG; DDS and KTX files, normal maps and mip files are counted in the log
// and left out). A PNG is read and decoded when its texture is first drawn (the frame waits for it: a pack loads as the
// areas are visited) and its pixels go once the image holds them. The GPU memory the HD images take is the player's
// call: the log says it as it grows ("[mem] HD textures"), and the images count in the renderer's texture budget like
// the game's own, which lets go of the least used past it.
//
// Orientation: the texture cache's pixels (DecodeToRgba8) are the image upright, its top row first, as Azahar's dumps
// write their PNGs; flip_png_files (true without a pack.json) means the files are so, false that their rows are bottom-up.
// The first texture of a decoded format checks that against the hash's own decode (rows bottom-up) and logs it.
//
//   MH4N_TEXTURE_PACKS=<dir>   the folder of the packs (the app: the game folder's textures/)
//   MH4N_HD_TEXTURES=0         no pack is read
//   MH4N_HD_MAX_SIZE=<px>      (MODIFIED, 2026-11-22) a pack texture larger than <px> on its longer side is reduced (halved, again and again,
//                              by averaging) before it is uploaded: for GPUs with little memory. 0 or none: no limit
//   MH4N_HD_DUMP=<dir>         (debug) each texture the game draws, once, as <dir>/tex1_<w>x<h>_<old hash>_<format>.png,
//                              upright: a pack of the game's own textures, for tests
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gpu/texture/formats.h"
#include "hdpack/archive.h"
#include "mh4n/types.h"

namespace mh4n::gpu::native {

class HdTextures {
public:
    HdTextures() = default;
    HdTextures(const HdTextures&) = delete;
    HdTextures& operator=(const HdTextures&) = delete;
    ~HdTextures();

    struct Replacement {
        u32 file = 0;  // index into files_
        u32 width = 0, height = 0;
    };

    // Reads the packs under `root` (or none: an empty root, a folder that isn't there).
    // The packs are scanned on a thread of their own (a big pack on a slow disk takes minutes: the game must not wait
    // for it); until it is done no texture is replaced, and TakeJustReady says once that it is, so the textures drawn
    // meanwhile can be made again from the pack.
    void Load(const std::string& root);
    bool active() const {
        return (ready_.load(std::memory_order_acquire) && (!by_hash_[0].empty() || !by_hash_[1].empty())) || !dump_dir_.empty();
    }
    bool TakeJustReady() { return ready_.load(std::memory_order_acquire) && just_ready_.exchange(false) && (!by_hash_[0].empty() || !by_hash_[1].empty()); }

    // The pack's texture for a texture whose content hash (the texture cache's) is `content` and whose memory is `data`;
    // nullptr when no pack has it. Answered once for each content.
    const Replacement* Find(u64 content, const u8* data, std::size_t size, u32 width, u32 height, texture::Format format);

    // MODIFIED for the Windows port: converted packs (*.mh4hdpk, made by mh4n_hdpack: BC7 blocks with mips, rows as the
    // renderer's images hold them) are read too. Set before Load: false when the device cannot sample block-compressed images.
    void SetCompressedAllowed(bool allowed) { compressed_allowed_ = allowed; }
    // A texture of such an archive: its bytes (Take) are the blob of the levels, level 0 first, not RGBA8.
    bool Compressed(const Replacement& r) const { return files_[r.file].archive >= 0; }
    u32 Mips(const Replacement& r) const { return files_[r.file].mips; }
    hdpack::PixelFormat PackFormat(const Replacement& r) const { return static_cast<hdpack::PixelFormat>(files_[r.file].pfmt); }
    // What the texture takes on the GPU (and what the upload counts): the blob's size, or width * height * 4
    u64 UploadBytes(const Replacement& r) const {
        const File& f = files_[r.file];
        return f.archive >= 0 ? f.size : u64{r.width} * r.height * 4;
    }

    // The size a texture of the pack is drawn at: the file's, or halved as many times as it takes to fit MH4N_HD_MAX_SIZE.
    void CappedSize(u32 width, u32 height, u32& out_width, u32& out_height) const;
    // Its pixels, RGBA8 upright as the texture cache's, read and decoded now. False when the file can't be read.
    bool Pixels(const Replacement& r, std::vector<u8>& rgba);

    // The same, without making the game thread wait (2026-11-20): the file is read and decoded by worker threads.
    // Status asks for the file when nobody has yet; Take hands over the decoded pixels once it is Ready.
    enum class State { Pending, Ready, Failed };
    State Status(const Replacement& r);
    bool Take(const Replacement& r, std::vector<u8>& rgba);
    // Once a frame: decoded pixels nobody asked for in a while are let go, and the retry of a failed upload comes due.
    void NextFrame();
    // A texture of the pack that could not be uploaded (a transient lack of GPU memory, say): tried once more later.
    // False when it has been tried so already (then Refuse it).
    bool DelayUploadRetry(const Replacement& r);
    // The most the game thread uploads from the pack in a frame (and the most one texture may be)
    static constexpr u64 kUploadPerFrame = 24u << 20;
    // A file that could not be read or uploaded: its textures are the game's own from now on (logged once)
    void Refuse(const Replacement& r);

    // A texture drawn for the first time (MH4N_HD_DUMP): written as a PNG of the pack's names, with its decoded pixels.
    void Dump(u64 content, const u8* data, std::size_t size, u32 width, u32 height, texture::Format format,
              const std::vector<u8>& rgba);

    // The images made from the pack: for the log's [mem] line
    void Made(u64 bytes) { ++images_, image_bytes_ += bytes; }
    void Gone(u64 bytes) { --images_, image_bytes_ -= bytes; }
    u64 images() const { return images_; }
    u64 image_bytes() const { return image_bytes_; }
    u64 replaced() const { return replaced_; }

private:
    u32 max_size_ = 0;  // MH4N_HD_MAX_SIZE (0: none)
    bool cap_logged_ = false;
    struct File {
        std::string path;
        bool flip = false;  // the rows are turned over as they are read (the pack's flip_png_files false)
        bool refused = false;  // (Refuse)
        u64 retry_at = 0;      // (DelayUploadRetry) the frame from which it may be asked for again
        u32 width = 0, height = 0;
        int archive = -1;  // index into archives_ (a converted pack), else a PNG file
        u64 offset = 0, size = 0;
        u8 mips = 1, pfmt = 0;
    };
    struct Answer {
        bool found = false;
        Replacement r;
    };
    void Scan(const std::string& root);
    void CheckOrientation(const u8* data, std::size_t size, u32 width, u32 height, texture::Format format,
                          const std::vector<u8>* rgba);

    std::vector<File> files_;
    std::vector<hdpack::ArchiveReader> archives_;
    bool compressed_allowed_ = true;
    std::unordered_map<u64, u32> by_hash_[2];  // old hash, new hash -> file
    std::unordered_map<u64, Answer> answers_;  // by content hash (and size, format)
    std::string dump_dir_;
    std::unordered_map<u64, bool> dumped_;
    std::thread loader_;
    // The decoding workers and what they hold (guarded by mutex_)
    struct Slot {
        State state = State::Pending;
        std::vector<u8> pixels;
        u64 bytes = 0;      // what the decoded pixels will take (counted from the request)
        u64 last_used = 0;  // the last frame somebody asked about it
    };
    void Worker();
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<u32> queue_;
    std::unordered_map<u32, Slot> slots_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stop_{false};
    u64 reserved_bytes_ = 0;  // the pixels asked for and not yet taken (mutex_)
    u64 frame_ = 0;
    std::atomic<bool> ready_{false};
    std::atomic<bool> just_ready_{false};
    bool oriented_ = false;
    u64 images_ = 0, image_bytes_ = 0, replaced_ = 0, lookups_ = 0;
};

}  // namespace mh4n::gpu::native
