/*
 * OpenGL backend of the drawing layer (OpenGL 2.1 / GLSL 1.20, no extensions).
 *
 * Every GL function is fetched through the host's loader (gfx_gl_init), so the
 * program does not link against an OpenGL library and starts even where OpenGL
 * is missing (the host then uses another backend).
 */
#include "gfx/gfx_gl.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#define GLAPI_CALL __stdcall
#else
#define GLAPI_CALL
#endif

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef char GLchar;

#define GL_TEXTURE_2D            0x0DE1
#define GL_TEXTURE_MIN_FILTER    0x2801
#define GL_TEXTURE_MAG_FILTER    0x2800
#define GL_TEXTURE_WRAP_S        0x2802
#define GL_TEXTURE_WRAP_T        0x2803
#define GL_NEAREST               0x2600
#define GL_LINEAR                0x2601
#define GL_CLAMP_TO_EDGE         0x812F
#define GL_RGBA                  0x1908
#define GL_RGBA8                 0x8058
#define GL_BGRA                  0x80E1
#define GL_UNSIGNED_BYTE         0x1401
#define GL_VERSION               0x1F02
#define GL_UNPACK_ROW_LENGTH     0x0CF2
#define GL_UNPACK_ALIGNMENT      0x0CF5
#define GL_COLOR_BUFFER_BIT      0x4000
#define GL_SCISSOR_TEST          0x0C11
#define GL_BLEND                 0x0BE2
#define GL_DEPTH_TEST            0x0B71
#define GL_CULL_FACE             0x0B44
#define GL_TRIANGLES             0x0004
#define GL_FLOAT                 0x1406
#define GL_FALSE                 0
#define GL_VERTEX_SHADER         0x8B31
#define GL_FRAGMENT_SHADER       0x8B30
#define GL_COMPILE_STATUS        0x8B81
#define GL_LINK_STATUS           0x8B82

#define GLF(ret, name, args) static ret (GLAPI_CALL *name) args
GLF(void, p_glGenTextures, (GLsizei, GLuint *));
GLF(void, p_glDeleteTextures, (GLsizei, const GLuint *));
GLF(void, p_glBindTexture, (GLenum, GLuint));
GLF(void, p_glTexParameteri, (GLenum, GLenum, GLint));
GLF(void, p_glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *));
GLF(void, p_glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *));
GLF(void, p_glPixelStorei, (GLenum, GLint));
GLF(void, p_glViewport, (GLint, GLint, GLsizei, GLsizei));
GLF(void, p_glScissor, (GLint, GLint, GLsizei, GLsizei));
GLF(void, p_glEnable, (GLenum));
GLF(void, p_glDisable, (GLenum));
GLF(void, p_glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat));
GLF(void, p_glClear, (GLbitfield));
GLF(void, p_glDrawArrays, (GLenum, GLint, GLsizei));
GLF(const unsigned char *, p_glGetString, (GLenum));
GLF(GLuint, p_glCreateShader, (GLenum));
GLF(void, p_glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *));
GLF(void, p_glCompileShader, (GLuint));
GLF(void, p_glGetShaderiv, (GLuint, GLenum, GLint *));
GLF(void, p_glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *));
GLF(GLuint, p_glCreateProgram, (void));
GLF(void, p_glAttachShader, (GLuint, GLuint));
GLF(void, p_glBindAttribLocation, (GLuint, GLuint, const GLchar *));
GLF(void, p_glLinkProgram, (GLuint));
GLF(void, p_glGetProgramiv, (GLuint, GLenum, GLint *));
GLF(void, p_glUseProgram, (GLuint));
GLF(GLint, p_glGetUniformLocation, (GLuint, const GLchar *));
GLF(void, p_glUniform1i, (GLint, GLint));
GLF(void, p_glUniform1f, (GLint, GLfloat));
GLF(void, p_glUniform2f, (GLint, GLfloat, GLfloat));
GLF(void, p_glEnableVertexAttribArray, (GLuint));
GLF(void, p_glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *));

struct GfxTex {
    GLuint id;
    int w, h;
    int smooth;          /* current filter */
};

#define MAX_QUADS 4096

static struct {
    GLuint prog;
    GLint u_scale, u_opaque;
    int target_w, target_h;
    GfxTex *tex;          /* batch state */
    unsigned batch_flags;
    float verts[MAX_QUADS * 6 * 4];
    int nverts;
    int es;               /* OpenGL ES context */
} gl;

/* No #version line: the same source compiles as desktop GLSL 1.10 and as
 * OpenGL ES GLSL 1.00 (SDL may hand us an ES context, e.g. on Linux with EGL). */
static const char *vs_src =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "uniform vec2 u_scale;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = vec4(a_pos * u_scale + vec2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n";

