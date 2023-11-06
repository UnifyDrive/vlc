/*****************************************************************************
 * gl_util.h
 *****************************************************************************
 * Copyright (C) 2020 VLC authors and VideoLAN
 * Copyright (C) 2020 Videolabs
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifndef VLC_GL_UTIL_H
#define VLC_GL_UTIL_H

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_opengl.h>
#include "converter.h"
#include <assert.h>

#ifndef GL_RED
# define GL_RED 0x1903
#endif
#ifndef GL_RG
# define GL_RG 0x8227
#endif
#ifndef GL_R16
# define GL_R16 0x822A
#endif
#ifndef GL_R16UI
# define GL_R16UI 0x8234
#endif
#ifndef GL_BGRA
# define GL_BGRA 0x80E1
#endif
#ifndef GL_RG16
# define GL_RG16 0x822C
#endif
#ifndef GL_RGBA8
# define GL_RGBA8 0x8058
#endif
#ifndef GL_LUMINANCE16
# define GL_LUMINANCE16 0x8042
#endif
#ifndef GL_LUMINANCE16_ALPHA16
# define GL_LUMINANCE16_ALPHA16 0x8048
#endif
#ifndef GL_R8
# define GL_R8 0x8229
#endif
#ifndef GL_RG8
# define GL_RG8 0x822B
#endif
#ifndef GL_RG_INTEGER
# define GL_RG_INTEGER 0x8228
#endif
#ifndef GL_RED_INTEGER
# define GL_RED_INTEGER 0x8D94
#endif
#ifndef GL_RG16UI
# define GL_RG16UI 0x823A
#endif
#ifndef GL_TEXTURE_RED_SIZE
# define GL_TEXTURE_RED_SIZE 0x805C
#endif

#ifndef GL_TEXTURE_LUMINANCE_SIZE
# define GL_TEXTURE_LUMINANCE_SIZE 0x8060
#endif

#ifndef GL_UNPACK_ROW_LENGTH
# define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER
# define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif

#if !defined(GL_MAJOR_VERSION)
# define GL_MAJOR_VERSION 0x821B
#endif

#if !defined(GL_NUM_EXTENSIONS)
# define GL_NUM_EXTENSIONS 0x821D
#endif

#ifndef NDEBUG
# define HAVE_GL_ASSERT_NOERROR
#endif

#ifdef HAVE_GL_ASSERT_NOERROR
# define GL_ASSERT_NOERROR(vt) do { \
    GLenum glError = (vt)->GetError(); \
    switch (glError) \
    { \
        case GL_NO_ERROR: break; \
        case GL_INVALID_ENUM: assert(!"GL_INVALID_ENUM"); \
        case GL_INVALID_VALUE: assert(!"GL_INVALID_VALUE"); \
        case GL_INVALID_OPERATION: assert(!"GL_INVALID_OPERATION"); \
        case GL_INVALID_FRAMEBUFFER_OPERATION: assert(!"GL_INVALID_FRAMEBUFFER_OPERATION"); \
        case GL_OUT_OF_MEMORY: assert(!"GL_OUT_OF_MEMORY"); \
        default: assert(!"GL_UNKNOWN_ERROR"); \
    } \
} while(0)
#else
# define GL_ASSERT_NOERROR(vt)
#endif

static const float MATRIX4_IDENTITY[4*4] = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1,
};

static const float MATRIX3_IDENTITY[3*3] = {
    1, 0, 0,
    0, 1, 0,
    0, 0, 1,
};

/* In column-major order */
static const float MATRIX2x3_IDENTITY[2*3] = {
    1, 0,
    0, 1,
    0, 0,
};

VLC_USED static inline int vlc_clzll(unsigned long long x)
{
    int i = sizeof (x) * 8;

    while (x)
    {
        x >>= 1;
        i--;
    }
    return i;
}

VLC_USED static inline int vlc_clzl(unsigned long x)
{
    return vlc_clzll(x) - ((sizeof (long long) - sizeof (long)) * 8);
}

VLC_USED static inline int vlc_clz(unsigned x)
{
    return vlc_clzll(x) - ((sizeof (long long) - sizeof (int)) * 8);
}

