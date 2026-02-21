#include <gtk/gtk.h>
#include <epoxy/gl.h>

#include <libaudcore/plugin.h>
#include <libaudcore/i18n.h>

#include <projectM-4/projectM.h>
#include <projectM-4/render_opengl.h>
#include <projectM-4/audio.h>
#include <projectM-4/parameters.h>
#include <projectM-4/playlist.h>
#include <projectM-4/playlist_core.h>
#include <projectM-4/playlist_playback.h>
#include <projectM-4/playlist_callbacks.h>

#include <atomic>
#include <algorithm>
#include <mutex>
#include <vector>

#include "ringbuf.h"

namespace {

GtkWidget* gl_area = nullptr;

std::mutex pm_mutex;
projectm_handle pm = nullptr;
projectm_playlist_handle playlist = nullptr;

int fb_w = 0;
int fb_h = 0;
GLuint dummy_vao = 0;

std::atomic<bool> running{false};
guint tick_id = 0;

// ~2 seconds at 48kHz
StereoFloatRing audio_ring(48000 * 2);

// UI-thread temp buffer for draining audio
std::vector<float> drain_buf;

gint current_scale_factor(GtkWidget* w) {
    if (!w) return 1;
    return std::max(gtk_widget_get_scale_factor(w), 1);
}

void pm_destroy_locked() {
    if (playlist) {
        projectm_playlist_destroy(playlist);
        playlist = nullptr;
    }
    if (pm) {
        projectm_destroy(pm);
        pm = nullptr;
    }
}

static void on_preset_switched(bool hard_cut, unsigned int index, void*)
{
    g_message("projectM: preset switched -> index=%u hard_cut=%d",
              index, hard_cut ? 1 : 0);
}

static void on_preset_switch_failed(const char* preset_filename, const char* message, void*)
{
    g_warning("projectM: preset switch FAILED -> %s (%s)",
              preset_filename ? preset_filename : "(null)",
              message ? message : "(no message)");
}

static void init_playlist_locked()
{
    const char* preset_dir = nullptr;

    // projectM package layouts differ by distro/version; probe the common ones.
    const char* env_preset_dir = g_getenv("PROJECTM_PRESET_DIR");
    const char* candidates[] = {
        env_preset_dir,
        "/usr/share/projectM/presets",
        "/usr/local/share/projectM/presets",
        "/usr/share/projectM4/presets",
        "/usr/local/share/projectM4/presets",
        "/usr/share/projectm/presets",
        "/usr/local/share/projectm/presets",
    };

    for (auto* d : candidates) {
        if (d && d[0] != '\0' && g_file_test(d, G_FILE_TEST_IS_DIR)) {
            preset_dir = d;
            break;
        }
    }

    if (!preset_dir) {
        g_warning("projectM: no preset directory found; set PROJECTM_PRESET_DIR or install presets under /usr/share/projectM(4)/presets");
        return;
    }

    if (playlist) {
        projectm_playlist_destroy(playlist);
        playlist = nullptr;
    }

    playlist = projectm_playlist_create(pm);
    if (!playlist) {
        g_warning("projectM: projectm_playlist_create() failed");
        return;
    }

    projectm_playlist_set_preset_switched_event_callback(playlist, on_preset_switched, nullptr);
    projectm_playlist_set_preset_switch_failed_event_callback(playlist, on_preset_switch_failed, nullptr);


    uint32_t added = projectm_playlist_add_path(playlist, preset_dir, true, false);
    if (added == 0) {
        g_message("projectM: add_path('%s') -> added=%u", preset_dir, added);
        g_warning("projectM: no presets added from %s", preset_dir);
        return;
    }

    g_message("projectM: added %u presets from %s", added, preset_dir);

    // Set how long presets should run (you have this API)
    projectm_set_preset_duration(pm, 15.0);

    // IMPORTANT: select a preset so projectM has something to render
    uint32_t idx = projectm_playlist_play_next(playlist, true);
    g_message("projectM: play_next(hard_cut=true) -> idx=%u", idx);
    g_message("projectM: initial preset index=%u", idx);
}


bool pm_create_locked(int w, int h) {
    pm = projectm_create();
    if (!pm) {
        g_warning("projectM: projectm_create() failed (GL context not ready?)");
        return false;
    }
    fb_w = std::max(w, 1);
    fb_h = std::max(h, 1);

    projectm_set_window_size(pm, fb_w, fb_h);
    projectm_set_mesh_size(pm, 64, 48);
    glViewport(0, 0, fb_w, fb_h);
    init_playlist_locked();
    return true;
}


GdkGLContext* on_gl_create_context(GtkGLArea* area, gpointer)
{
    GError* error = nullptr;
    GdkWindow* window = gtk_widget_get_window(GTK_WIDGET(area));
    if (!window)
        return nullptr;

    GdkGLContext* context = gdk_window_create_gl_context(window, &error);
    if (!context) {
        g_warning("projectM: failed to create GL context: %s", error ? error->message : "unknown error");
        g_clear_error(&error);
        return nullptr;
    }

    // GtkGLArea requires >= 3.2; request desktop GL and allow compatibility.
    gdk_gl_context_set_use_es(context, FALSE);
    gdk_gl_context_set_required_version(context, 3, 2);
    gdk_gl_context_set_forward_compatible(context, FALSE);

    return context;
}

void on_gl_realize(GtkGLArea* area, gpointer) {
    g_message("projectM: GL realize");
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area)) {
        g_warning("projectM: GtkGLArea realize error");
        return;
    }
    if (!dummy_vao) {
        glGenVertexArrays(1, &dummy_vao);
    }
    glBindVertexArray(dummy_vao);

    int scale = current_scale_factor(GTK_WIDGET(area));
    int w = gtk_widget_get_allocated_width(GTK_WIDGET(area)) * scale;
    int h = gtk_widget_get_allocated_height(GTK_WIDGET(area)) * scale;

    std::lock_guard<std::mutex> lock(pm_mutex);
    pm_destroy_locked();
    pm_create_locked(w, h);
}

