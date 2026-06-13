#include "imgui_layer.hpp"

#include <imgui.h>

#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace th2 {
namespace {

std::string find_imgui_font_path()
{
    if (!FcInit()) {
        return {};
    }
    FcPattern* pattern = FcNameParse(
        reinterpret_cast<const FcChar8*>("sans-serif"));
    if (!pattern) {
        return {};
    }
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pattern, &result);
    std::string path;
    if (match) {
        FcChar8* file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
            path = reinterpret_cast<const char*>(file);
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pattern);
    return path;
}

ImTextureID texture_id(SDL_Texture* texture)
{
    return static_cast<ImTextureID>(
        reinterpret_cast<std::uintptr_t>(texture));
}

SDL_Texture* sdl_texture(ImTextureID id)
{
    return reinterpret_cast<SDL_Texture*>(
        static_cast<std::uintptr_t>(id));
}

void update_texture(SDL_Renderer* renderer, ImTextureData* texture)
{
    if (texture->Status == ImTextureStatus_WantDestroy) {
        SDL_DestroyTexture(sdl_texture(texture->GetTexID()));
        texture->SetTexID(ImTextureID_Invalid);
        texture->BackendUserData = nullptr;
        texture->SetStatus(ImTextureStatus_Destroyed);
        return;
    }

    std::vector<unsigned char> rgba;
    const void* pixels = texture->GetPixels();
    int pitch = texture->GetPitch();
    if (texture->Format == ImTextureFormat_Alpha8) {
        rgba.resize(texture->Width * texture->Height * 4);
        for (int i = 0; i < texture->Width * texture->Height; ++i) {
            rgba[i * 4 + 0] = 255;
            rgba[i * 4 + 1] = 255;
            rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = texture->Pixels[i];
        }
        pixels = rgba.data();
        pitch = texture->Width * 4;
    }

    SDL_Texture* sdl = sdl_texture(texture->GetTexID());
    if (texture->Status == ImTextureStatus_WantCreate) {
        sdl = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
            texture->Width, texture->Height);
        if (!sdl) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_SetTextureBlendMode(sdl, SDL_BLENDMODE_BLEND);
        texture->SetTexID(texture_id(sdl));
        texture->BackendUserData = sdl;
    }
    if (!SDL_UpdateTexture(sdl, nullptr, pixels, pitch)) {
        throw std::runtime_error(SDL_GetError());
    }
    texture->SetStatus(ImTextureStatus_OK);
}

ImGuiKey imgui_key(SDL_Keycode key)
{
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_INSERT: return ImGuiKey_Insert;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_A: return ImGuiKey_A;
    case SDLK_C: return ImGuiKey_C;
    case SDLK_V: return ImGuiKey_V;
    case SDLK_X: return ImGuiKey_X;
    case SDLK_Y: return ImGuiKey_Y;
    case SDLK_Z: return ImGuiKey_Z;
    default: return ImGuiKey_None;
    }
}

}  // namespace

ImGuiLayer::ImGuiLayer(SDL_Window* window, SDL_Renderer* renderer)
    : window_(window), renderer_(renderer)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    imgui_font_path_ = find_imgui_font_path();
}

ImGuiLayer::~ImGuiLayer()
{
    if (SDL_TextInputActive(window_)) {
        SDL_StopTextInput(window_);
    }
    for (auto* texture : ImGui::GetPlatformIO().Textures) {
        SDL_DestroyTexture(sdl_texture(texture->GetTexID()));
        texture->SetTexID(ImTextureID_Invalid);
        texture->BackendUserData = nullptr;
    }
    ImGui::DestroyContext();
}

