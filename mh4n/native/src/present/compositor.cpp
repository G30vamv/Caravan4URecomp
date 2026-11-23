// The compositor (N2b-05). Not derived from Azahar: the shaders and the drawing are ours (Azahar's renderers
// draw the screens with their own pipelines; the layouts are N2b-04's).
#include "present/compositor.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "gpu/backend/targets/upload.h"
#include "mh4n/log.h"
#include "present/fsr/fsr_source.inc"  // MODIFIED: kFfxA, kFfxFsr1 (AMD FidelityFX FSR 1, MIT)
#include "present/shader_compile.h"

namespace mh4n::present {

namespace rhi = ::mh4n::gpu::rhi;

namespace {

constexpr const char* kLog = "Present";

// One quad of a screen: four vertices of a triangle strip. `pos` is in the target's NDC; `uv` in the source
// image's normalized coordinates; `bounds` the part of the image the sample is clamped to (x0, y0, x1, y1:
// the centres of the region's edge texels, so that linear filtering never reads outside the region);
// `alpha` the layer's opacity.
struct Vertex {
    float pos[2];
    float uv[2];
    float bounds[4];
    float alpha;
};
static_assert(sizeof(Vertex) == 36);

const char* const kVertexShader = R"(#version 450
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_bounds;
layout(location = 3) in float in_alpha;
layout(location = 0) out vec2 v_uv;
layout(location = 1) flat out vec4 v_bounds;
layout(location = 2) flat out float v_alpha;
void main() {
    gl_Position = vec4(in_pos, 0.0, 1.0);
    v_uv = in_uv;
    v_bounds = in_bounds;
    v_alpha = in_alpha;
}
)";

// The screen is opaque whatever its alpha channel says; the layer's opacity is the blend's source alpha.
const char* const kFragmentShader = R"(#version 450
layout(set = 0, binding = 0) uniform texture2D tex;
layout(set = 0, binding = 1) uniform sampler smp;
layout(location = 0) in vec2 v_uv;
layout(location = 1) flat in vec4 v_bounds;
layout(location = 2) flat in float v_alpha;
layout(location = 0) out vec4 out_color;
void main() {
    vec2 uv = clamp(v_uv, v_bounds.xy, v_bounds.zw);
    out_color = vec4(texture(sampler2D(tex, smp), uv).rgb, v_alpha);
}
)";

// MODIFIED (2026-11-22): FSR 1.0 in two passes. EASU draws a screen into an image of the size it is shown at (the position it samples
// comes from the interpolated uv, so a rotated screen works too); RCAS then reads that image 1:1 and sharpens it into the target.
const char* const kFsrHead = R"(#version 450
#define A_GPU 1
#define A_GLSL 1
layout(set = 0, binding = 0) uniform texture2D tex;
layout(set = 0, binding = 1) uniform sampler smp;
layout(location = 0) in vec2 v_uv;
layout(location = 1) flat in vec4 v_bounds;
layout(location = 2) flat in float v_alpha;
layout(location = 0) out vec4 out_color;
)";

const char* const kEasuHead = R"(
#define FSR_EASU_F 1
vec2 g_easu_pp;
uvec4 g_con1;
uvec4 g_con2;
uvec4 g_con3;
)";

const char* const kEasuTail = R"(
vec4 FsrEasuRF(vec2 p) { return textureGather(sampler2D(tex, smp), p, 0); }
vec4 FsrEasuGF(vec2 p) { return textureGather(sampler2D(tex, smp), p, 1); }
vec4 FsrEasuBF(vec2 p) { return textureGather(sampler2D(tex, smp), p, 2); }
void main() {
    vec2 size = vec2(textureSize(sampler2D(tex, smp), 0));
    vec2 inv = 1.0 / size;
    g_con1 = floatBitsToUint(vec4(inv.x, inv.y, inv.x, -inv.y));
    g_con2 = floatBitsToUint(vec4(-inv.x, 2.0 * inv.y, inv.x, 2.0 * inv.y));
    g_con3 = floatBitsToUint(vec4(0.0, 4.0 * inv.y, 0.0, 0.0));
    g_easu_pp = clamp(v_uv, v_bounds.xy, v_bounds.zw) * size - 0.5;
    vec3 c;
    FsrEasuF(c, uvec2(0u), uvec4(0u), g_con1, g_con2, g_con3);
    out_color = vec4(c, 1.0);
}
)";

