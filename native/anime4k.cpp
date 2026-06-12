#include "anime4k.hpp"

#include <fstream>
#include <stdexcept>
#include <vector>

namespace th2 {
namespace {

struct TextureDeleter {
    void operator()(SDL_Texture* texture) const { SDL_DestroyTexture(texture); }
};

std::vector<std::uint8_t> read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open shader: " + path.string());
    }
    return {std::istreambuf_iterator<char>(input), {}};
}

}  // namespace

struct Anime4K::Impl {
    SDL_Renderer* renderer;
    SDL_GPUDevice* device = nullptr;
    std::unique_ptr<SDL_Texture, TextureDeleter> art;
    std::unique_ptr<SDL_Texture, TextureDeleter> overlay;
    int overlay_width = 0;
    int overlay_height = 0;
    std::vector<SDL_GPUShader*> shaders;
    std::vector<SDL_GPURenderState*> states;
    bool ready = false;

    SDL_GPUShader* load_shader(const std::filesystem::path& path,
                               int samplers)
    {
        const auto code = read_file(path);
        SDL_GPUShaderCreateInfo info{};
        info.code = code.data();
        info.code_size = code.size();
        info.entrypoint = "main";
        info.format = SDL_GPU_SHADERFORMAT_SPIRV;
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

    Impl(SDL_Renderer* renderer_, const std::filesystem::path& shader_dir)
        : renderer(renderer_)
    {
        device = SDL_GetGPURendererDevice(renderer);
        if (!device
            || !(SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_SPIRV)) {
            return;
        }
        auto target = [&](int width, int height) {
            auto texture = std::unique_ptr<SDL_Texture, TextureDeleter>(
                SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                  SDL_TEXTUREACCESS_TARGET, width, height));
            if (!texture) {
                throw std::runtime_error(SDL_GetError());
            }
            SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(texture.get(), SDL_SCALEMODE_LINEAR);
            return texture;
        };
        art = target(800, 600);
        make_state(load_shader(shader_dir / "apply.frag.spv", 1));
        ready = true;
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

    void ensure_overlay()
    {
        int width = 0;
        int height = 0;
        if (!SDL_GetRenderOutputSize(renderer, &width, &height)) {
            throw std::runtime_error(SDL_GetError());
        }
        if (overlay && width == overlay_width && height == overlay_height) {
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
        overlay_width = width;
        overlay_height = height;
    }

    void present()
    {
        ensure_overlay();
        SDL_SetRenderTarget(renderer, nullptr);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_SetGPURenderState(renderer, states[0]);
        SDL_RenderTexture(renderer, art.get(), nullptr, nullptr);
        SDL_SetGPURenderState(renderer, nullptr);
        SDL_SetGPURenderState(renderer, nullptr);
        SDL_RenderTexture(renderer, overlay.get(), nullptr, nullptr);
    }
};

Anime4K::Anime4K(
    SDL_Renderer* renderer, const std::filesystem::path& shader_dir)
    : impl_(std::make_unique<Impl>(renderer, shader_dir))
{
}

Anime4K::~Anime4K() = default;

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

void Anime4K::present()
{
    impl_->present();
}

}  // namespace th2