void ImGuiLayer::process_event(const SDL_Event& event)
{
    auto& io = ImGui::GetIO();
    // SDL mouse event coordinates are in window points, which matches the
    // ImGui logical coordinate system set in new_frame().
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        io.AddMousePosEvent(event.motion.x, event.motion.y);
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
               || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        int button = -1;
        if (event.button.button == SDL_BUTTON_LEFT) button = 0;
        if (event.button.button == SDL_BUTTON_RIGHT) button = 1;
        if (event.button.button == SDL_BUTTON_MIDDLE) button = 2;
        if (button >= 0) {
            io.AddMouseButtonEvent(
                button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
    } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        io.AddMousePosEvent(event.wheel.mouse_x, event.wheel.mouse_y);
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
    } else if (event.type == SDL_EVENT_TEXT_INPUT) {
        io.AddInputCharactersUTF8(event.text.text);
    } else if (event.type == SDL_EVENT_KEY_DOWN
               || event.type == SDL_EVENT_KEY_UP) {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        const auto key = imgui_key(event.key.key);
        if (key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        const auto modifiers = SDL_GetModState();
        io.AddKeyEvent(ImGuiMod_Ctrl, (modifiers & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (modifiers & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (modifiers & SDL_KMOD_ALT) != 0);
        io.AddKeyEvent(ImGuiMod_Super, (modifiers & SDL_KMOD_GUI) != 0);
    }
}

void ImGuiLayer::rebuild_font_atlas(float display_scale)
{
    auto& io = ImGui::GetIO();
    io.Fonts->Clear();
    if (!imgui_font_path_.empty()) {
        // Scale the reference 13px default size to the monitor DPI.
        io.Fonts->AddFontFromFileTTF(
            imgui_font_path_.c_str(), 13.0f * display_scale);
    }
    if (io.Fonts->Fonts.empty()) {
        io.Fonts->AddFontDefault();
    }
    last_font_scale_ = display_scale;
}
void ImGuiLayer::new_frame(
    int window_width, int window_height, float display_scale)
{
    auto& io = ImGui::GetIO();
    display_scale_ = display_scale > 0.0f ? display_scale : 1.0f;
    // ImGui's logical canvas covers the full window in points; the display
    // scale keeps the UI elements at a comfortable OS-DPI size when rendered.
    io.DisplaySize = ImVec2(
        static_cast<float>(window_width),
        static_cast<float>(window_height));
    io.DisplayFramebufferScale = ImVec2(display_scale_, display_scale_);

    // Rebuild the font atlas when the scale changes so text stays crisp
    // at the monitor's native resolution.
    if (std::abs(display_scale_ - last_font_scale_) > 0.05f) {
        rebuild_font_atlas(display_scale_);
    }

    const auto ticks = SDL_GetTicks();
    io.DeltaTime = last_frame_ticks_ == 0
        ? 1.0f / 60.0f
        : std::max((ticks - last_frame_ticks_) / 1000.0f, 0.001f);
    last_frame_ticks_ = ticks;
    ImGui::NewFrame();
}

void ImGuiLayer::render()
{
    ImGui::Render();
    const bool wants_text = ImGui::GetIO().WantTextInput;
    if (wants_text && !SDL_TextInputActive(window_)) {
        SDL_StartTextInput(window_);
    } else if (!wants_text && SDL_TextInputActive(window_)) {
        SDL_StopTextInput(window_);
    }
    const auto* data = ImGui::GetDrawData();
    if (!data) {
        return;
    }
    if (data->Textures) {
        for (auto* texture : *data->Textures) {
            if (texture->Status != ImTextureStatus_OK) {
                update_texture(renderer_, texture);
            }
        }
    }
    for (int list_index = 0; list_index < data->CmdListsCount; ++list_index) {
        const auto* list = data->CmdLists[list_index];
        std::vector<SDL_Vertex> vertices;
        vertices.reserve(list->VtxBuffer.Size);
        for (const auto& vertex : list->VtxBuffer) {
            const auto color = vertex.col;
            vertices.push_back(SDL_Vertex{
                {vertex.pos.x, vertex.pos.y},
                {
                    ((color >> IM_COL32_R_SHIFT) & 0xff) / 255.0f,
                    ((color >> IM_COL32_G_SHIFT) & 0xff) / 255.0f,
                    ((color >> IM_COL32_B_SHIFT) & 0xff) / 255.0f,
                    ((color >> IM_COL32_A_SHIFT) & 0xff) / 255.0f,
                },
                {vertex.uv.x, vertex.uv.y},
            });
        }
        for (const auto& command : list->CmdBuffer) {
            if (command.UserCallback) {
                command.UserCallback(list, &command);
                continue;
            }
            const SDL_Rect clip{
                static_cast<int>(command.ClipRect.x),
                static_cast<int>(command.ClipRect.y),
                static_cast<int>(command.ClipRect.z - command.ClipRect.x),
                static_cast<int>(command.ClipRect.w - command.ClipRect.y),
            };
            SDL_SetRenderClipRect(renderer_, &clip);
            auto* texture = sdl_texture(command.GetTexID());
            std::vector<int> indices;
            indices.reserve(command.ElemCount);
            for (unsigned int i = 0; i < command.ElemCount; ++i) {
                indices.push_back(
                    list->IdxBuffer[command.IdxOffset + i]
                    + static_cast<int>(command.VtxOffset));
            }
            SDL_RenderGeometry(
                renderer_, texture, vertices.data(),
                static_cast<int>(vertices.size()),
                indices.data(), static_cast<int>(indices.size()));
        }
    }
    SDL_SetRenderClipRect(renderer_, nullptr);
}

bool ImGuiLayer::wants_input() const
{
    const auto& io = ImGui::GetIO();
    return io.WantCaptureKeyboard || io.WantCaptureMouse;
}

bool ImGuiLayer::wants_mouse() const
{
    return ImGui::GetIO().WantCaptureMouse;
}

}  // namespace th2
