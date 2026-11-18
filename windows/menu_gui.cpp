// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by
// khorne-hub (https://github.com/khorne-hub/4u-native); see ADDITIONAL-TERMS and THIRD_PARTY_NOTICES.
//
// The look of the in-game menu: see menu_gui.h. The design is that of the menu of my MH3U port; the colours are this game's:
// warm brown and amber of its menus and carpets, the teal and cyan of its title, and parchment for the text.
#include "menu_gui.h"

#include "gpu/backend/vulkan_access.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_IMPL_VULKAN_NO_PROTOTYPES
#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

extern const unsigned char kCinzelTtf[];
extern const unsigned int kCinzelTtfSize;

namespace rhi = ::mh4n::gpu::rhi;

namespace ingame {

Layout LayoutOf(float sw, float sh) {
    Layout l;
    float h = std::max(sh * 0.86f, 300.0f);
    float w = std::min(std::max(std::min(sw * 0.78f, h * 1.5f), 420.0f), sw);
    h = std::min(h, sh);
    const float cx = sw / 2, cy = sh / 2;
    l.panel = {cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2};
    const float u = h / 640.0f;
    l.u = u;
    const float pad = 34.0f * u;
    const Rect inner = {l.panel.x0 + pad, l.panel.y0 + 22.0f * u, l.panel.x1 - pad, l.panel.y1 - 22.0f * u};
    l.header = {inner.x0, inner.y0, inner.x1, inner.y0 + 58.0f * u};
    l.tabs = {inner.x0, l.header.y1 + 6.0f * u, inner.x1, l.header.y1 + 6.0f * u + 46.0f * u};
    l.keys = {inner.x0, inner.y1 - 34.0f * u, inner.x1, inner.y1};
    l.help = {inner.x0, l.keys.y0 - 66.0f * u, inner.x1, l.keys.y0 - 8.0f * u};
    l.rows = {inner.x0, l.tabs.y1 + 14.0f * u, inner.x1, l.help.y0 - 8.0f * u};
    l.row_h = 55.0f * u;
    return l;
}

SetupLayout SetupLayoutOf(float sw, float sh) {
    SetupLayout s;
    s.l = LayoutOf(sw, sh);
    const Layout& l = s.l;
    const float u = l.u;
    const float x0 = l.header.x0, x1 = l.header.x1;
    const float c0 = l.header.y1 + 22.0f * u;
    s.intro = {x0, c0, x1, c0 + 118.0f * u};
    const float row1 = c0 + 132.0f * u, row2 = row1 + 100.0f * u;
    s.base_label_y = row1 + 8.0f * u;
    s.base_field = {x0, row1 + 26.0f * u, x1 - 190.0f * u, row1 + 76.0f * u};
    s.base_browse = {x1 - 170.0f * u, row1 + 26.0f * u, x1, row1 + 76.0f * u};
    s.update_label_y = row2 + 8.0f * u;
    s.update_field = {x0, row2 + 26.0f * u, x1 - 190.0f * u, row2 + 76.0f * u};
    s.update_browse = {x1 - 170.0f * u, row2 + 26.0f * u, x1, row2 + 76.0f * u};
    s.hint = {x0, row2 + 92.0f * u, x1, row2 + 122.0f * u};
    s.primary = {x1 - 210.0f * u, l.keys.y1 - 46.0f * u, x1, l.keys.y1};
    s.secondary = {s.primary.x0 - 18.0f * u - 170.0f * u, s.primary.y0, s.primary.x0 - 18.0f * u, s.primary.y1};
    s.step_y = c0 + 14.0f * u;
    s.bar = {x0, c0 + 44.0f * u, x1 - 110.0f * u, c0 + 72.0f * u};
    s.eta_y = c0 + 98.0f * u;
    s.log = {x0, c0 + 120.0f * u, x1, s.primary.y0 - 16.0f * u};
    return s;
}

void WindowOf(const Layout& l, std::size_t total, int sel, int* first, int* fit, int scroll) {
    const int f = std::max(1, static_cast<int>(std::floor(l.rows.H() / l.row_h)));
    int start = scroll >= 0 ? scroll : (sel < f ? 0 : sel + 1 - f);
    start = std::min(start, std::max(0, static_cast<int>(total) - f));
    *first = std::max(0, start);
    *fit = f;
}

namespace {

// The palette of this game: the amber of its logo and carpets (accent), the parchment of its dialogues (text), the teal and
// cyan of its title screen (values and the bar under the marked row), and the dark brown of its menus (the panel).
constexpr std::uint32_t kGold = 0xF2A22C;
constexpr std::uint32_t kGoldDim = 0x9C6A2A;
constexpr std::uint32_t kCream = 0xEFE2B9;
constexpr std::uint32_t kBlue = 0x35C6DD;
constexpr std::uint32_t kBlueDeep = 0x0C6F8C;
constexpr std::uint32_t kInk = 0x1A1208;
constexpr std::uint32_t kPanelTop = 0x26170A;
constexpr std::uint32_t kPanelBottom = 0x0D0804;
constexpr std::uint32_t kGlow = 0x8A5A1E;
constexpr std::uint32_t kCap = 0x2E2012;
constexpr std::uint32_t kWell = 0x0A0603;
constexpr std::uint32_t kOffTrack = 0x2A1E12;

ImU32 C(std::uint32_t rgb, float a = 1.0f) {
    return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, static_cast<int>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f));
}