/** Return the smallest larger or equal power of 2 */
static inline unsigned vlc_align_pot(unsigned x)
{
    unsigned align = 1 << (8 * sizeof (unsigned) - vlc_clz(x));
    return ((align >> 1) == x) ? x : align;
}

/**
 * Build an OpenGL program
 *
 * Both the fragment shader and fragment shader are passed as a list of
 * strings, forming the shader source code once concatenated, like
 * glShaderSource().
 *
 * \param obj a VLC object, used to log messages
 * \param vt the OpenGL virtual table
 * \param vstring_count the number of strings in vstrings
 * \param vstrings a list of NUL-terminated strings containing the vertex
 *                 shader source code
 * \param fstring_count the number of strings in fstrings
 * \param fstrings a list of NUL-terminated strings containing the fragment
 *                 shader source code
 */
GLuint
vlc_gl_BuildProgram(vlc_object_t *obj, const opengl_vtable_t *vt,
                    GLsizei vstring_count, const GLchar **vstrings,
                    GLsizei fstring_count, const GLchar **fstrings);

/**
 * Wrap an OpenGL filter from a video filter
 *
 * Open an OpenGL filter (with capability "opengl filter") from a video filter
 * (with capability "video filter").
 *
 * This internally uses the "opengl" video filter to load the OpenGL filter
 * with the given name.
 */
module_t *
vlc_gl_WrapOpenGLFilter(filter_t *filter, const char *opengl_filter_name);

struct vlc_gl_extension_vt {
    PFNGLGETSTRINGPROC      GetString;
    //PFNGLGETSTRINGIPROC     GetStringi;
    PFNGLGETINTEGERVPROC    GetIntegerv;
    PFNGLGETERRORPROC       GetError;
};

static inline unsigned
vlc_gl_GetVersionMajor(struct vlc_gl_extension_vt *vt)
{
    GLint version;
    vt->GetIntegerv(GL_MAJOR_VERSION, &version);
    uint32_t error = vt->GetError();

    if (error != GL_NO_ERROR)
        version = 2;

    /* Drain the errors before continuing. */
    while (error != GL_NO_ERROR)
        error = vt->GetError();

    return version;
}


static inline void
vlc_gl_LoadExtensionFunctions(vlc_gl_t *gl, struct vlc_gl_extension_vt *vt)
{
    vt->GetString = vlc_gl_GetProcAddress(gl, "glGetString");
    vt->GetIntegerv = vlc_gl_GetProcAddress(gl, "glGetIntegerv");
    vt->GetError = vlc_gl_GetProcAddress(gl, "glGetError");
    //vt->GetStringi = NULL;

    //unsigned version = vlc_gl_GetVersionMajor(vt);

    /* glGetStringi is available in OpenGL>=3 and GLES>=3.
     * https://www.khronos.org/registry/OpenGL-Refpages/gl4/html/glGetString.xhtml
     * https://www.khronos.org/registry/OpenGL-Refpages/es3/html/glGetString.xhtml
     */
    //if (version >= 3)
    //    vt->GetStringi = vlc_gl_GetProcAddress(gl, "glGetStringi");
}
static inline bool vlc_gl_StrHasToken(const char *apis, const char *api)
{
    size_t apilen = strlen(api);
    while (apis) {
        while (*apis == ' ')
            apis++;
        if (!strncmp(apis, api, apilen) && memchr(" ", apis[apilen], 2))
            return true;
        apis = strchr(apis, ' ');
    }
    return false;
}
static inline bool
vlc_gl_HasExtension(
    struct vlc_gl_extension_vt *vt,
    const char *name
){
    (void*)vt;
    (void*)name;
    return false;
#if 0
    if (vt->GetStringi == NULL)
    {
        const GLubyte *extensions = vt->GetString(GL_EXTENSIONS);
        return vlc_gl_StrHasToken((const char *)extensions, name);
    }

    int32_t count = 0;
    vt->GetIntegerv(GL_NUM_EXTENSIONS, &count);

    for (int i = 0; i < count; ++i)
    {
        const uint8_t *extension = vt->GetStringi(GL_EXTENSIONS, i);
        if (strcmp((const char *)extension, name) == 0)
            return true;
    }
#endif
    return false;
}

#endif