/* Pixels are 0xAARRGGBB, i.e. bytes B, G, R, A. Desktop GL takes them as BGRA;
 * OpenGL ES 2 has no BGRA upload, so there they arrive as RGBA and the shader
 * swaps red and blue (SWAP_RB). */
static const char *fs_src =
    "#ifdef GL_ES\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_opaque;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    vec4 c = texture2D(u_tex, v_uv);\n"
    "#ifdef SWAP_RB\n"
    "    c = c.bgra;\n"
    "#endif\n"
    "    if (u_opaque < 0.5 && c.a < 0.5) discard;\n"
    "    gl_FragColor = vec4(c.rgb, 1.0);\n"
    "}\n";

static GLuint compile(GLenum kind, const char *prefix, const char *src)
{
    GLuint s = p_glCreateShader(kind);
    GLint ok = 0;
    const char *parts[2];
    parts[0] = prefix;
    parts[1] = src;
    p_glShaderSource(s, 2, parts, NULL);
    p_glCompileShader(s);
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        p_glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "gfx_gl: shader: %s\n", log);
        return 0;
    }
    return s;
}

static void flush(void)
{
    if (!gl.nverts || !gl.tex) { gl.nverts = 0; return; }
    p_glBindTexture(GL_TEXTURE_2D, gl.tex->id);
    p_glUniform1f(gl.u_opaque, (gl.batch_flags & GFX_OPAQUE) ? 1.0f : 0.0f);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), gl.verts);
    p_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), gl.verts + 2);
    p_glDrawArrays(GL_TRIANGLES, 0, gl.nverts);
    gl.nverts = 0;
}

static GfxTex *tex_create(int w, int h)
{
    GfxTex *t = (GfxTex *)calloc(1, sizeof *t);
    if (!t) return NULL;
    flush();
    t->w = w;
    t->h = h;
    p_glGenTextures(1, &t->id);
    p_glBindTexture(GL_TEXTURE_2D, t->id);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (gl.es) p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    else p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    return t;
}