enum class Align { Left, Right, Center };

struct Painter {
    ImDrawList* dl = nullptr;
    ImFont* font = nullptr;  // Cinzel
    ImFont* text = nullptr;  // the plain font, for the help
    float fade = 1.0f;

    ImVec2 Size(ImFont* f, float size, const char* t) const { return f->CalcTextSizeA(size, 3.0e38f, 0.0f, t); }
    // draws `t` with its anchor at (x, y_centre); returns the text's rectangle
    Rect Write(ImFont* f, float size, float x, float y, Align a, ImU32 col, const char* t) const {
        const ImVec2 s = Size(f, size, t);
        const float left = a == Align::Left ? x : a == Align::Right ? x - s.x : x - s.x / 2;
        dl->AddText(f, size, ImVec2(left, y - s.y / 2), col, t);
        return {left, y - s.y / 2, left + s.x, y + s.y / 2};
    }
    void GradV(const Rect& r, ImU32 top, ImU32 bottom) const {
        dl->AddRectFilledMultiColor(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), top, top, bottom, bottom);
    }
    void GradH(const Rect& r, ImU32 left, ImU32 right) const {
        dl->AddRectFilledMultiColor(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), left, right, right, left);
    }
    void Triangle(float cx, float cy, float size, int dir, ImU32 col) const {  // dir: -1 left, 1 right, -2 up, 2 down
        float dx = 0, dy = 0;
        if (dir == -1) dx = -1;
        else if (dir == 1) dx = 1;
        else dy = dir < -1 ? -1.0f : 1.0f;
        const float nx = -dy, ny = dx;
        const ImVec2 tip(cx + dx * size, cy + dy * size);
        const ImVec2 base(cx - dx * size * 0.6f, cy - dy * size * 0.6f);
        dl->AddTriangleFilled(tip, ImVec2(base.x + nx * size * 0.85f, base.y + ny * size * 0.85f),
                              ImVec2(base.x - nx * size * 0.85f, base.y - ny * size * 0.85f), col);
    }
    void Diamond(float cx, float cy, float r, ImU32 fill, ImU32 stroke = 0) const {
        const ImVec2 pts[4] = {{cx, cy - r}, {cx + r, cy}, {cx, cy + r}, {cx - r, cy}};
        dl->AddConvexPolyFilled(pts, 4, fill);
        if (stroke) dl->AddPolyline(pts, 4, stroke, 1.0f, ImDrawFlags_Closed);
    }
    void Frame(const Rect& r, float rounding, ImU32 col, float thickness) const {  // a stroke inside the rectangle
        const float h = thickness / 2;
        dl->AddRect(ImVec2(r.x0 + h, r.y0 + h), ImVec2(r.x1 - h, r.y1 - h), col, rounding, thickness, 0);
    }
    void Fill(const Rect& r, float rounding, ImU32 col) const { dl->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), col, rounding); }
};

Rect Shrink(const Rect& r, float dx, float dy) { return {r.x0 + dx, r.y0 + dy, r.x1 - dx, r.y1 - dy}; }

// The panel behind everything: gradient, warm glow, lattice, rosette, borders and studs.
void Backdrop(const Painter& p, const Layout& l, float fade) {
    const float u = l.u;
    const Rect panel = l.panel;
    const float ph = panel.H(), pw = panel.W();
    p.GradV(panel, C(kPanelTop, 0.97f * fade), C(kPanelBottom, 0.98f * fade));
    p.GradV({panel.x0, panel.y0, panel.x1, panel.y0 + ph * 0.38f}, C(kGlow, 0.20f * fade), C(kGlow, 0.0f));
    // the lattice: two sets of diagonals, a net of diamonds
    const Rect clip = Shrink(panel, 2 * u, 2 * u);
    p.dl->PushClipRect(ImVec2(clip.x0, clip.y0), ImVec2(clip.x1, clip.y1), true);
    const float step = 44.0f * u;
    const ImU32 line = C(kGold, 0.055f * fade);
    for (float x = panel.x0 - ph; x < panel.x1; x += step) {
        p.dl->AddLine(ImVec2(x, panel.y1), ImVec2(x + ph, panel.y0), line, 1.0f);
        p.dl->AddLine(ImVec2(x, panel.y0), ImVec2(x + ph, panel.y1), line, 1.0f);
    }
    // a rosette of rings and rays, big and faint, behind the lower right
    const ImVec2 c(panel.x1 - pw * 0.16f, panel.y1 - ph * 0.22f);
    const float radii[4] = {0.26f, 0.34f, 0.46f, 0.50f};
    for (int k = 0; k < 4; ++k) {
        p.dl->AddCircle(c, ph * radii[k], C(kGold, 0.07f * fade), 96, k % 2 == 0 ? 1.4f * u : 0.8f * u);
    }
    for (int i = 0; i < 36; ++i) {
        const float a = static_cast<float>(i) / 36.0f * 6.2831853f;
        const float s0 = ph * 0.34f, s1 = ph * (i % 3 == 0 ? 0.52f : 0.46f);
        p.dl->AddLine(ImVec2(c.x + std::cos(a) * s0, c.y + std::sin(a) * s0), ImVec2(c.x + std::cos(a) * s1, c.y + std::sin(a) * s1),
                      C(kGold, 0.06f * fade), 1.0f);
    }
    p.dl->PopClipRect();
    // the borders: a heavy amber one and a fine teal one inside
    p.Frame(panel, 3.0f * u, C(kGold, fade), 2.2f * u);
    p.Frame(Shrink(panel, 9 * u, 9 * u), 2.0f * u, C(kBlueDeep, 0.9f * fade), 1.0f);
    // corner studs and the middle ones
    const ImVec2 corners[4] = {{panel.x0, panel.y0}, {panel.x1, panel.y0}, {panel.x0, panel.y1}, {panel.x1, panel.y1}};
    for (const ImVec2& corner : corners) {
        const float sx = corner.x < (panel.x0 + panel.x1) / 2 ? 1.0f : -1.0f;
        const float sy = corner.y < (panel.y0 + panel.y1) / 2 ? 1.0f : -1.0f;
        const ImVec2 at(corner.x + sx * 9 * u, corner.y + sy * 9 * u);
        p.Diamond(at.x, at.y, 6.5f * u, C(kGold, fade), C(kInk, fade));
        p.dl->AddLine(at, ImVec2(at.x + sx * 40 * u, at.y), C(kGold, fade), 1.6f * u);
        p.dl->AddLine(at, ImVec2(at.x, at.y + sy * 40 * u), C(kGold, fade), 1.6f * u);
    }
    const float mx = (panel.x0 + panel.x1) / 2;
    p.Diamond(mx, panel.y0, 7.0f * u, C(kGold, fade), C(kInk, fade));
    p.Diamond(mx, panel.y1, 7.0f * u, C(kGold, fade), C(kInk, fade));
}