const char* const kRcasHead = R"(
#define FSR_RCAS_F 1
)";

const char* const kRcasTail = R"(
vec4 FsrRcasLoadF(ivec2 p) {
    return texelFetch(sampler2D(tex, smp), clamp(p, ivec2(0), textureSize(sampler2D(tex, smp), 0) - 1), 0);
}
void FsrRcasInputF(inout float r, inout float g, inout float b) {}
void main() {
    ivec2 base = ivec2(floor(v_uv * vec2(textureSize(sampler2D(tex, smp), 0))));
    uvec4 rcon;
    FsrRcasCon(rcon, 0.25);
    float r, g, b;
    FsrRcasF(r, g, b, uvec2(base), rcon);
    out_color = vec4(r, g, b, v_alpha);
}
)";

// The fragment shaders: the head, AMD's two headers (for EASU, with the one line that works the position out from the output pixel
// replaced by the position the shader computes), and the tail.
// MH4N_FSR_VARIANT (a debugging aid for drivers that fail on the FSR shaders): 1 both passes plain copies, 2 EASU real and RCAS a plain
// copy, 3 EASU a plain copy and RCAS real
int FsrVariant() {
    const char* text = std::getenv("MH4N_FSR_VARIANT");
    return text != nullptr ? std::atoi(text) : 0;
}

std::string EasuFragmentSource() {
    if (FsrVariant() == 1 || FsrVariant() == 3) {
        return std::string(kFsrHead) + "void main() { out_color = vec4(texture(sampler2D(tex, smp), clamp(v_uv, v_bounds.xy, v_bounds.zw)).rgb, 1.0); }\n";
    }
    std::string fsr1 = kFfxFsr1;
    const std::string line = "AF2 pp=AF2(ip)*AF2_AU2(con0.xy)+AF2_AU2(con0.zw);";
    const std::size_t at = fsr1.find(line);
    if (at != std::string::npos) {
        fsr1.replace(at, line.size(), "AF2 pp=g_easu_pp;");
    }
    return std::string(kFsrHead) + kEasuHead + kFfxA + "\n" + fsr1 + kEasuTail;
}

std::string RcasFragmentSource() {
    if (FsrVariant() == 1 || FsrVariant() == 2) {
        return std::string(kFsrHead) + "void main() { out_color = vec4(texelFetch(sampler2D(tex, smp), ivec2(floor(v_uv * vec2(textureSize(sampler2D(tex, smp), 0)))), 0).rgb, v_alpha); }\n";
    }
    return std::string(kFsrHead) + kRcasHead + kFfxA + "\n" + kFfxFsr1 + kRcasTail;
}

const rhi::BindingSetDesc kSetDesc{{{0, rhi::BindingType::SampledImage}, {1, rhi::BindingType::Sampler}}};

constexpr u64 kVertexRingSize = 256 * 1024;
constexpr std::size_t kMaxIdleTextures = 8;

} // namespace

struct ComposeResult::CpuTexture {
    std::shared_ptr<rhi::Image> image;
    u32 width = 0;
    u32 height = 0;
    rhi::TimelineValue busy_until = 0; // the value of the last submission that sampled it
    u64 taken_in = 0;                  // the Compose that is using it (State::serial)
};