static void tex_update(GfxTex *t, int x, int y, int w, int h, const uint32_t *pixels, int pitch)
{
    if (t == gl.tex) flush();
    p_glBindTexture(GL_TEXTURE_2D, t->id);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (gl.es) {
        /* (bytes B, G, R, A arrive as R, G, B, A: the shader swaps them back) */
        if (pitch == w) p_glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        else {
            int row;   /* ES 2 cannot skip the rest of a row */
            for (row = 0; row < h; row++)
                p_glTexSubImage2D(GL_TEXTURE_2D, 0, x, y + row, w, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                  pixels + (size_t)row * (size_t)pitch);
        }
        return;
    }
    p_glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    p_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

static void tex_destroy(GfxTex *t)
{
    if (!t) return;
    if (t == gl.tex) { flush(); gl.tex = NULL; }
    p_glDeleteTextures(1, &t->id);
    free(t);
}

static void frame_begin(int w, int h)
{
    gl.target_w = w;
    gl.target_h = h;
    gl.tex = NULL;
    gl.nverts = 0;
    p_glViewport(0, 0, w, h);
    p_glDisable(GL_SCISSOR_TEST);
    p_glDisable(GL_BLEND);
    p_glDisable(GL_DEPTH_TEST);
    p_glDisable(GL_CULL_FACE);
    p_glClearColor(0, 0, 0, 1);
    p_glClear(GL_COLOR_BUFFER_BIT);
    p_glUseProgram(gl.prog);
    p_glUniform1i(p_glGetUniformLocation(gl.prog, "u_tex"), 0);
    p_glUniform2f(gl.u_scale, 2.0f / (float)w, -2.0f / (float)h);
    p_glEnableVertexAttribArray(0);
    p_glEnableVertexAttribArray(1);
}

static void clip(int x, int y, int w, int h)
{
    flush();
    if (w <= 0 || h <= 0) { p_glDisable(GL_SCISSOR_TEST); return; }
    p_glEnable(GL_SCISSOR_TEST);
    p_glScissor(x, gl.target_h - (y + h), w, h);   /* GL counts rows from the bottom */
}

static void quad(GfxTex *t, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh, unsigned flags)
{
    float u0, v0, u1, v1, x0 = (float)dx, y0 = (float)dy, x1 = (float)(dx + dw), y1 = (float)(dy + dh);
    float *v;
    int smooth = (flags & GFX_SMOOTH) != 0;
    if (!t) return;
    if (t != gl.tex || ((flags ^ gl.batch_flags) & GFX_OPAQUE) || smooth != t->smooth ||
        gl.nverts + 6 > MAX_QUADS * 6) {
        flush();
        gl.tex = t;
        gl.batch_flags = flags;
        if (smooth != t->smooth) {
            p_glBindTexture(GL_TEXTURE_2D, t->id);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
            p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
            t->smooth = smooth;
        }
    }
    u0 = (float)sx / (float)t->w;
    u1 = (float)(sx + sw) / (float)t->w;
    v0 = (float)sy / (float)t->h;
    v1 = (float)(sy + sh) / (float)t->h;
    if (flags & GFX_FLIP_X) { float k = u0; u0 = u1; u1 = k; }
    if (flags & GFX_FLIP_Y) { float k = v0; v0 = v1; v1 = k; }
    v = gl.verts + gl.nverts * 4;
#define VTX(X, Y, U, V) (*v++ = (X), *v++ = (Y), *v++ = (U), *v++ = (V))
    VTX(x0, y0, u0, v0); VTX(x1, y0, u1, v0); VTX(x0, y1, u0, v1);
    VTX(x1, y0, u1, v0); VTX(x1, y1, u1, v1); VTX(x0, y1, u0, v1);
#undef VTX
    gl.nverts += 6;
}

static void frame_end(void)
{
    flush();
}

static const GfxBackend backend = {
    "OpenGL", tex_create, tex_update, tex_destroy, frame_begin, clip, quad, frame_end
};

const GfxBackend *gfx_gl_init(void *(*getproc)(const char *name))
{
    GLuint vs, fs;
    GLint ok = 0;
#define LOAD(p, n) do { *(void **)&p = getproc(n); if (!p) { fprintf(stderr, "gfx_gl: no %s\n", n); return NULL; } } while (0)
    LOAD(p_glGenTextures, "glGenTextures");
    LOAD(p_glDeleteTextures, "glDeleteTextures");
    LOAD(p_glBindTexture, "glBindTexture");
    LOAD(p_glTexParameteri, "glTexParameteri");
    LOAD(p_glTexImage2D, "glTexImage2D");
    LOAD(p_glTexSubImage2D, "glTexSubImage2D");
    LOAD(p_glPixelStorei, "glPixelStorei");
    LOAD(p_glViewport, "glViewport");
    LOAD(p_glScissor, "glScissor");
    LOAD(p_glEnable, "glEnable");
    LOAD(p_glDisable, "glDisable");
    LOAD(p_glClearColor, "glClearColor");
    LOAD(p_glClear, "glClear");
    LOAD(p_glDrawArrays, "glDrawArrays");
    LOAD(p_glGetString, "glGetString");
    LOAD(p_glCreateShader, "glCreateShader");
    LOAD(p_glShaderSource, "glShaderSource");
    LOAD(p_glCompileShader, "glCompileShader");
    LOAD(p_glGetShaderiv, "glGetShaderiv");
    LOAD(p_glGetShaderInfoLog, "glGetShaderInfoLog");
    LOAD(p_glCreateProgram, "glCreateProgram");
    LOAD(p_glAttachShader, "glAttachShader");
    LOAD(p_glBindAttribLocation, "glBindAttribLocation");
    LOAD(p_glLinkProgram, "glLinkProgram");
    LOAD(p_glGetProgramiv, "glGetProgramiv");
    LOAD(p_glUseProgram, "glUseProgram");
    LOAD(p_glGetUniformLocation, "glGetUniformLocation");
    LOAD(p_glUniform1i, "glUniform1i");
    LOAD(p_glUniform1f, "glUniform1f");
    LOAD(p_glUniform2f, "glUniform2f");
    LOAD(p_glEnableVertexAttribArray, "glEnableVertexAttribArray");
    LOAD(p_glVertexAttribPointer, "glVertexAttribPointer");
#undef LOAD
    {
        const char *ver = (const char *)p_glGetString(GL_VERSION);
        gl.es = ver && strstr(ver, "OpenGL ES") != NULL;
    }
    vs = compile(GL_VERTEX_SHADER, "", vs_src);
    fs = compile(GL_FRAGMENT_SHADER, gl.es ? "#define SWAP_RB 1\n" : "", fs_src);
    if (!vs || !fs) return NULL;
    gl.prog = p_glCreateProgram();
    p_glAttachShader(gl.prog, vs);
    p_glAttachShader(gl.prog, fs);
    p_glBindAttribLocation(gl.prog, 0, "a_pos");
    p_glBindAttribLocation(gl.prog, 1, "a_uv");
    p_glLinkProgram(gl.prog);
    p_glGetProgramiv(gl.prog, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "gfx_gl: link failed\n"); return NULL; }
    gl.u_scale = p_glGetUniformLocation(gl.prog, "u_scale");
    gl.u_opaque = p_glGetUniformLocation(gl.prog, "u_opaque");
    return &backend;
}
