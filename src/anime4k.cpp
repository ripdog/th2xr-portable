#include "anime4k.hpp"

#include <cstring>
#include <stdexcept>
#include <vector>

namespace th2 {
namespace {

struct TextureDeleter {
    void operator()(SDL_Texture* texture) const { SDL_DestroyTexture(texture); }
};

SDL_FRect letterbox_rect(int output_width, int output_height)
{
    const float scale = std::min(
        output_width / 800.0f, output_height / 600.0f);
    const float width = 800.0f * scale;
    const float height = 600.0f * scale;
    return {
        (output_width - width) / 2.0f,
        (output_height - height) / 2.0f,
        width,
        height};
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path)
{
    std::size_t size = 0;
    void* data = SDL_LoadFile(path.string().c_str(), &size);
    if (!data) {
        throw std::runtime_error("cannot open shader: " + path.string());
    }
    std::vector<std::uint8_t> result(size);
    std::memcpy(result.data(), data, size);
    SDL_free(data);
    return result;
}

}  // namespace

struct Anime4K::Impl {
    SDL_Renderer* renderer;
    std::filesystem::path shader_dir;
    SDL_GPUDevice* device = nullptr;
    std::unique_ptr<SDL_Texture, TextureDeleter> art;
    std::unique_ptr<SDL_Texture, TextureDeleter> authentic_text;
    std::unique_ptr<SDL_Texture, TextureDeleter> overlay;
    std::unique_ptr<SDL_Texture, TextureDeleter> sidebar;
    int overlay_width = 0;
    int overlay_height = 0;
    std::vector<SDL_GPUShader*> shaders;
    std::vector<SDL_GPURenderState*> states;
    bool ready = false;

    SDL_GPUShader* load_shader(
        const std::filesystem::path& path,
        SDL_GPUShaderFormat format,
        const char* entrypoint,
        int samplers)
    {
        const auto code = read_file(path);
        SDL_GPUShaderCreateInfo info{};
        info.code = code.data();
        info.code_size = code.size();
        info.entrypoint = entrypoint;
        info.format = format;
        info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        info.num_samplers = samplers;
        auto* shader = SDL_CreateGPUShader(device, &info);
        if (!shader) {
            throw std::runtime_error(SDL_GetError());
        }
        shaders.push_back(shader);
        return shader;
    }

    SDL_GPURenderState* make_state(SDL_GPUShader* shader)
    {
        SDL_GPURenderStateCreateInfo info{};
        info.fragment_shader = shader;
        auto* state = SDL_CreateGPURenderState(renderer, &info);
        if (!state) {
            throw std::runtime_error(SDL_GetError());
        }
        states.push_back(state);
        return state;
    }

    auto target(int width, int height)
    {
        auto texture = std::unique_ptr<SDL_Texture, TextureDeleter>(
            SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                              SDL_TEXTUREACCESS_TARGET, width, height));
        if (!texture) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture.get(), SDL_SCALEMODE_LINEAR);
        return texture;
    }

    void init()
    {
        device = SDL_GetGPURendererDevice(renderer);
        if (!device) {
            return;
        }
        const auto formats = SDL_GetGPUShaderFormats(device);
        art = target(800, 600);
        authentic_text = target(800, 600);
        if (formats & SDL_GPU_SHADERFORMAT_SPIRV) {
            make_state(load_shader(
                shader_dir / "apply.frag.spv",
                SDL_GPU_SHADERFORMAT_SPIRV, "main", 1));
        } else if (formats & SDL_GPU_SHADERFORMAT_MSL) {
            make_state(load_shader(
                shader_dir / "apply.frag.msl",
                SDL_GPU_SHADERFORMAT_MSL, "main0", 1));
        } else {
            art.reset();
            authentic_text.reset();
            return;
        }
        ready = true;
    }

    Impl(SDL_Renderer* renderer_, const std::filesystem::path& shader_dir_)
        : renderer(renderer_), shader_dir(shader_dir_)
    {
        init();
    }

    ~Impl()
    {
        for (auto* state : states) {
            SDL_DestroyGPURenderState(state);
        }
        for (auto* shader : shaders) {
            SDL_ReleaseGPUShader(device, shader);
        }
    }

    void reset()
    {
        for (auto* state : states) {
            SDL_DestroyGPURenderState(state);
        }
        states.clear();
        for (auto* shader : shaders) {
            SDL_ReleaseGPUShader(device, shader);
        }
        shaders.clear();
        overlay.reset();
        sidebar.reset();
        overlay_width = 0;
        overlay_height = 0;
        ready = false;
        init();
    }

    void ensure_overlay()
    {
        int output_width = 0;
        int output_height = 0;
        if (!SDL_GetRenderOutputSize(renderer, &output_width, &output_height)) {
            throw std::runtime_error(SDL_GetError());
        }
        const auto rect = letterbox_rect(output_width, output_height);
        const int width = static_cast<int>(rect.w);
        const int height = static_cast<int>(rect.h);
        if (overlay && sidebar
            && width == overlay_width && height == overlay_height) {
            return;
        }
        overlay.reset(SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
            width, height));
        if (!overlay) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_SetTextureBlendMode(overlay.get(), SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(overlay.get(), SDL_SCALEMODE_LINEAR);
        sidebar.reset(SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
            width, height));
        if (!sidebar) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_SetTextureBlendMode(sidebar.get(), SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(sidebar.get(), SDL_SCALEMODE_LINEAR);
        overlay_width = width;
        overlay_height = height;
    }

    void present()
    {
        ensure_overlay();
        int output_width = 0;
        int output_height = 0;
        if (!SDL_GetRenderOutputSize(renderer, &output_width, &output_height)) {
            throw std::runtime_error(SDL_GetError());
        }
        const auto destination = letterbox_rect(output_width, output_height);

        SDL_SetRenderScale(renderer, 1.0f, 1.0f);
        SDL_SetRenderTarget(renderer, nullptr);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_SetGPURenderState(renderer, states[0]);
        SDL_RenderTexture(renderer, art.get(), nullptr, &destination);
        SDL_SetGPURenderState(renderer, nullptr);
        SDL_RenderTexture(
            renderer, authentic_text.get(), nullptr, &destination);
        SDL_RenderTexture(renderer, overlay.get(), nullptr, &destination);
        SDL_RenderTexture(renderer, sidebar.get(), nullptr, &destination);
    }
};

Anime4K::Anime4K(
    SDL_Renderer* renderer, const std::filesystem::path& shader_dir)
    : impl_(std::make_unique<Impl>(renderer, shader_dir))
{
}

Anime4K::~Anime4K() = default;

Anime4K::Anime4K(Anime4K&&) noexcept = default;

Anime4K& Anime4K::operator=(Anime4K&&) noexcept = default;

bool Anime4K::available() const
{
    return impl_->ready;
}

SDL_Texture* Anime4K::art_target() const
{
    return impl_->art.get();
}

SDL_Texture* Anime4K::overlay_target()
{
    impl_->ensure_overlay();
    return impl_->overlay.get();
}

SDL_Texture* Anime4K::authentic_text_target() const
{
    return impl_->authentic_text.get();
}

SDL_Texture* Anime4K::sidebar_target()
{
    impl_->ensure_overlay();
    return impl_->sidebar.get();
}

void Anime4K::present()
{
    impl_->present();
}

void Anime4K::reset()
{
    impl_->reset();
}

}  // namespace th2