struct Compositor::State {
    std::shared_ptr<rhi::Backend> backend;
    std::vector<u32> vertex_spirv;
    std::vector<u32> fragment_spirv;
    std::map<rhi::Format, std::shared_ptr<rhi::Pipeline>> pipelines;
    std::vector<u32> easu_spirv;  // MODIFIED: FSR's two passes, made on first use (about 110 KB of GLSL each)
    std::vector<u32> rcas_spirv;
    bool fsr_tried = false;
    std::map<rhi::Format, std::shared_ptr<rhi::Pipeline>> easu_pipelines;
    std::map<rhi::Format, std::shared_ptr<rhi::Pipeline>> rcas_pipelines;
    std::shared_ptr<rhi::Sampler> nearest;
    std::shared_ptr<rhi::Sampler> linear;
    std::unique_ptr<rhi::Ring> vertices;
    std::unique_ptr<rhi::BindingArena> arena;
    std::vector<std::unique_ptr<ComposeResult::CpuTexture>> textures;
    u64 serial = 0;
    u32 textures_created = 0;
    rhi::TimelineValue last_value = 0;
    bool warned_invalid_top = false;
    bool warned_invalid_bottom = false;

    // The pipeline for a target of `format`, made on first use; null when the format cannot be drawn to.
    rhi::Pipeline* PipelineFor(rhi::Format format) { return PipelineWith(format, 0); }

    // FSR 1.0's pipelines: null when FSR cannot be had (a shader did not compile: said once in the log).
    // A marker file lives while FSR's pipelines are being made: some drivers (AMD's amdvlk on an integrated GPU) crash inside
    // vkCreateGraphicsPipelines for these shaders, and a crash cannot be caught. If the marker of this GPU is still there on the next
    // start, FSR stays off (MH4N_FSR_FORCE=1 tries again).
    std::filesystem::path FsrMarker() const {
        std::error_code ec;
        return std::filesystem::temp_directory_path(ec) / "Caravan4URecomp_fsr_pipeline.marker";
    }
    std::string FsrGpu() const { return backend->Caps().device_name + " / " + backend->Caps().driver_name; }
    bool FsrCrashedBefore() const {
        const char* force = std::getenv("MH4N_FSR_FORCE");
        if (force != nullptr && force[0] == '1') {
            return false;
        }
        std::ifstream in(FsrMarker());
        std::string text;
        std::getline(in, text);
        return in && text == FsrGpu();
    }

    bool FsrReady() {
        if (!fsr_tried) {
            fsr_tried = true;
            std::string error;
            if (FsrCrashedBefore()) {
                MH4N_LOG(Error, kLog, "FSR stays off: the driver of {} crashed while making its pipelines last time (MH4N_FSR_FORCE=1 tries again)",
                         FsrGpu());
            } else if (!CompileGlsl(GlslStage::Fragment, EasuFragmentSource(), easu_spirv, error) ||
                !CompileGlsl(GlslStage::Fragment, RcasFragmentSource(), rcas_spirv, error)) {
                MH4N_LOG(Error, kLog, "an FSR shader does not compile, so FSR stays off: {}", error);
                easu_spirv.clear();
                rcas_spirv.clear();
            } else {
                MH4N_LOG(Info, kLog, "FSR 1.0 (EASU, then RCAS) is ready");
            }
        }
        return !easu_spirv.empty() && !rcas_spirv.empty();
    }
    rhi::Pipeline* EasuPipelineFor() { return FsrReady() ? PipelineWith(rhi::Format::Rgba8Unorm, 1) : nullptr; }
    rhi::Pipeline* RcasPipelineFor(rhi::Format format) { return FsrReady() ? PipelineWith(format, 2) : nullptr; }

