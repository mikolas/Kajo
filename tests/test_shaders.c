#include <glib.h>
#include <gio/gio.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <string.h>

/* Exact Niri GLESv2 vertex shader and fragment wrappers from niri/src/render_helpers/shaders/ */

static const char *NIRI_VERTEX_SHADER =
    "precision highp float;\n"
    "attribute vec2 position;\n"
    "varying vec2 niri_v_coords;\n"
    "void main() {\n"
    "    niri_v_coords = position;\n"
    "    gl_Position = vec4(position, 0.0, 1.0);\n"
    "}\n";

static const char *NIRI_OPEN_PRELUDE =
    "precision highp float;\n"
    "varying vec2 niri_v_coords;\n"
    "uniform vec2 niri_size;\n"
    "uniform mat3 niri_input_to_geo;\n"
    "uniform vec2 niri_geo_size;\n"
    "uniform sampler2D niri_tex;\n"
    "uniform mat3 niri_geo_to_tex;\n"
    "uniform float niri_progress;\n"
    "uniform float niri_clamped_progress;\n"
    "uniform float niri_random_seed;\n"
    "uniform float niri_alpha;\n"
    "uniform float niri_scale;\n\n";

static const char *NIRI_OPEN_EPILOGUE =
    "\nvoid main() {\n"
    "    vec3 coords_geo = niri_input_to_geo * vec3(niri_v_coords, 1.0);\n"
    "    vec3 size_geo = vec3(niri_geo_size, 1.0);\n"
    "    vec4 color = open_color(coords_geo, size_geo);\n"
    "    color = color * niri_alpha;\n"
    "    gl_FragColor = color;\n"
    "}\n";

static const char *NIRI_CLOSE_PRELUDE =
    "precision highp float;\n"
    "varying vec2 niri_v_coords;\n"
    "uniform vec2 niri_size;\n"
    "uniform mat3 niri_input_to_geo;\n"
    "uniform vec2 niri_geo_size;\n"
    "uniform sampler2D niri_tex;\n"
    "uniform mat3 niri_geo_to_tex;\n"
    "uniform float niri_progress;\n"
    "uniform float niri_clamped_progress;\n"
    "uniform float niri_random_seed;\n"
    "uniform float niri_alpha;\n"
    "uniform float niri_scale;\n\n";

static const char *NIRI_CLOSE_EPILOGUE =
    "\nvoid main() {\n"
    "    vec3 coords_geo = niri_input_to_geo * vec3(niri_v_coords, 1.0);\n"
    "    vec3 size_geo = vec3(niri_geo_size, 1.0);\n"
    "    vec4 color = close_color(coords_geo, size_geo);\n"
    "    color = color * niri_alpha;\n"
    "    gl_FragColor = color;\n"
    "}\n";

static const char *NIRI_RESIZE_PRELUDE =
    "precision highp float;\n"
    "varying vec2 niri_v_coords;\n"
    "uniform vec2 niri_size;\n"
    "uniform mat3 niri_input_to_curr_geo;\n"
    "uniform mat3 niri_curr_geo_to_prev_geo;\n"
    "uniform mat3 niri_curr_geo_to_next_geo;\n"
    "uniform vec2 niri_curr_geo_size;\n"
    "uniform sampler2D niri_tex_prev;\n"
    "uniform mat3 niri_geo_to_tex_prev;\n"
    "uniform sampler2D niri_tex_next;\n"
    "uniform mat3 niri_geo_to_tex_next;\n"
    "uniform float niri_progress;\n"
    "uniform float niri_clamped_progress;\n"
    "uniform vec4 niri_corner_radius;\n"
    "uniform float niri_clip_to_geometry;\n"
    "uniform float niri_alpha;\n"
    "uniform float niri_scale;\n"
    "float niri_rounding_alpha(vec2 coords, vec2 size, vec4 corner_radius);\n\n";

static const char *NIRI_RESIZE_EPILOGUE =
    "\nvoid main() {\n"
    "    vec3 coords_curr_geo = niri_input_to_curr_geo * vec3(niri_v_coords, 1.0);\n"
    "    vec3 size_curr_geo = vec3(niri_curr_geo_size, 1.0);\n"
    "    vec4 color = resize_color(coords_curr_geo, size_curr_geo);\n"
    "    if (niri_clip_to_geometry == 1.0) {\n"
    "        if (coords_curr_geo.x < 0.0 || 1.0 < coords_curr_geo.x\n"
    "                || coords_curr_geo.y < 0.0 || 1.0 < coords_curr_geo.y) {\n"
    "            color = vec4(0.0);\n"
    "        } else {\n"
    "            color = color * niri_rounding_alpha(coords_curr_geo.xy * size_curr_geo.xy, size_curr_geo.xy, niri_corner_radius);\n"
    "        }\n"
    "    }\n"
    "    color = color * niri_alpha;\n"
    "    gl_FragColor = color;\n"
    "}\n"
    "float niri_rounding_alpha(vec2 coords, vec2 size, vec4 corner_radius) {\n"
    "    return 1.0;\n"
    "}\n";

