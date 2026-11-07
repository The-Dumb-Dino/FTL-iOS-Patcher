// gltex.c
//
// iOS only: textures bigger than the GPU takes. Mods made for the PC game have
// animation strips up to 19300 pixels wide (Multiverse's hangar animation is
// 11500); iOS GPUs take less, the upload fails, and the game draws the texture as
// a black box. The game finds its GL functions by name (dlsym, as on the Mac), so
// this hands it wrappers for the texture uploads: a texture over the limit is
// scaled down to fit, and later updates of it are scaled the same way. The game
// addresses textures in fractions of their size, so everything still lines up;
// only those textures get softer.

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLES_SILENCE_DEPRECATION
#include <OpenGLES/ES3/gl.h>
#include <OpenGLES/ES3/glext.h>

// Registers a replacement function with dyld's interposing mechanism.
#define DYLD_INTERPOSE(_replacement, _replacee)                                                                        \
    __attribute__((used)) static struct                                                                                \
    {                                                                                                                  \
        const void *replacement;                                                                                       \
        const void *replacee;                                                                                          \
    } _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = {                                        \
        (const void *)(unsigned long)&_replacement, (const void *)(unsigned long)&_replacee};

static GLint gMax;  // GL_MAX_TEXTURE_SIZE, once a context is current

// Textures that were scaled down, by name, and how much (1 / factor)
#define MAX_SCALED 512
static struct
{
    GLuint tex;
    int factor;
} gScaled[MAX_SCALED];
static int gNscaled;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;

static GLint MaxSize(void)
{
    if (!gMax)
    {
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gMax);
        if (gMax > 0) fprintf(stderr, "[ftlios] largest texture: %d pixels\n", gMax);
    }
    return gMax > 0 ? gMax : 4096;
}

static GLuint BoundTexture(void)
{
    GLint t = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &t);
    return (GLuint)t;
}

static int FactorOf(GLuint tex)
{
    pthread_mutex_lock(&gLock);
    int f = 1;
    for (int i = 0; i < gNscaled; i++)
        if (gScaled[i].tex == tex) f = gScaled[i].factor;
    pthread_mutex_unlock(&gLock);
    return f;
}

static void SetFactor(GLuint tex, int f)
{
    pthread_mutex_lock(&gLock);
    int i = 0;
    while (i < gNscaled && gScaled[i].tex != tex) i++;
    if (i == gNscaled && f > 1 && gNscaled < MAX_SCALED) gNscaled++;
    if (i < gNscaled)
    {
        if (f > 1)
        {
            gScaled[i].tex = tex;
            gScaled[i].factor = f;
        }
        else
        {
            gScaled[i] = gScaled[--gNscaled];  // a new image at full size: forget it
        }
    }
    pthread_mutex_unlock(&gLock);
}

static int BytesPerPixel(GLenum format, GLenum type)
{
    if (type != GL_UNSIGNED_BYTE) return 0;
    switch (format)
    {
        case GL_RGBA:
        case GL_BGRA_EXT: return 4;
        case GL_RGB: return 3;
        case GL_LUMINANCE_ALPHA: return 2;
        case GL_ALPHA:
        case GL_LUMINANCE: return 1;
        default: return 0;
    }
}

// Averages f x f blocks; edge blocks average what they have.
static void *Shrink(const uint8_t *src, int w, int h, int bpp, int f, int *nw, int *nh)
{
    *nw = (w + f - 1) / f;
    *nh = (h + f - 1) / f;
    uint8_t *dst = malloc((size_t)*nw * *nh * bpp);
    if (!dst) return NULL;
    // GL's default unpack alignment is 4: rows of other widths are padded
    size_t stride = ((size_t)w * bpp + 3) & ~(size_t)3;
    for (int y = 0; y < *nh; y++)
        for (int x = 0; x < *nw; x++)
        {
            unsigned sum[4] = {0}, n = 0;
            for (int dy = 0; dy < f && y * f + dy < h; dy++)
                for (int dx = 0; dx < f && x * f + dx < w; dx++, n++)
                {
                    const uint8_t *p = src + (size_t)(y * f + dy) * stride + (size_t)(x * f + dx) * bpp;
                    for (int c = 0; c < bpp; c++) sum[c] += p[c];
                }
            uint8_t *q = dst + ((size_t)y * *nw + x) * bpp;
            for (int c = 0; c < bpp; c++) q[c] = (uint8_t)(sum[c] / n);
        }
    return dst;
}

static void TexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum format,
                       GLenum type, const void *pixels)
{
    if (target == GL_TEXTURE_2D)
    {
        GLint max = MaxSize();
        GLuint tex = BoundTexture();
        int f = level == 0 ? 1 : FactorOf(tex);
        if (level == 0)
        {
            while ((w + f - 1) / f > max || (h + f - 1) / f > max) f++;
            SetFactor(tex, f);
        }
        int bpp = BytesPerPixel(format, type);
        if (f > 1 && (bpp || !pixels))
        {
            int nw = (w + f - 1) / f, nh = (h + f - 1) / f;
            void *small = pixels ? Shrink(pixels, w, h, bpp, f, &nw, &nh) : NULL;
            if (level == 0) fprintf(stderr, "[ftlios] texture %ux%u is over %d: scaled to %dx%d\n", w, h, max, nw, nh);
            GLint align;
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(target, level, ifmt, nw, nh, border, format, type, small);
            glPixelStorei(GL_UNPACK_ALIGNMENT, align);
            free(small);
            return;
        }
    }
    glTexImage2D(target, level, ifmt, w, h, border, format, type, pixels);
}

static void TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                          GLenum type, const void *pixels)
{
    int f = target == GL_TEXTURE_2D ? FactorOf(BoundTexture()) : 1;
    int bpp = BytesPerPixel(format, type);
    if (f > 1 && bpp && pixels)
    {
        int nw, nh;
        void *small = Shrink(pixels, w, h, bpp, f, &nw, &nh);
        GLint align;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(target, level, x / f, y / f, nw, nh, format, type, small);
        glPixelStorei(GL_UNPACK_ALIGNMENT, align);
        free(small);
        return;
    }
    glTexSubImage2D(target, level, x, y, w, h, format, type, pixels);
}

static void TexStorage2D(GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h)
{
    if (target == GL_TEXTURE_2D)
    {
        GLint max = MaxSize();
        int f = 1;
        while ((w + f - 1) / f > max || (h + f - 1) / f > max) f++;
        SetFactor(BoundTexture(), f);
        if (f > 1)
        {
            fprintf(stderr, "[ftlios] texture %ux%u is over %d: scaled down %dx\n", w, h, max, f);
            w = (w + f - 1) / f;
            h = (h + f - 1) / f;
        }
    }
    glTexStorage2D(target, levels, ifmt, w, h);
}

static void *MyDlsym(void *handle, const char *name)
{
    if (name && name[0] == 'g' && name[1] == 'l')
    {
        if (!strcmp(name, "glTexImage2D")) return (void *)TexImage2D;
        if (!strcmp(name, "glTexSubImage2D")) return (void *)TexSubImage2D;
        if (!strcmp(name, "glTexStorage2D")) return (void *)TexStorage2D;
    }
    return dlsym(handle, name);
}
DYLD_INTERPOSE(MyDlsym, dlsym)