    // kind: 0 the plain compositor, 1 FSR's EASU, 2 FSR's RCAS
    rhi::Pipeline* PipelineWith(rhi::Format format, int kind) {
        auto& cache = kind == 1 ? easu_pipelines : kind == 2 ? rcas_pipelines : pipelines;
        auto found = cache.find(format);
        if (found != cache.end()) {
            return found->second.get();
        }
        std::shared_ptr<rhi::Pipeline> made;
        if (backend->QueryFormat(format).attachment) {
            rhi::GraphicsPipelineDesc desc;
            desc.vertex.spirv = vertex_spirv;
            desc.fragment.spirv = kind == 1 ? easu_spirv : kind == 2 ? rcas_spirv : fragment_spirv;
            desc.vertex_layout.bindings = {{0, sizeof(Vertex), false}};
            desc.vertex_layout.attributes = {
                {0, 0, rhi::Format::Rg32Float, offsetof(Vertex, pos)},
                {1, 0, rhi::Format::Rg32Float, offsetof(Vertex, uv)},
                {2, 0, rhi::Format::Rgba32Float, offsetof(Vertex, bounds)},
                {3, 0, rhi::Format::R32Float, offsetof(Vertex, alpha)},
            };
            desc.topology = rhi::Topology::TriangleStrip;
            desc.color_format = format;
            // src * a + dst * (1 - a) on the colour; the target's alpha stays what the clear made it.
            desc.state.blend_enabled = true;
            desc.state.src_color = rhi::BlendFactor::SrcAlpha;
            desc.state.dst_color = rhi::BlendFactor::OneMinusSrcAlpha;
            desc.state.color_op = rhi::BlendOp::Add;
            desc.state.src_alpha = rhi::BlendFactor::Zero;
            desc.state.dst_alpha = rhi::BlendFactor::One;
            desc.state.alpha_op = rhi::BlendOp::Add;
            desc.sets = {kSetDesc};
            desc.name = kind == 1 ? "compositor (FSR EASU)" : kind == 2 ? "compositor (FSR RCAS)" : "compositor";
            if (kind != 0) {
                std::ofstream(FsrMarker(), std::ios::trunc) << FsrGpu() << "\n";
            }
            rhi::Result<std::shared_ptr<rhi::Pipeline>> pipeline = backend->CreateGraphicsPipeline(desc);
            if (kind != 0) {
                std::error_code ec;
                std::filesystem::remove(FsrMarker(), ec);
            }
            if (pipeline.ok()) {
                made = *pipeline;
            } else {
                MH4N_LOG(Error, kLog, "compositor pipeline for a target format: {}", pipeline.error());
            }
        }
        cache.emplace(format, made);
        return made.get();
    }

    // A texture of w x h for a CPU-pixel screen: an idle one of the pool, else a new one.
    ComposeResult::CpuTexture* AcquireTexture(u32 w, u32 h) {
        const rhi::TimelineValue completed = backend->CompletedValue();
        for (auto& texture : textures) {
            if (texture->width == w && texture->height == h && texture->taken_in != serial &&
                texture->busy_until <= completed) {
                texture->taken_in = serial;
                return texture.get();
            }
        }
        rhi::ImageDesc desc;
        desc.format = rhi::Format::Rgba8Unorm;
        desc.width = w;
        desc.height = h;
        desc.name = "compositor screen upload";
        rhi::Result<std::shared_ptr<rhi::Image>> image = backend->CreateImage(desc);
        if (!image.ok()) {
            MH4N_LOG(Error, kLog, "cannot make a {}x{} texture for CPU pixels: {}", w, h, image.error());
            return nullptr;
        }
        auto texture = std::make_unique<ComposeResult::CpuTexture>();
        texture->image = *image;
        texture->width = w;
        texture->height = h;
        texture->taken_in = serial;
        ++textures_created;
        textures.push_back(std::move(texture));
        return textures.back().get();
    }

    // Drops idle textures beyond the pool's size (oldest first); none that the running Compose uses.
    void TrimTextures() {
        const rhi::TimelineValue completed = backend->CompletedValue();
        std::size_t idle = 0;
        for (const auto& texture : textures) {
            idle += texture->taken_in != serial && texture->busy_until <= completed;
        }
        for (auto it = textures.begin(); it != textures.end() && idle > kMaxIdleTextures;) {
            if ((*it)->taken_in != serial && (*it)->busy_until <= completed) {
                it = textures.erase(it);
                --idle;
            } else {
                ++it;
            }
        }
    }
};

