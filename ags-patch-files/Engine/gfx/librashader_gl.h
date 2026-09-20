//=============================================================================
//
// Adventure Game Studio (AGS)
//
// librashader bridge for the native OpenGL renderer.
//
// This class does NOT implement any shader/preset/pass logic itself.
// All of that (parsing .slangp/.glslp presets, compiling passes, feedback,
// history, LUTs, parameters, ...) is handled entirely by librashader
// (https://github.com/SnowflakePowered/librashader), loaded dynamically at
// runtime via its official librashader_ld.h loader (dlopen-based, no link
// step required). This file only wires that library into two points of
// AGS's existing OpenGL pipeline: it feeds the frame AGS already renders at
// native game resolution into librashader's filter chain, and hands back a
// texture for AGS to present with its own existing blit code.
//
//=============================================================================
#ifndef __AGS_EE_GFX__LIBRASHADER_GL_H
#define __AGS_EE_GFX__LIBRASHADER_GL_H

#include <cstdint>
#include <string>

namespace AGS
{
namespace Engine
{
namespace OGL
{

// GL function loader signature matching SDL_GL_GetProcAddress.
typedef void *(*GLProcLoader)(const char *name);

// Thin RAII wrapper around a single librashader OpenGL filter chain.
// One instance == one loaded .slangp/.glslp preset.
class LibrashaderGL
{
public:
    ~LibrashaderGL() { Shutdown(); }

    // Loads librashader.so (dlopen by bare name: found via LD_LIBRARY_PATH or
    // the system loader path, not next to the binary), then loads and compiles
    // the preset at preset_path. Returns false (and leaves the object inactive)
    // on any failure; AGS falls back to normal unshaded rendering in that case.
    bool Init(const std::string &preset_path, GLProcLoader loader);

    bool IsActive() const { return _chain != nullptr; }

    // Runs the filter chain: reads from in_tex (in_w x in_h, the game's
    // native-resolution frame) and writes into out_tex (out_w x out_h,
    // already bound to its own framebuffer by the caller via a normal
    // render-target DDB). Caller keeps ownership of both textures.
    bool Frame(uint64_t frame_count,
               uint32_t in_tex, uint32_t in_w, uint32_t in_h,
               uint32_t out_tex, uint32_t out_w, uint32_t out_h);

    void Shutdown();

    // Human-readable reason the last Init()/Frame() call failed, for logging.
    const std::string &LastError() const { return _lastError; }

private:
    void *_chain = nullptr;      // libra_gl_filter_chain_t, opaque here to avoid pulling librashader.h into this header
    void *_libHandle = nullptr;  // dlopen handle for librashader.so
    std::string _lastError;
};

} // namespace OGL
} // namespace Engine
} // namespace AGS

#endif // __AGS_EE_GFX__LIBRASHADER_GL_H