void on_gl_unrealize(GtkGLArea* area, gpointer) {
    gtk_gl_area_make_current(area);
    std::lock_guard<std::mutex> lock(pm_mutex);
    if (dummy_vao) {
        glDeleteVertexArrays(1, &dummy_vao);
        dummy_vao = 0;
    }
    pm_destroy_locked();

    (void)area;
}

static void drain_audio_into_projectm_locked() {
    if (!pm) return;

    constexpr size_t frames = 512;
    drain_buf.resize(frames * 2);

    size_t got = audio_ring.pop(drain_buf.data(), frames);
    static unsigned n = 0;
    if ((++n % 120) == 0) { // ~2 seconds at 60fps
        g_message("projectM: pop got=%zu frames", got);
    }
    if (got < frames) {
        std::fill(drain_buf.begin() + got * 2, drain_buf.end(), 0.0f);
    }

    // count = number of float samples (interleaved stereo)
    projectm_pcm_add_float(pm, drain_buf.data(), (unsigned)(frames * 2), PROJECTM_STEREO);
}


gboolean on_gl_render(GtkGLArea* area, GdkGLContext*, gpointer) {
static bool logged = false;
if (!logged) {
    logged = true;
    g_message("GL_VERSION=%s", glGetString(GL_VERSION));
    g_message("GL_RENDERER=%s", glGetString(GL_RENDERER));
}

    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area))
        return TRUE;

    std::lock_guard<std::mutex> lock(pm_mutex);
    if (!pm)
        return TRUE;

    int scale = current_scale_factor(GTK_WIDGET(area));
    int w = gtk_widget_get_allocated_width(GTK_WIDGET(area)) * scale;
    int h = gtk_widget_get_allocated_height(GTK_WIDGET(area)) * scale;
    w = std::max(w, 1);
    h = std::max(h, 1);

    if (w != fb_w || h != fb_h) {
        fb_w = w; fb_h = h;
        projectm_set_window_size(pm, fb_w, fb_h);
        glViewport(0, 0, fb_w, fb_h);
    }

    drain_audio_into_projectm_locked();

    // Avoid forcing GL pipeline state here: projectM manages its own render state,
    // and clobbering global state (notably blend/cull/depth settings) can result
    // in a fully black frame on some drivers/builds.
    glBindVertexArray(dummy_vao);
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    projectm_opengl_render_frame(pm);

    // Log GL errors once in a while
    static unsigned errn = 0;
    if ((++errn % 120) == 0) {
        for (GLenum e = glGetError(); e != GL_NO_ERROR; e = glGetError()) {
            g_warning("projectM: GL error after render: 0x%x", (unsigned)e);
        }
    }

