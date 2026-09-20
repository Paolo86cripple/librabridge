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

// AGS's GL loader targets an older GL and does not define these (GL 3.0 tokens).
#ifndef GL_MAJOR_VERSION
#define GL_MAJOR_VERSION 0x821B
#endif
#ifndef GL_MINOR_VERSION
#define GL_MINOR_VERSION 0x821C
#endif

// Highest GLSL version the current GL context supports (GL 3.3 -> 330,
// GL 4.x -> 4x0). librashader compiles every shader for this version, and
// some presets need more than 330 (e.g. packUnorm4x8() is GLSL 4.00+).
// Queried through the loader because this runs right after context creation,
// possibly before the engine has loaded its own GL function pointers.
static uint16_t DetectGlslVersion(GLProcLoader loader)
{
    typedef void (*GetIntegervFn)(unsigned int, int *);
    GetIntegervFn get_integerv =
        loader ? reinterpret_cast<GetIntegervFn>(loader("glGetIntegerv")) : nullptr;
    if (!get_integerv)
        return 330;
    int major = 0, minor = 0;
    get_integerv(GL_MAJOR_VERSION, &major);
    get_integerv(GL_MINOR_VERSION, &minor);
    if (major < 3 || (major == 3 && minor < 3))
        return 330; // librashader's minimum
    if (major > 4 || (major == 4 && minor > 6))
        return 460;
    return static_cast<uint16_t>(major * 100 + minor * 10);
}

bool LibrashaderGL::Init(const std::string &preset_path, GLProcLoader loader)
{
    Shutdown();

    const libra_instance_t &libra = GetLibra();
    if (!libra.instance_loaded)
    {
        _lastError = "librashader.so not found, or its ABI does not match this build "
                      "(searched via LD_LIBRARY_PATH and the system library path)";
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
    opts.glsl_version = DetectGlslVersion(loader);
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
    Debug::Printf(kDbgMsg_Info, "librashader: loaded preset '%s' (GLSL %u)",
        preset_path.c_str(), static_cast<unsigned>(opts.glsl_version));
    return true;
}

bool LibrashaderGL::Frame(uint64_t frame_count,
                           uint32_t in_tex, uint32_t in_w, uint32_t in_h,
                           uint32_t out_tex, uint32_t out_w, uint32_t out_h)
{
    if (!_chain)
        return false;

    const libra_instance_t &libra = GetLibra();

    // librashader samples the input frame through sampler objects whose
    // minification filter may be a mipmapped one. AGS creates its textures
    // with glTexImage2D and a single level, and the default
    // GL_TEXTURE_MAX_LEVEL (1000) then makes the texture mipmap-incomplete:
    // it samples as black and the whole chain outputs black. Restrict the
    // texture to level 0 (it never has any other level anyway).
    {
        GLint prev_active = 0, prev_tex = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
        glBindTexture(GL_TEXTURE_2D, in_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
        glActiveTexture(static_cast<GLenum>(prev_active));
    }

    libra_image_gl_t image{};
    image.handle = in_tex;
    // Must be a SIZED internal format: for presets that keep frame history,
    // librashader hands it to glTexStorage2D, which rejects the unsized
    // GL_RGBA (the frame then fails with an incomplete framebuffer).
    // AGS's own render-target textures are RGBA8.
    image.format = GL_RGBA8;
    image.width = in_w;
    image.height = in_h;

    libra_image_gl_t out{};
    out.handle = out_tex;
    out.format = GL_RGBA8;
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
