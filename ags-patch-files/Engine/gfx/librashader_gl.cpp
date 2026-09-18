//=============================================================================
//
// Adventure Game Studio (AGS)
//
// See librashader_gl.h for an overview. This file contains no shader logic;
// it only calls into librashader's own C API (vendored, unmodified, under
// Engine/gfx/librashader/, MIT licensed) via its dlopen-based loader.
//
//=============================================================================
#include "gfx/librashader_gl.h"
#include "gfx/ogl_headers.h"

#define LIBRA_RUNTIME_OPENGL
#include "gfx/librashader/librashader_ld.h"

#include "debug/out.h"

namespace AGS
{
namespace Engine
{
namespace OGL
{

using namespace AGS::Common;

// One process-wide instance of the loaded function table. librashader.so is
// dlopen'd once on first use and never unloaded (matches how other runtime
// libraries, e.g. SDL2, glad, are treated elsewhere in the engine).
static libra_instance_t &GetLibra()
{
    static libra_instance_t s_instance = librashader_load_instance();
    return s_instance;
}

// glow (librashader's GL loader crate) expects a loader with this exact
// C calling signature; SDL_GL_GetProcAddress already matches it, so the
// wrapper just needs to satisfy the extern "C" linkage librashader expects.
struct GLLoaderThunk
{
    static GLProcLoader s_loader;
    static const void *Load(const char *name)
    {
        return s_loader ? reinterpret_cast<const void *>(s_loader(name)) : nullptr;
    }
};
GLProcLoader GLLoaderThunk::s_loader = nullptr;

bool LibrashaderGL::Init(const std::string &preset_path, GLProcLoader loader)
{
    Shutdown();

    const libra_instance_t &libra = GetLibra();
    if (!libra.instance_loaded)
    {
        _lastError = "librashader.so not found, or its ABI does not match this build "
                      "(checked next to the engine binary and the system library path)";
        return false;
    }

    libra_shader_preset_t preset = nullptr;
    libra_error_t err = libra.preset_create(preset_path.c_str(), &preset);
    if (err != nullptr || preset == nullptr)
    {
        _lastError = "failed to parse shader preset: " + preset_path;
        if (err) { libra.error_print(err); libra.error_free(&err); }
        return false;
    }

    GLLoaderThunk::s_loader = loader;

    filter_chain_gl_opt_t opts{};
    opts.version = LIBRASHADER_CURRENT_VERSION; // per librashader.h: always set to the current version
    opts.glsl_version = 330;
    opts.use_dsa = false;      // keep to GL 3.3, don't require 4.5
    opts.force_no_mipmaps = false;
    opts.disable_cache = false;

    libra_gl_filter_chain_t chain = nullptr;
    err = libra.gl_filter_chain_create(&preset, &GLLoaderThunk::Load, &opts, &chain);
    if (err != nullptr || chain == nullptr)
    {
        _lastError = "failed to build librashader filter chain for: " + preset_path;
        if (err) { libra.error_print(err); libra.error_free(&err); }
        return false;
    }

    _chain = chain;
    Debug::Printf(kDbgMsg_Info, "librashader: loaded preset '%s'", preset_path.c_str());
    return true;
}

bool LibrashaderGL::Frame(uint64_t frame_count,
                           uint32_t in_tex, uint32_t in_w, uint32_t in_h,
                           uint32_t out_tex, uint32_t out_w, uint32_t out_h)
{
    if (!_chain)
        return false;

    const libra_instance_t &libra = GetLibra();

    libra_image_gl_t image{};
    image.handle = in_tex;
    image.format = GL_RGBA; // matches AGS's own render-target texture format
    image.width = in_w;
    image.height = in_h;

    libra_image_gl_t out{};
    out.handle = out_tex;
    out.format = GL_RGBA;
    out.width = out_w;
    out.height = out_h;

    auto chain = static_cast<libra_gl_filter_chain_t>(_chain);
    libra_error_t err = libra.gl_filter_chain_frame(
        &chain, static_cast<size_t>(frame_count), image, out,
        /*viewport*/ nullptr, /*mvp*/ nullptr, /*opt*/ nullptr);

    if (err != nullptr)
    {
        libra.error_print(err);
        libra.error_free(&err);
        return false;
    }
    return true;
}

void LibrashaderGL::Shutdown()
{
    if (_chain)
    {
        const libra_instance_t &libra = GetLibra();
        auto chain = static_cast<libra_gl_filter_chain_t>(_chain);
        libra.gl_filter_chain_free(&chain);
        _chain = nullptr;
    }
}

} // namespace OGL
} // namespace Engine
} // namespace AGS