return TRUE;
}

gboolean tick_cb(gpointer) {
    if (!running.load(std::memory_order_relaxed))
        return G_SOURCE_REMOVE;

    if (gl_area)
        gtk_gl_area_queue_render(GTK_GL_AREA(gl_area));

    return G_SOURCE_CONTINUE;
}

GtkWidget* create_gl_area() {
    GtkWidget* area = gtk_gl_area_new();
    gtk_widget_set_hexpand(area, TRUE);
    gtk_widget_set_vexpand(area, TRUE);

    gtk_gl_area_set_auto_render(GTK_GL_AREA(area), FALSE);
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(area), TRUE);
    gtk_gl_area_set_required_version(GTK_GL_AREA(area), 3, 2);
    gtk_gl_area_set_use_es(GTK_GL_AREA(area), FALSE);

    g_signal_connect(area, "create-context", G_CALLBACK(on_gl_create_context), nullptr);
    g_signal_connect(area, "realize",   G_CALLBACK(on_gl_realize),   nullptr);
    g_signal_connect(area, "unrealize", G_CALLBACK(on_gl_unrealize), nullptr);
    g_signal_connect(area, "render",    G_CALLBACK(on_gl_render),    nullptr);

    return area;
}

// Convert interleaved multi-channel PCM (512 frames) -> stereo interleaved
void push_multi_pcm_as_stereo(const float* pcm, int channels) {
    if (!pcm || channels <= 0)
        return;

    constexpr int frames = 512;

    // Fast path: already stereo interleaved
    if (channels == 2) {
        audio_ring.push(pcm, frames);
        return;
    }

    // Mix down/up to stereo: take ch0/ch1 if available, otherwise duplicate ch0
    std::vector<float> tmp;
    tmp.resize(frames * 2);

    for (int i = 0; i < frames; ++i) {
        float l = pcm[i * channels + 0];
        float r = (channels > 1) ? pcm[i * channels + 1] : l;
        tmp[i * 2 + 0] = l;
        tmp[i * 2 + 1] = r;
    }

    audio_ring.push(tmp.data(), frames);
}

} // namespace

static constexpr PluginInfo projectm_info = {
    N_("projectM Visualizer (GTK3)"),
    N_("projectM visualizer using GtkGLArea/OpenGL"),
    N_("Renders projectM visuals (MilkDrop-style) using OpenGL.")
};

class ProjectMVis final : public VisPlugin {
public:
    ProjectMVis() : VisPlugin(projectm_info, Visualizer::MonoPCM | Visualizer::MultiPCM) {}

    void clear() override {
        audio_ring.clear();
        std::lock_guard<std::mutex> lock(pm_mutex);
        // optional: reset projectM state if desired
    }

    void render_multi_pcm(const float* pcm, int channels) override {
        static unsigned calls = 0;
        if ((++calls % 200) == 0) {
            g_message("projectM: render_multi_pcm calls=%u channels=%d", calls, channels);
        }
        push_multi_pcm_as_stereo(pcm, channels);
    }


    void* get_gtk_widget() override {
        if (!gl_area) {
            gtk_init_check(nullptr, nullptr);
            gl_area = create_gl_area();
            gtk_widget_show(gl_area);
            running.store(true, std::memory_order_relaxed);
            if (!tick_id)
                tick_id = g_timeout_add(16, tick_cb, nullptr);
        }
        return gl_area;
    }

    bool init() override { return true; }

    void cleanup() override {
        running.store(false, std::memory_order_relaxed);

        if (tick_id) {
            g_source_remove(tick_id);
            tick_id = 0;
        }

        if (gl_area) {
            gtk_widget_destroy(gl_area);
            gl_area = nullptr;
        }

        std::lock_guard<std::mutex> lock(pm_mutex);
        pm_destroy_locked();
    }
};

extern "C" {

// Force the symbol to be exported in the dynamic symbol table.
__attribute__((visibility("default")))
ProjectMVis aud_plugin_instance;

} // extern "C"