// The line under the title: amber, fading out at both ends, with a diamond in the middle.
void Rule(const Painter& p, float y, float left, float right, float u, float fade) {
    const float mid = (left + right) / 2;
    p.GradH({left, y, mid, y + 1.6f * u}, C(kGold, 0.0f), C(kGold, fade));
    p.GradH({mid, y, right, y + 1.6f * u}, C(kGold, fade), C(kGold, 0.0f));
    p.Diamond(mid, y + 0.8f * u, 5.0f * u, C(kGold, fade));
}

// One key of the bottom line: a small rounded cap with its name, then what it does. Returns the width it took.
float KeyHint(const Painter& p, float x, float y, const char* cap, const char* text, float u, float fade) {
    const float size = 15.0f * u;
    const ImVec2 s = p.Size(p.font, size, cap);
    const float cap_w = std::max(s.x + 14.0f * u, 26.0f * u);
    const Rect cap_rect = {x, y - 13.0f * u, x + cap_w, y + 13.0f * u};
    p.Fill(cap_rect, 6.0f * u, C(kCap, fade));
    p.Frame(cap_rect, 6.0f * u, C(kGoldDim, fade), 1.0f);
    p.Write(p.font, size, (cap_rect.x0 + cap_rect.x1) / 2, y, Align::Center, C(kGold, fade), cap);
    const Rect t = p.Write(p.text, 15.0f * u, cap_rect.x1 + 7.0f * u, y, Align::Left, C(kCream, 0.8f * fade), text);
    return t.x1 + 22.0f * u - x;
}

float Approach(float value, float target, float dt, float rate) {
    return value + (target - value) * (1.0f - std::exp(-dt * rate));
}

class Renderer final : public GuiRenderer {
public:
    Renderer(std::shared_ptr<rhi::Backend> backend, std::function<void()> redraw) : backend_(std::move(backend)), redraw_(std::move(redraw)) {}

    ~Renderer() override {
        if (!initialised_) return;
        const rhi::VulkanHandles h = rhi::Handles(*backend_);
        vkDeviceWaitIdle(h.device);
        ImGui::SetCurrentContext(context_);
        ImGui_ImplVulkan_Shutdown();
        ImGui::DestroyContext(context_);
        if (pass_ != VK_NULL_HANDLE) vkDestroyRenderPass(h.device, pass_, nullptr);
    }