namespace {

// The part of a screen a draw shows, `crop` in fractions of the displayed screen (screen_layout::RectF), and
// the source image it samples.
struct SourceImage {
    const rhi::Image* image = nullptr;
    // The stored region, in image pixels.
    double x = 0, y = 0, width = 0, height = 0;
    bool rotated = false;
    bool mirrored = false;  // Rotated90Mirrored
    double scaled_width = 0;  // the displayed size in image pixels (scale * width, scale * height)
    double scaled_height = 0;
};

// A displayed position (fractions of the screen) as a normalized coordinate of the source image. Upright:
// image position = displayed position. Rotated90 (present.h): the pixel row y of the screen is the stored
// column scaled_height - 1 - y, and the pixel column x the stored row x, so the continuous mapping is
// (X, Y) -> (scaled_height - Y, X).
void SourceCoord(const SourceImage& src, double fx, double fy, float uv[2]) {
    const double X = fx * src.scaled_width;
    const double Y = fy * src.scaled_height;
    const double ix = src.rotated ? src.scaled_height - Y : X;
    const double iy = src.rotated ? (src.mirrored ? src.scaled_width - X : X) : Y;
    uv[0] = static_cast<float>((src.x + ix) / src.image->Width());
    uv[1] = static_cast<float>((src.y + iy) / src.image->Height());
}

// The rectangle `a` clipped to the target; width or height 0 when nothing is left.
rhi::Rect ClipToTarget(const screen_layout::Rect& r, u32 target_w, u32 target_h) {
    const u32 left = std::min(r.left, target_w);
    const u32 top = std::min(r.top, target_h);
    const u32 right = std::clamp(r.right, left, target_w);
    const u32 bottom = std::clamp(r.bottom, top, target_h);
    return rhi::Rect{static_cast<s32>(left), static_cast<s32>(top), right - left, bottom - top};
}

} // namespace

Compositor::Compositor(std::unique_ptr<State> state) : state_(std::move(state)) {}

Compositor::~Compositor() {
    if (state_ != nullptr && state_->backend != nullptr && state_->last_value != 0) {
        state_->backend->WaitIdle(state_->last_value);
    }
}

u32 Compositor::PipelineCount() const {
    u32 count = 0;
    for (const auto& entry : state_->pipelines) {
        count += entry.second != nullptr;
    }
    return count;
}

u32 Compositor::TexturesCreated() const {
    return state_->textures_created;
}

rhi::Result<std::unique_ptr<Compositor>> Compositor::Create(std::shared_ptr<rhi::Backend> backend) {
    if (backend == nullptr) {
        return rhi::Error{"Compositor::Create: no backend"};
    }
    auto state = std::make_unique<State>();
    state->backend = backend;
    std::string error;
    if (!CompileGlsl(GlslStage::Vertex, kVertexShader, state->vertex_spirv, error)) {
        return rhi::Error{"the compositor's vertex shader does not compile: " + error};
    }
    if (!CompileGlsl(GlslStage::Fragment, kFragmentShader, state->fragment_spirv, error)) {
        return rhi::Error{"the compositor's fragment shader does not compile: " + error};
    }
    for (const bool linear : {false, true}) {
        rhi::SamplerDesc desc;
        desc.min_filter = desc.mag_filter = linear ? rhi::Filter::Linear : rhi::Filter::Nearest;
        desc.mip_filter = rhi::Filter::Nearest;
        desc.max_lod = 0.0f;
        rhi::Result<std::shared_ptr<rhi::Sampler>> sampler = backend->CreateSampler(desc);
        if (!sampler.ok()) {
            return rhi::Error{"the compositor's sampler: " + sampler.error()};
        }
        (linear ? state->linear : state->nearest) = *sampler;
    }
    rhi::Result<std::unique_ptr<rhi::Ring>> ring =
        backend->CreateRing(kVertexRingSize, rhi::BufferUsage::Vertex, "compositor vertices");
    if (!ring.ok()) {
        return rhi::Error{"the compositor's vertex ring: " + ring.error()};
    }
    state->vertices = std::move(*ring);
    state->arena = std::make_unique<rhi::BindingArena>(backend);
    // The pipelines for the usual targets now: the offscreen image and the swapchain's usual format.
    for (const rhi::Format format : {rhi::Format::Rgba8Unorm, rhi::Format::Bgra8Unorm}) {
        if (backend->QueryFormat(format).attachment && state->PipelineFor(format) == nullptr) {
            return rhi::Error{"the compositor's pipeline could not be made (see the log)"};
        }
    }
    return std::unique_ptr<Compositor>(new Compositor(std::move(state)));
}