static gboolean
init_headless_egl(EGLDisplay *out_dpy, EGLContext *out_ctx)
{
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) {
        PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_dpy =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (get_platform_dpy)
            dpy = get_platform_dpy(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    }
    if (dpy == EGL_NO_DISPLAY)
        return FALSE;

    if (!eglInitialize(dpy, NULL, NULL))
        return FALSE;

    if (!eglBindAPI(EGL_OPENGL_ES_API))
        return FALSE;

    const EGLint cfg_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint num_cfg = 0;
    if (!eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &num_cfg) || num_cfg == 0)
        cfg = EGL_NO_CONFIG_KHR;

    const EGLint ctx_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (ctx == EGL_NO_CONTEXT)
        return FALSE;

    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
        return FALSE;

    *out_dpy = dpy;
    *out_ctx = ctx;
    return TRUE;
}

static void
verify_shader_file(const char *rel_path, const char *prelude, const char *epilogue)
{
    g_autofree gchar *full_path = g_build_filename(KAJO_SOURCE_DIR, rel_path, NULL);
    g_autofree gchar *body = NULL;
    GError *err = NULL;

    gboolean ok = g_file_get_contents(full_path, &body, NULL, &err);
    g_assert_no_error(err);
    g_assert_true(ok);
    g_assert_nonnull(body);

    g_autofree gchar *frag_src = g_strconcat(prelude, body, epilogue, NULL);

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &NIRI_VERTEX_SHADER, NULL);
    glCompileShader(vs);
    GLint vs_ok = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &vs_ok);
    g_assert_cmpint(vs_ok, ==, GL_TRUE);

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    const char *fs_ptr = frag_src;
    glShaderSource(fs, 1, &fs_ptr, NULL);
    glCompileShader(fs);

    GLint fs_ok = 0;
    glGetShaderiv(fs, GL_COMPILE_STATUS, &fs_ok);
    if (fs_ok != GL_TRUE) {
        char log[2048] = {0};
        glGetShaderInfoLog(fs, sizeof(log), NULL, log);
        g_error("GLSL compilation failed for %s:\n%s", rel_path, log);
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint link_ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &link_ok);
    if (link_ok != GL_TRUE) {
        char log[2048] = {0};
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        g_error("GLSL program link failed for %s:\n%s", rel_path, log);
    }

    glDeleteProgram(prog);
    glDeleteShader(fs);
    glDeleteShader(vs);
}

static void
test_all_niri_shaders(void)
{
    static const char *open_shaders[] = {
        "data/shaders/open/metro-flip.frag",
        "data/shaders/open/expanding-circle.frag",
        "data/shaders/open/crt-on.frag",
        "data/shaders/open/pixelate-in.frag",
        "data/shaders/open/glitch-in.frag",
        "data/shaders/open/curtain-open.frag",
    };
    static const char *close_shaders[] = {
        "data/shaders/close/fall-and-rotate.frag",
        "data/shaders/close/crt-off.frag",
        "data/shaders/close/ash-dissolve.frag",
        "data/shaders/close/vortex-implode.frag",
        "data/shaders/close/pixelate-out.frag",
        "data/shaders/close/slice-shatter.frag",
    };
    static const char *resize_shaders[] = {
        "data/shaders/resize/stretch-next.frag",
        "data/shaders/resize/pixelate-morph.frag",
        "data/shaders/resize/glitch-morph.frag",
    };

    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    gboolean has_egl = init_headless_egl(&dpy, &ctx);
    g_assert_true(has_egl);

    for (guint i = 0; i < G_N_ELEMENTS(open_shaders); i++)
        verify_shader_file(open_shaders[i], NIRI_OPEN_PRELUDE, NIRI_OPEN_EPILOGUE);

    for (guint i = 0; i < G_N_ELEMENTS(close_shaders); i++)
        verify_shader_file(close_shaders[i], NIRI_CLOSE_PRELUDE, NIRI_CLOSE_EPILOGUE);

    for (guint i = 0; i < G_N_ELEMENTS(resize_shaders); i++)
        verify_shader_file(resize_shaders[i], NIRI_RESIZE_PRELUDE, NIRI_RESIZE_EPILOGUE);

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/shaders/gles2-compile-and-link", test_all_niri_shaders);
    return g_test_run();
}