    bool Init(int format) {
        const rhi::VulkanHandles h = rhi::Handles(*backend_);
        // a render pass with the target's one colour attachment: the pipelines of ImGui are made for it, and the pass the
        // compositor draws in (one colour attachment of this format, one sample, one subpass) is compatible with it
        VkAttachmentDescription attachment = {};
        attachment.format = static_cast<VkFormat>(format);
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        VkRenderPassCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = 1;
        info.pAttachments = &attachment;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        if (vkCreateRenderPass(h.device, &info, nullptr, &pass_) != VK_SUCCESS) return false;

        instance_ = h.instance;
        const auto loader = [](const char* name, void* user) -> PFN_vkVoidFunction {
            return vkGetInstanceProcAddr(static_cast<VkInstance>(user), name);
        };
        if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1, loader, instance_)) return false;

        context_ = ImGui::CreateContext();
        ImGui::SetCurrentContext(context_);
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;  // the font is in the program
        font_ = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kCinzelTtf), static_cast<int>(kCinzelTtfSize), 0.0f, &cfg);
        text_ = io.Fonts->AddFontDefault();
        if (font_ == nullptr || text_ == nullptr) return false;

        ImGui_ImplVulkan_InitInfo init = {};
        init.ApiVersion = VK_API_VERSION_1_1;
        init.Instance = h.instance;
        init.PhysicalDevice = h.physical_device;
        init.Device = h.device;
        init.QueueFamily = h.queue_family_index;
        init.Queue = h.queue;
        init.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
        init.MinImageCount = 2;
        init.ImageCount = 5;  // a set of vertex buffers per frame that may be in flight
        init.PipelineInfoMain.RenderPass = pass_;
        init.PipelineInfoMain.Subpass = 0;
        init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        if (!ImGui_ImplVulkan_Init(&init)) return false;
        initialised_ = true;
        last_ = std::chrono::steady_clock::now();
        return true;
    }

    bool Draw(void* recorder, const View& view, std::uint32_t width, std::uint32_t height) override {
        return Frame(recorder, width, height, [&](float dt) { Build(view, static_cast<float>(width), static_cast<float>(height), dt); });
    }
    bool DrawSetup(void* recorder, const SetupView& view, std::uint32_t width, std::uint32_t height) override {
        return Frame(recorder, width, height, [&](float dt) { BuildSetup(view, static_cast<float>(width), static_cast<float>(height), dt); });
    }

    template <class Build>
    bool Frame(void* recorder, std::uint32_t width, std::uint32_t height, Build&& build) {
        if (!initialised_) return false;
        ImGui::SetCurrentContext(context_);
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(now - last_).count(), 0.001f, 0.1f);
        last_ = now;
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
        io.DeltaTime = dt;
        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();
        build(dt);
        ImGui::Render();
        ImDrawData* data = ImGui::GetDrawData();
        if (data == nullptr || data->CmdLists.Size == 0) return true;

        // the textures first (the font's glyphs): an upload is a submission of its own to the queue, under its lock
        ImVector<ImTextureData*>* textures = data->Textures;
        if (textures != nullptr) {
            bool uploads = false;
            for (const ImTextureData* tex : *textures) {
                uploads |= tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates;
            }
            if (uploads) {
                rhi::QueueLock lock(*backend_);
                for (ImTextureData* tex : *textures) {
                    if (tex->Status != ImTextureStatus_OK) ImGui_ImplVulkan_UpdateTexture(tex);
                }
            }
        }
        data->Textures = nullptr;
        ImGui_ImplVulkan_RenderDrawData(data, rhi::CommandBuffer(*static_cast<rhi::Recorder*>(recorder)));
        data->Textures = textures;
        return true;
    }