ComposeResult Compositor::Compose(rhi::Recorder& recorder, rhi::Image& target, const DisplayFrame& frame,
                                  const screen_layout::Layout& layout, const ComposeOptions& options) {
    State& s = *state_;
    ComposeResult result;
    rhi::Pipeline* pipeline = s.PipelineFor(target.GetFormat());
    if (pipeline == nullptr) {
        return result;
    }
    ++s.serial;
    s.TrimTextures();
    result.frame_images_ = std::make_shared<std::vector<std::shared_ptr<const void>>>();

    // ---- the screens that can be drawn, and the images they sample ----
    struct Source {
        const ScreenImage* screen = nullptr;
        SourceImage src;
        bool ok = false;
    };
    const auto prepare = [&](const std::optional<ScreenImage>& screen, bool& warned, const char* name) {
        Source out;
        if (!screen.has_value()) {
            return out;
        }
        if (!screen->Valid()) {
            if (!warned) {
                warned = true;
                MH4N_LOG(Warning, kLog, "the {} screen of a frame is not valid (sizes disagree): drawn black", name);
            }
            return out;
        }
        out.screen = &*screen;
        if (screen->IsBackendImage()) {
            const rhi::Image* image = static_cast<const rhi::Image*>(screen->image.get());
            const ImageRegion& region = screen->region;
            if (static_cast<u64>(region.x) + region.width > image->Width() ||
                static_cast<u64>(region.y) + region.height > image->Height()) {
                if (!warned) {
                    warned = true;
                    MH4N_LOG(Warning, kLog, "the {} screen's region lies outside its image: drawn black", name);
                }
                return out;
            }
            out.src.image = image;
            out.src.x = region.x;
            out.src.y = region.y;
            out.src.width = region.width;
            out.src.height = region.height;
            out.src.rotated = IsRotated(screen->orientation);
            out.src.mirrored = screen->orientation == Orientation::Rotated90Mirrored;
            result.frame_images_->push_back(screen->image);
            if (screen->timeline_value != 0) {
                bool merged = false;
                for (rhi::Wait& wait : result.waits) {
                    if (wait.stage == rhi::Stage::FragmentShader) {
                        wait.value = std::max(wait.value, screen->timeline_value);
                        merged = true;
                    }
                }
                if (!merged) {
                    result.waits.push_back(rhi::Wait{screen->timeline_value, rhi::Stage::FragmentShader});
                }
            }
        } else {
            ComposeResult::CpuTexture* texture = s.AcquireTexture(screen->width, screen->height);
            if (texture == nullptr) {
                return out;
            }
            rhi::BufferImageRegion copy;
            copy.image_rect = rhi::Rect{0, 0, screen->width, screen->height};
            rhi::Result<void> uploaded =
                rhi::UploadToImage(*s.backend, recorder, *texture->image, copy,
                                   std::span<const u8>(screen->pixels->data(), screen->pixels->size()));
            if (!uploaded.ok()) {
                MH4N_LOG(Error, kLog, "uploading the {} screen's pixels: {}", name, uploaded.error());
                return out;
            }
            result.cpu_textures_.push_back(texture);
            out.src.image = texture->image.get();
            out.src.x = 0;
            out.src.y = 0;
            out.src.width = screen->width;
            out.src.height = screen->height;
        }
        out.src.scaled_width = screen->ScaledWidth();
        out.src.scaled_height = screen->ScaledHeight();
        out.ok = true;
        return out;
    };
    // The uploads come first: a copy ends the target's pass.
    const Source top = prepare(frame.top, s.warned_invalid_top, "top");
    const Source bottom = prepare(frame.bottom, s.warned_invalid_bottom, "bottom");

    // ---- what is drawn, planned first so that FSR's first pass can run before the target's pass (MODIFIED) ----
    struct Planned {
        const Source* source;
        screen_layout::Rect rect;
        screen_layout::RectF crop;
        float opacity;
        ComposeResult::CpuTexture* fsr = nullptr;  // the screen as EASU drew it, at the size it is shown at
    };
    std::vector<Planned> plan;
    if (layout.top_shown) {
        plan.push_back({&top, layout.top, screen_layout::RectF{0.0f, 0.0f, 1.0f, 1.0f}, std::clamp(layout.top_opacity, 0.0f, 1.0f)});
    }
    if (layout.bottom_shown) {
        // The further views are drawn at the bottom screen's opacity (platform.h: ScreenOverlay::View).
        const float opacity = std::clamp(layout.bottom_opacity, 0.0f, 1.0f);
        plan.push_back({&bottom, layout.bottom, layout.bottom_crop, opacity});
        const u32 views = std::min<u32>(layout.view_count, static_cast<u32>(layout.views.size()));
        for (u32 i = 0; i < views; ++i) {
            plan.push_back({&bottom, layout.views[i].screen, layout.views[i].crop, opacity});
        }
    }

    const rhi::Sampler& sampler = options.linear_filter ? *s.linear : *s.nearest;
    // One quad of `rect` (of a target of w x h) showing `crop` of `src`, drawn with `pipeline`.
    const auto emit = [&](const SourceImage& src, const screen_layout::Rect& rect, const screen_layout::RectF& crop, float opacity,
                          rhi::Pipeline* pipeline_to_use, const rhi::Sampler& with_sampler, u32 target_w, u32 target_h) {
        const rhi::Rect scissor = ClipToTarget(rect, target_w, target_h);
        if (scissor.width == 0 || scissor.height == 0 || rect.right <= rect.left || rect.bottom <= rect.top) {
            return;
        }
        Vertex v[4];
        const double w = target_w;
        const double h = target_h;
        // Strip order: top-left, top-right, bottom-left, bottom-right.
        for (int i = 0; i < 4; ++i) {
            const double sx = i & 1;
            const double sy = i >> 1;
            v[i].pos[0] = static_cast<float>(2.0 * (sx != 0 ? rect.right : rect.left) / w - 1.0);
            v[i].pos[1] = static_cast<float>(2.0 * (sy != 0 ? rect.bottom : rect.top) / h - 1.0);
            const double fx = crop.left + sx * (crop.right - crop.left);
            const double fy = crop.top + sy * (crop.bottom - crop.top);
            SourceCoord(src, fx, fy, v[i].uv);
            v[i].bounds[0] = static_cast<float>((src.x + 0.5) / src.image->Width());
            v[i].bounds[1] = static_cast<float>((src.y + 0.5) / src.image->Height());
            v[i].bounds[2] = static_cast<float>((src.x + src.width - 0.5) / src.image->Width());
            v[i].bounds[3] = static_cast<float>((src.y + src.height - 0.5) / src.image->Height());
            v[i].alpha = opacity;
        }
        rhi::Result<rhi::RingAllocation> allocation = s.vertices->Map(sizeof(v), 16);
        if (!allocation.ok()) {
            MH4N_LOG(Error, kLog, "the vertex ring: {}", allocation.error());
            return;
        }
        std::memcpy(allocation->data.data(), v, sizeof(v));
        if (!s.vertices->Commit(sizeof(v)).ok()) {
            MH4N_LOG(Error, kLog, "the vertex ring cannot commit");
            return;
        }
        rhi::Result<rhi::BindingSet*> set = s.arena->Acquire(kSetDesc);
        if (!set.ok()) {
            MH4N_LOG(Error, kLog, "binding set: {}", set.error());
            return;
        }
        if (!(*set)->SetSampledImage(0, *src.image, rhi::ImageView::Sampled).ok() ||
            !(*set)->SetSampler(1, with_sampler).ok()) {
            MH4N_LOG(Error, kLog, "binding a screen's image failed");
            return;
        }
        recorder.SetPipeline(*pipeline_to_use);
        recorder.Bind(**set, 0);
        const rhi::VertexBufferBinding binding{s.vertices->GetBuffer().get(), allocation->offset};
        recorder.SetVertexBuffers(0, std::span<const rhi::VertexBufferBinding>(&binding, 1));
        rhi::Viewport viewport;
        viewport.width = static_cast<float>(target_w);
        viewport.height = static_cast<float>(target_h);
        recorder.SetViewport(viewport);
        recorder.SetScissor(scissor);
        recorder.Draw(4);
    };

    // FSR's first pass: each screen drawn larger than its picture (in both directions) goes through EASU into an image of the size it
    // is shown at (one pass of its own each: this is before the target's pass)
    if (options.fsr) {
        for (Planned& item : plan) {
            const Source& source = *item.source;
            const screen_layout::Rect& rect = item.rect;
            if (!source.ok || source.src.scaled_width <= 0 || source.src.scaled_height <= 0 || rect.right <= rect.left ||
                rect.bottom <= rect.top) {
                continue;
            }
            const u32 out_w = rect.right - rect.left, out_h = rect.bottom - rect.top;
            const double in_w = source.src.scaled_width * (item.crop.right - item.crop.left);
            const double in_h = source.src.scaled_height * (item.crop.bottom - item.crop.top);
            if (!(out_w > in_w * 1.05 && out_h > in_h * 1.05) || out_w > 8192 || out_h > 8192) {
                continue;
            }
            rhi::Pipeline* easu = s.EasuPipelineFor();
            if (easu == nullptr || s.RcasPipelineFor(target.GetFormat()) == nullptr) {
                break;
            }
            ComposeResult::CpuTexture* texture = s.AcquireTexture(out_w, out_h);
            if (texture == nullptr) {
                continue;
            }
            rhi::TargetPassDesc easu_pass;
            easu_pass.color = texture->image.get();
            easu_pass.color_load = rhi::LoadOp::Clear;
            easu_pass.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
            recorder.BeginTargetPass(easu_pass);
            emit(source.src, screen_layout::Rect{0, 0, out_w, out_h}, item.crop, 1.0f, easu, sampler, out_w, out_h);
            result.cpu_textures_.push_back(texture);
            item.fsr = texture;
        }
    }

    // ---- the pass: clear to black, then the draws ----
    const u32 target_w = target.Width();
    const u32 target_h = target.Height();
    rhi::TargetPassDesc pass;
    pass.color = &target;
    pass.color_load = rhi::LoadOp::Clear;
    pass.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    recorder.BeginTargetPass(pass);
    result.drawn = true;

    for (const Planned& item : plan) {
        if (!item.source->ok) {
            continue;
        }
        if (item.fsr != nullptr) {  // RCAS: the EASU image, 1:1
            SourceImage eased;
            eased.image = item.fsr->image.get();
            eased.width = item.fsr->width;
            eased.height = item.fsr->height;
            eased.scaled_width = eased.width;
            eased.scaled_height = eased.height;
            emit(eased, item.rect, screen_layout::RectF{0.0f, 0.0f, 1.0f, 1.0f}, item.opacity, s.RcasPipelineFor(target.GetFormat()),
                 *s.nearest, target_w, target_h);
        } else {
            emit(item.source->src, item.rect, item.crop, item.opacity, pipeline, sampler, target_w, target_h);
        }
    }

    if (options.ui != nullptr) {
        // The viewport and the scissor are the last screen's; the layer gets the whole target. (The pipeline, the
        // bindings and the vertex buffers are still the compositor's: the layer binds its own.)
        rhi::Viewport whole;
        whole.width = static_cast<float>(target_w);
        whole.height = static_cast<float>(target_h);
        recorder.SetViewport(whole);
        recorder.SetScissor(rhi::Rect{0, 0, target_w, target_h});
        options.ui->Record(&recorder, target_w, target_h);
    }
    return result;
}

rhi::TimelineValue Compositor::Submit(rhi::Recorder&& recorder, ComposeResult&& result) {
    State& s = *state_;
    const rhi::TimelineValue value = s.backend->Submit(std::move(recorder), result.waits);
    s.last_value = std::max(s.last_value, value);
    for (ComposeResult::CpuTexture* texture : result.cpu_textures_) {
        texture->busy_until = value;
    }
    if (result.frame_images_ != nullptr && !result.frame_images_->empty()) {
        s.backend->Retire(result.frame_images_, value);
    }
    result.cpu_textures_.clear();
    result.frame_images_.reset();
    return value;
}

} // namespace mh4n::present