private:
    std::shared_ptr<rhi::Backend> backend_;
    std::function<void()> redraw_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkRenderPass pass_ = VK_NULL_HANDLE;
    ImGuiContext* context_ = nullptr;
    ImFont* font_ = nullptr;
    ImFont* text_ = nullptr;
    bool initialised_ = false;
    std::chrono::steady_clock::time_point last_;
    // animation state (this thread's)
    float fade_ = 0.0f;
    float pill_x_ = -1.0f, bar_y_ = -1.0f;
    float blink_ = 0.0f;

    float setup_progress_ = 0.0f;

    // A button of the first start's screen.
    void Button(const Painter& p, const Rect& r, const std::string& text, bool primary, bool enabled, bool focus, bool hover, float u, float fade) {
        const float lift = hover && enabled ? 1.0f : 0.0f;
        if (primary && enabled) {
            p.GradV(r, C(kGold, fade), C(0xB9741C, fade));
            p.Frame(r, 9.0f * u, C(kInk, fade), 1.0f);
        } else {
            p.Fill(r, 9.0f * u, C(hover && enabled ? 0x3A2A18 : kCap, fade));
        }
        p.Frame(r, 9.0f * u, C(enabled ? kGold : kGoldDim, (enabled ? 1.0f : 0.5f) * fade), focus ? 2.4f * u : 1.0f);
        const ImU32 colour = !enabled ? C(kCream, 0.3f * fade) : primary ? C(kInk, fade) : C(kGold, fade);
        p.Write(font_, 21.0f * u, (r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2 - lift, Align::Center, colour, text.c_str());
    }

    void BuildSetup(const SetupView& v, float sw, float sh, float dt) {
        if (!v.open) return;
        const SetupLayout s = SetupLayoutOf(sw, sh);
        const Layout& l = s.l;
        const float u = l.u;
        bool moving = false;
        fade_ = Approach(fade_, 1.0f, dt, 10.0f);
        if (fade_ > 0.995f) fade_ = 1.0f; else moving = true;
        const float fade = fade_;
        Painter p;
        p.dl = ImGui::GetForegroundDrawList();
        p.font = font_;
        p.text = text_;
        p.fade = fade;
        blink_ += dt;
        p.dl->AddRectFilled(ImVec2(0, 0), ImVec2(sw, sh), C(kPanelBottom, 1.0f));  // nothing of the test picture shows through
        Backdrop(p, l, fade);
        const float hy = (l.header.y0 + l.header.y1) / 2;
        const Rect brand = p.Write(font_, 32.0f * u, l.header.x0, hy - 2 * u, Align::Left, C(kGold, fade), "Caravan4URecomp");
        p.Write(font_, 17.0f * u, brand.x1 + 18.0f * u, hy + 6 * u, Align::Left, C(kCream, 0.55f * fade), v.title.c_str());
        Rule(p, l.header.y1 - 2 * u, l.header.x0, l.header.x1, u, fade);

        const auto wrapped = [&](const Rect& r, const std::string& text, float size, ImU32 colour) {
            p.dl->AddText(text_, size, ImVec2(r.x0, r.y0), colour, text.c_str(), nullptr, r.W());
        };
        if (v.stage == 0) {
            wrapped(s.intro, v.intro, 19.0f * u, C(kCream, 0.9f * fade));
            const auto row = [&](const char* label, float label_y, const Rect& field, const Rect& browse, const std::string& path, int index) {
                p.Write(font_, 21.0f * u, l.header.x0, label_y, Align::Left, C(kCream, 0.92f * fade), label);
                p.Fill(field, 8.0f * u, C(kWell, 0.85f * fade));
                p.Frame(field, 8.0f * u, C(path.empty() ? kGoldDim : kBlueDeep, fade), 1.0f);
                p.dl->PushClipRect(ImVec2(field.x0 + 10 * u, field.y0), ImVec2(field.x1 - 10 * u, field.y1), true);
                if (path.empty()) {
                    p.Write(p.text, 18.0f * u, field.x0 + 16 * u, (field.y0 + field.y1) / 2, Align::Left, C(kCream, 0.35f * fade), v.placeholder.c_str());
                } else {
                    // the end of a long path is what tells the files apart
                    const float room = field.W() - 32.0f * u;
                    std::string shown = path;
                    while (shown.size() > 8 && p.Size(p.text, 18.0f * u, shown.c_str()).x > room) shown.erase(0, 4), shown.insert(0, "...");
                    p.Write(p.text, 18.0f * u, field.x0 + 16 * u, (field.y0 + field.y1) / 2, Align::Left, C(kBlue, fade), shown.c_str());
                }
                p.dl->PopClipRect();
                Button(p, browse, v.browse, false, true, v.focus == index, v.hover == index, u, fade);
            };
            row(v.base_label.c_str(), s.base_label_y, s.base_field, s.base_browse, v.base_path, 0);
            row(v.update_label.c_str(), s.update_label_y, s.update_field, s.update_browse, v.update_path, 1);
            p.Write(p.text, 17.0f * u, s.hint.x0, (s.hint.y0 + s.hint.y1) / 2, Align::Left, C(kCream, 0.6f * fade), v.hint.c_str());
        } else if (v.stage == 1 || v.stage == 3) {
            p.Write(font_, 26.0f * u, l.header.x0, s.step_y, Align::Left, C(kGold, fade), v.step.c_str());
            const float target = v.progress < 0 ? 0.0f : std::clamp(v.progress, 0.0f, 1.0f);
            setup_progress_ = Approach(setup_progress_, target, dt, 6.0f);
            if (std::fabs(setup_progress_ - target) > 0.002f) moving = true; else setup_progress_ = target;
            p.Fill(s.bar, 8.0f * u, C(kWell, fade));
            p.Frame(s.bar, 8.0f * u, C(kGoldDim, fade), 1.0f);
            const float fx = s.bar.x0 + s.bar.W() * setup_progress_;
            if (fx - s.bar.x0 > 2.0f) p.GradH(Shrink({s.bar.x0, s.bar.y0, fx, s.bar.y1}, 2 * u, 2 * u), C(kBlueDeep, fade), C(kGold, fade));
            p.Diamond(fx, (s.bar.y0 + s.bar.y1) / 2, 11.0f * u, C(kGold, fade), C(kInk, fade));
            char percent[16];
            std::snprintf(percent, sizeof percent, "%d%%", static_cast<int>(std::lround(setup_progress_ * 100.0f)));
            p.Write(font_, 24.0f * u, l.header.x1, (s.bar.y0 + s.bar.y1) / 2, Align::Right, C(kBlue, fade), percent);
            p.Write(p.text, 19.0f * u, l.header.x0, s.eta_y, Align::Left, C(kCream, 0.75f * fade), v.eta.c_str());
            // what the tool says, the last lines
            p.Fill(s.log, 8.0f * u, C(kWell, 0.7f * fade));
            p.Frame(s.log, 8.0f * u, C(kBlueDeep, fade), 1.0f);
            p.dl->PushClipRect(ImVec2(s.log.x0 + 6 * u, s.log.y0 + 4 * u), ImVec2(s.log.x1 - 6 * u, s.log.y1 - 4 * u), true);
            const float line_h = 22.0f * u;
            const int fit = std::max(1, static_cast<int>((s.log.H() - 12.0f * u) / line_h));
            const int first = std::max(0, static_cast<int>(v.log.size()) - fit);
            for (int i = first; i < static_cast<int>(v.log.size()); ++i) {
                const float y = s.log.y0 + 12.0f * u + line_h * static_cast<float>(i - first) + line_h / 2;
                p.Write(p.text, 15.5f * u, s.log.x0 + 14 * u, y, Align::Left, C(kCream, 0.55f * fade), v.log[static_cast<std::size_t>(i)].c_str());
            }
            p.dl->PopClipRect();
        } else {  // failed
            p.Write(font_, 26.0f * u, l.header.x0, s.step_y, Align::Left, C(0xFF7B6B, fade), v.step.c_str());
            wrapped({s.intro.x0, s.intro.y0 + 36 * u, s.intro.x1, s.intro.y1 + 40 * u}, v.error, 19.0f * u, C(kCream, 0.92f * fade));
            p.Fill(s.log, 8.0f * u, C(kWell, 0.7f * fade));
            p.Frame(s.log, 8.0f * u, C(kBlueDeep, fade), 1.0f);
            p.dl->PushClipRect(ImVec2(s.log.x0 + 6 * u, s.log.y0 + 4 * u), ImVec2(s.log.x1 - 6 * u, s.log.y1 - 4 * u), true);
            const float line_h = 22.0f * u;
            const int fit = std::max(1, static_cast<int>((s.log.H() - 12.0f * u) / line_h));
            const int first = std::max(0, static_cast<int>(v.log.size()) - fit);
            for (int i = first; i < static_cast<int>(v.log.size()); ++i) {
                const float y = s.log.y0 + 12.0f * u + line_h * static_cast<float>(i - first) + line_h / 2;
                p.Write(p.text, 15.5f * u, s.log.x0 + 14 * u, y, Align::Left, C(kCream, 0.55f * fade), v.log[static_cast<std::size_t>(i)].c_str());
            }
            p.dl->PopClipRect();
        }
        if (!v.primary.empty()) Button(p, s.primary, v.primary, true, v.primary_enabled, v.focus == 2, v.hover == 2, u, fade);
        if (!v.secondary.empty()) Button(p, s.secondary, v.secondary, false, true, v.focus == 3, v.hover == 3, u, fade);
        if (moving && redraw_) redraw_();
    }

    void Build(const View& view, float sw, float sh, float dt) {
        const Layout l = LayoutOf(sw, sh);
        const float u = l.u;
        if (!view.open) {
            fade_ = 0.0f;
            pill_x_ = bar_y_ = -1.0f;
            return;
        }
        bool moving = false;
        fade_ = Approach(fade_, 1.0f, dt, 14.0f);
        if (fade_ > 0.995f) fade_ = 1.0f; else moving = true;
        const float fade = fade_;
        Painter p;
        p.dl = ImGui::GetForegroundDrawList();
        p.font = font_;
        p.text = text_;
        p.fade = fade;
        const ImU32 white = IM_COL32(255, 255, 255, 255);
        blink_ += dt;

        // the game dims behind the panel
        p.dl->AddRectFilled(ImVec2(0, 0), ImVec2(sw, sh), IM_COL32(0, 0, 0, static_cast<int>(170.0f * fade)));
        Backdrop(p, l, fade);

        // header: the brand and the page's name
        const Rect brand = p.Write(font_, 32.0f * u, l.header.x0, (l.header.y0 + l.header.y1) / 2 - 2 * u, Align::Left, C(kGold, fade), "Caravan4URecomp");
        p.Write(font_, 17.0f * u, brand.x1 + 18.0f * u, (l.header.y0 + l.header.y1) / 2 + 6 * u, Align::Left, C(kCream, 0.55f * fade),
                view.title.c_str());
        const float hint_y = (l.header.y0 + l.header.y1) / 2;
        const Rect rb = p.Write(font_, 16.0f * u, l.header.x1, hint_y, Align::Right, C(kGoldDim, fade), "RB");
        p.Triangle(rb.x0 - 14.0f * u, hint_y, 6.0f * u, 1, C(kGoldDim, fade));
        const Rect lb = p.Write(font_, 16.0f * u, rb.x0 - 74.0f * u, hint_y, Align::Right, C(kGoldDim, fade), "LB");
        p.Triangle(lb.x1 + 14.0f * u, hint_y, 6.0f * u, -1, C(kGoldDim, fade));
        Rule(p, l.header.y1 - 2 * u, l.header.x0, l.header.x1, u, fade);

        // tabs, with a pill that slides to the one in force
        const int tab_count = std::max<int>(1, static_cast<int>(view.tabs.size()));
        const float tab_w = l.tabs.W() / static_cast<float>(tab_count);
        const float pill_target = l.tabs.x0 + tab_w * static_cast<float>(view.tab);
        if (pill_x_ < 0) pill_x_ = pill_target;
        pill_x_ = Approach(pill_x_, pill_target, dt, 18.0f);
        if (std::fabs(pill_x_ - pill_target) > 0.5f) moving = true; else pill_x_ = pill_target;
        {
            const Rect pill = Shrink({pill_x_, l.tabs.y0, pill_x_ + tab_w, l.tabs.y1}, 10 * u, 5 * u);
            p.GradV(pill, C(kGold, fade), C(0xB9741C, fade));
            p.Frame(pill, 8.0f * u, C(kInk, fade), 1.0f);
        }
        for (int i = 0; i < static_cast<int>(view.tabs.size()); ++i) {
            const Rect cell = {l.tabs.x0 + tab_w * static_cast<float>(i), l.tabs.y0, l.tabs.x0 + tab_w * static_cast<float>(i + 1), l.tabs.y1};
            const bool on = i == view.tab;
            const bool hover = i == view.hover_tab;
            // the text over the pill is dark only while the pill is under it
            const float under = std::clamp(1.0f - std::fabs(pill_x_ - cell.x0) / tab_w, 0.0f, 1.0f);
            ImU32 colour = hover && !on ? C(kCream, fade) : C(kCream, 0.62f * fade);
            if (under > 0.5f || on) colour = C(kInk, fade);
            p.Write(font_, 23.0f * u, (cell.x0 + cell.x1) / 2, (cell.y0 + cell.y1) / 2, Align::Center, colour, view.tabs[static_cast<std::size_t>(i)].c_str());
        }

        // rows
        int first = 0, fit = 1;
        WindowOf(l, view.rows.size(), view.sel, &first, &fit, view.scroll);
        const float label_x = l.rows.x0 + 26.0f * u;
        const float value_left = l.rows.x0 + l.rows.W() * 0.50f;
        const float value_right = l.rows.x1 - 18.0f * u;
        // the bar of the marked row slides to it
        if (view.sel >= first && view.sel < first + fit) {
            const float target = l.rows.y0 + l.row_h * static_cast<float>(view.sel - first);
            if (bar_y_ < 0) bar_y_ = target;
            bar_y_ = Approach(bar_y_, target, dt, 22.0f);
            if (std::fabs(bar_y_ - target) > 0.5f) moving = true; else bar_y_ = target;
            const Rect bar = {l.rows.x0, bar_y_ + 3 * u, l.rows.x1, bar_y_ + l.row_h - 3 * u};
            p.GradH(bar, C(kBlueDeep, 0.85f * fade), C(kBlueDeep, 0.0f));
            p.Fill({bar.x0, bar.y0, bar.x0 + 4 * u, bar.y1}, 0.0f, C(kGold, fade));
            p.Triangle(l.rows.x0 + 17 * u, bar_y_ + l.row_h / 2, 7.0f * u, 1, C(kGold, fade));
        }
        for (int slot = 0, index = first; index < static_cast<int>(view.rows.size()) && slot < fit; ++slot, ++index) {
            const ViewRow& row = view.rows[static_cast<std::size_t>(index)];
            const Rect rect = {l.rows.x0, l.rows.y0 + l.row_h * static_cast<float>(slot), l.rows.x1, l.rows.y0 + l.row_h * static_cast<float>(slot + 1)};
            const float cy = (rect.y0 + rect.y1) / 2;
            const bool marked = row.selected;
            const bool live = row.enabled;
            const bool selectable = row.kind != ViewRow::Info;
            const ImU32 label_col = !live ? C(kCream, 0.30f * fade) : marked ? C(kGold, fade) : C(kCream, 0.92f * fade);
            const ImU32 value_col = !live ? C(kCream, 0.30f * fade) : marked ? white : C(kBlue, fade);
            if (row.kind == ViewRow::Info) {
                const ImU32 info = row.color == 1 ? C(kGold, fade) : row.color == 3 ? C(0x7FE08A, fade) : row.color == 4 ? C(0xFF7B6B, fade)
                                  : row.color == 2 ? C(kCream, 0.55f * fade) : C(kCream, 0.85f * fade);
                p.dl->PushClipRect(ImVec2(rect.x0 + 14 * u, rect.y0), ImVec2(rect.x1 - 14 * u, rect.y1), true);
                p.Write(p.text, 19.0f * u, label_x, cy, Align::Left, info, row.label.c_str());
                p.dl->PopClipRect();
            } else {
                const float label_right = row.kind == ViewRow::Room ? value_right - 150.0f * u : value_left - 14.0f * u;
                p.dl->PushClipRect(ImVec2(label_x - 2, rect.y0), ImVec2(label_right, rect.y1), true);
                p.Write(font_, row.kind == ViewRow::Room ? 22.0f * u : 26.0f * u, label_x, cy, Align::Left, label_col, row.label.c_str());
                p.dl->PopClipRect();
            }
            switch (row.kind) {
            case ViewRow::Choice: {
                auto arrow = [&](bool usable) {
                    return !live || !usable ? C(kGoldDim, 0.35f * fade) : marked ? C(kGold, fade) : C(kGoldDim, fade);
                };
                const float lx = value_left + 12 * u, rx = value_right - 12 * u;
                p.Triangle(lx, cy, 8.0f * u, -1, arrow(row.left));
                p.Triangle(rx, cy, 8.0f * u, 1, arrow(row.right));
                p.dl->PushClipRect(ImVec2(lx + 12 * u, rect.y0), ImVec2(rx - 12 * u, rect.y1), true);
                p.Write(font_, 23.0f * u, (lx + rx) / 2, cy, Align::Center, value_col, row.value.c_str());
                p.dl->PopClipRect();
                break;
            }
            case ViewRow::Slider: {
                const float text_w = 74.0f * u;
                const Rect bar = {value_left, cy - 5 * u, value_right - text_w - 12 * u, cy + 5 * u};
                p.Fill(bar, 5.0f * u, C(kWell, fade));
                p.Frame(bar, 5.0f * u, live ? C(kGoldDim, fade) : C(kGoldDim, 0.4f * fade), 1.0f);
                const float fx = bar.x0 + bar.W() * std::clamp(row.frac, 0.0f, 1.0f);
                if (fx - bar.x0 > 1.0f) {
                    p.GradH(Shrink({bar.x0, bar.y0, fx, bar.y1}, 1.5f * u, 1.5f * u), C(kBlueDeep, live ? fade : 0.4f * fade), C(live ? kGold : kGoldDim, fade));
                }
                p.Diamond(fx, cy, 8.5f * u, C(live ? kGold : kGoldDim, fade), C(kInk, fade));
                p.Write(font_, 23.0f * u, value_right, cy, Align::Right, value_col, row.value.c_str());
                break;
            }
            case ViewRow::Toggle: {
                const Rect track = {value_right - 34 * u - 33 * u, cy - 14 * u, value_right - 34 * u + 33 * u, cy + 14 * u};
                p.Fill(track, 14.0f * u, row.on ? C(kBlueDeep, fade) : C(kOffTrack, fade));
                p.Frame(track, 14.0f * u, C(row.on ? kGold : kGoldDim, fade), 1.0f);
                const float knob = row.on ? track.x1 - 14 * u : track.x0 + 14 * u;
                p.dl->AddCircleFilled(ImVec2(knob, cy), 10 * u, C(row.on ? kGold : kCream, row.on ? fade : 0.6f * fade), 24);
                p.Write(font_, 20.0f * u, track.x0 - 14 * u, cy, Align::Right, value_col, row.value.c_str());
                break;
            }
            case ViewRow::Button: {
                const Rect cap = {value_right - 62 * u - 62 * u, cy - 17 * u, value_right - 62 * u + 62 * u, cy + 17 * u};
                p.Fill(cap, 8.0f * u, marked ? C(kGold, fade) : C(kCap, fade));
                p.Frame(cap, 8.0f * u, C(live ? kGold : kGoldDim, fade), 1.0f);
                p.Write(font_, 18.0f * u, (cap.x0 + cap.x1) / 2, cy, Align::Center,
                        marked ? C(kInk, fade) : live ? C(kGold, fade) : C(kCream, 0.3f * fade), row.value.c_str());
                break;
            }
            case ViewRow::Text: {
                const Rect field = {value_left, cy - 19 * u, value_right, cy + 19 * u};
                p.Fill(field, 7.0f * u, C(kWell, 0.85f * fade));
                p.Frame(field, 7.0f * u, C(row.caret ? kGold : kGoldDim, fade), row.caret ? 1.6f * u : 1.0f);
                p.dl->PushClipRect(ImVec2(field.x0 + 8 * u, field.y0), ImVec2(field.x1 - 8 * u, field.y1), true);
                std::string shown = row.value;
                const bool blink = row.caret && std::fmod(blink_, 1.0f) < 0.6f;
                const Rect t = p.Write(font_, 21.0f * u, field.x0 + 14 * u, cy, Align::Left, row.caret ? white : value_col, shown.c_str());
                if (blink) p.dl->AddLine(ImVec2(t.x1 + 3 * u, cy - 11 * u), ImVec2(t.x1 + 3 * u, cy + 11 * u), C(kGold, fade), 1.8f * u);
                p.dl->PopClipRect();
                if (row.caret) moving = true;
                break;
            }
            case ViewRow::Room:
                p.Write(font_, 21.0f * u, value_right, cy, Align::Right, value_col, row.value.c_str());
                break;
            case ViewRow::Info:
                break;
            }
            // a fine line between rows
            p.dl->AddLine(ImVec2(rect.x0 + 20 * u, rect.y1), ImVec2(rect.x1 - 20 * u, rect.y1), C(kGold, 0.10f * fade), 1.0f);
            (void)selectable;
        }
        // more rows above or below
        if (first > 0) p.Triangle((l.rows.x0 + l.rows.x1) / 2, l.rows.y0 + 2 * u, 6.0f * u, -2, C(kGold, fade));
        if (first + fit < static_cast<int>(view.rows.size())) p.Triangle((l.rows.x0 + l.rows.x1) / 2, l.rows.y1 - 2 * u, 6.0f * u, 2, C(kGold, fade));

        // the help of the marked row, or what the menu just said, and the settings that wait for a restart
        {
            std::string text;
            const bool said = !view.message.empty();
            if (said) {
                text = view.message;
            } else if (view.sel >= 0 && view.sel < static_cast<int>(view.rows.size())) {
                text = view.rows[static_cast<std::size_t>(view.sel)].help;
            }
            if (!view.notice.empty()) text += (text.empty() ? "" : "\n") + view.notice;
            if (!text.empty()) {
                const float wrap = l.help.W() - 40.0f * u;
                const ImVec2 s = text_->CalcTextSizeA(17.5f * u, 3.0e38f, wrap, text.c_str());
                const Rect box = {l.help.x0, l.help.y0, l.help.x1, l.help.y0 + std::max(l.help.H(), s.y + 14.0f * u)};
                p.Fill(box, 6.0f * u, C(kWell, 0.75f * fade));
                p.Frame(box, 6.0f * u, C(kBlueDeep, fade), 1.0f);
                p.Diamond(box.x0 + 16 * u, (box.y0 + box.y1) / 2, 4.5f * u, C(kGold, fade));
                p.dl->AddText(text_, 17.5f * u, ImVec2(box.x0 + 32 * u, (box.y0 + box.y1) / 2 - s.y / 2), said ? C(kGold, fade) : C(kCream, 0.88f * fade),
                              text.c_str(), nullptr, wrap);
            }
        }

        // the keys
        float x = l.keys.x0;
        const float ky = (l.keys.y0 + l.keys.y1) / 2;
        for (const auto& [cap, what] : view.keys) x += KeyHint(p, x, ky, cap.c_str(), what.c_str(), u, fade);

        if (moving && redraw_) redraw_();
    }
};

}  // namespace

std::unique_ptr<GuiRenderer> MakeGuiRenderer(std::shared_ptr<rhi::Backend> backend, int format, std::function<void()> redraw) {
    auto renderer = std::make_unique<Renderer>(std::move(backend), std::move(redraw));
    if (!renderer->Init(format)) return nullptr;
    return renderer;
}

}  // namespace ingame
