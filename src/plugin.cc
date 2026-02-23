#include <gtk/gtk.h>
#include <epoxy/gl.h>

#include <QDebug>
#include <QKeyEvent>
#include <QOpenGLWidget>
#include <QPointer>
#include <QSurfaceFormat>
#include <QTimer>

#include <libaudcore/plugin.h>
#include <libaudcore/i18n.h>
#include <libaudcore/drct.h>
#include <libaudcore/preferences.h>
#include <libaudcore/runtime.h>
#include <libaudcore/hook.h>

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

// ---------------------------------------------------------------------------
// Settings keys, defaults, and config registration
// ---------------------------------------------------------------------------
#define PLUGIN_DOMAIN "projectm-vis"

static const char* const PREF_PRESET_DIR = "preset_dir";
static const char* const PREF_FPS        = "fps";
static const char* const PREF_PRESET_DUR = "preset_duration";
static const char* const PREF_MESH_W     = "mesh_width";
static const char* const PREF_MESH_H     = "mesh_height";

static const char* const plugin_defaults[] = {
    PREF_PRESET_DIR, "/usr/share/projectM/presets",
    PREF_FPS,        "60",
    PREF_PRESET_DUR, "15",
    PREF_MESH_W,     "64",
    PREF_MESH_H,     "48",
    nullptr
};

// WidgetEntry for strings, WidgetSpin for ints — no WidgetDouble in libaudcore.
// Preset duration stored as integer seconds.
static const PreferencesWidget prefs_widgets[] = {
    WidgetLabel(N_("<b>Presets</b>")),
    WidgetEntry(N_("Preset folder:"),
        WidgetString(PLUGIN_DOMAIN, PREF_PRESET_DIR)),

    WidgetLabel(N_("<b>Rendering</b>")),
    WidgetSpin(N_("Frame rate (FPS):"),
        WidgetInt(PLUGIN_DOMAIN, PREF_FPS),
        {10, 120, 1, N_("fps")}),
    WidgetSpin(N_("Preset duration:"),
        WidgetInt(PLUGIN_DOMAIN, PREF_PRESET_DUR),
        {5, 300, 5, N_("seconds")}),

    WidgetLabel(N_("<b>Complexity (mesh size)</b>")),
    WidgetSpin(N_("Mesh width:"),
        WidgetInt(PLUGIN_DOMAIN, PREF_MESH_W),
        {8, 256, 8, N_("vertices")}),
    WidgetSpin(N_("Mesh height:"),
        WidgetInt(PLUGIN_DOMAIN, PREF_MESH_H),
        {8, 192, 8, N_("vertices")}),
    WidgetLabel(N_("<small>Recommended: 64×48 (medium), 128×96 (high).\n"
                   "Higher mesh = more detail, higher CPU cost.</small>")),
};

static const PluginPreferences prefs_page = {{prefs_widgets}};

// ---------------------------------------------------------------------------

namespace {

GtkWidget* gl_area = nullptr;
class ProjectMQtWidget;
QPointer<ProjectMQtWidget> qt_widget;

std::mutex pm_mutex;
projectm_handle pm = nullptr;
projectm_playlist_handle playlist = nullptr;

int fb_w = 0;
int fb_h = 0;
GLuint dummy_vao = 0;

std::atomic<bool> running{false};
std::atomic<bool> request_next_preset{false};
std::atomic<bool> settings_changed{false};

guint tick_id = 0;

StereoFloatRing audio_ring(48000 * 2);
std::vector<float> drain_buf;

// ---------------------------------------------------------------------------
// Settings helpers — read live from Audacious config store
// ---------------------------------------------------------------------------
static String get_preset_dir() {
    String s = aud_get_str(PLUGIN_DOMAIN, PREF_PRESET_DIR);
    if (!s || !s[0]) return String("/usr/share/projectM/presets");
    return s;
}
static int get_fps() {
    int v = aud_get_int(PLUGIN_DOMAIN, PREF_FPS);
    return (v >= 10 && v <= 120) ? v : 60;
}
static int get_preset_duration() {
    int v = aud_get_int(PLUGIN_DOMAIN, PREF_PRESET_DUR);
    return (v >= 5 && v <= 300) ? v : 15;
}
static int get_mesh_w() {
    int v = aud_get_int(PLUGIN_DOMAIN, PREF_MESH_W);
    return (v >= 8 && v <= 256) ? v : 64;
}
static int get_mesh_h() {
    int v = aud_get_int(PLUGIN_DOMAIN, PREF_MESH_H);
    return (v >= 8 && v <= 192) ? v : 48;
}

// ---------------------------------------------------------------------------

gint current_scale_factor(GtkWidget* w) {
    if (!w) return 1;
    return std::max(gtk_widget_get_scale_factor(w), 1);
}

void pm_destroy_locked() {
    if (playlist) { projectm_playlist_destroy(playlist); playlist = nullptr; }
    if (pm)       { projectm_destroy(pm);                pm = nullptr;       }
}

static void on_preset_switched(bool hard_cut, unsigned int index, void*)
{
    g_message("projectM: preset switched -> index=%u hard_cut=%d",
              index, (int)hard_cut);
}

static void on_preset_switch_failed(const char* preset_filename,
                                     const char* message, void*)
{
    g_warning("projectM: preset switch FAILED -> %s (%s)",
              preset_filename ? preset_filename : "(null)",
              message        ? message         : "(no message)");
}

static void init_playlist_locked()
{
    String preset_dir = get_preset_dir();

    // Environment variable overrides the stored setting
    const char* env_dir = g_getenv("PROJECTM_PRESET_DIR");
    if (env_dir && env_dir[0] != '\0' && g_file_test(env_dir, G_FILE_TEST_IS_DIR))
        preset_dir = String(env_dir);

    if (!g_file_test((const char*)preset_dir, G_FILE_TEST_IS_DIR)) {
        g_warning("projectM: preset directory not found: %s",
                  (const char*)preset_dir);
        return;
    }

    if (playlist) { projectm_playlist_destroy(playlist); playlist = nullptr; }

    playlist = projectm_playlist_create(pm);
    if (!playlist) { g_warning("projectM: projectm_playlist_create() failed"); return; }

    projectm_playlist_set_preset_switched_event_callback(
        playlist, on_preset_switched, nullptr);
    projectm_playlist_set_preset_switch_failed_event_callback(
        playlist, on_preset_switch_failed, nullptr);

    uint32_t added = projectm_playlist_add_path(
        playlist, (const char*)preset_dir, true, false);
    if (added == 0) {
        g_warning("projectM: no presets added from %s", (const char*)preset_dir);
        return;
    }
    g_message("projectM: added %u presets from %s", added, (const char*)preset_dir);

    projectm_set_preset_duration(pm, (double)get_preset_duration());

    uint32_t idx = projectm_playlist_play_next(playlist, true);
    g_message("projectM: play_next(hard_cut=true) -> idx=%u", idx);
}

// Apply FPS / duration / mesh to a running projectM instance.
// Must be called with pm_mutex held and GL context current.
static void apply_live_settings_locked()
{
    if (!pm) return;
    projectm_set_fps(pm, (float)get_fps());
    projectm_set_preset_duration(pm, (double)get_preset_duration());
    projectm_set_mesh_size(pm, (unsigned)get_mesh_w(), (unsigned)get_mesh_h());
}

bool pm_create_locked(int w, int h) {
    pm = projectm_create();
    if (!pm) return false;
    fb_w = std::max(w, 1);
    fb_h = std::max(h, 1);
    projectm_set_window_size(pm, fb_w, fb_h);
    projectm_set_mesh_size(pm, (unsigned)get_mesh_w(), (unsigned)get_mesh_h());
    projectm_set_fps(pm, (float)get_fps());
    init_playlist_locked();
    return true;
}

// ---------------------------------------------------------------------------

GdkGLContext* on_gl_create_context(GtkGLArea* area, gpointer)
{
    GError* error = nullptr;
    GdkWindow* window = gtk_widget_get_window(GTK_WIDGET(area));
    if (!window) return nullptr;
    GdkGLContext* context = gdk_window_create_gl_context(window, &error);
    if (!context) { g_clear_error(&error); return nullptr; }
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
    if (!dummy_vao) glGenVertexArrays(1, &dummy_vao);
    glBindVertexArray(dummy_vao);

    int scale = current_scale_factor(GTK_WIDGET(area));
    int w = gtk_widget_get_allocated_width(GTK_WIDGET(area))  * scale;
    int h = gtk_widget_get_allocated_height(GTK_WIDGET(area)) * scale;

    std::lock_guard<std::mutex> lock(pm_mutex);
    pm_destroy_locked();
    pm_create_locked(w, h);
}

void on_gl_unrealize(GtkGLArea* area, gpointer) {
    gtk_gl_area_make_current(area);
    std::lock_guard<std::mutex> lock(pm_mutex);
    if (dummy_vao) { glDeleteVertexArrays(1, &dummy_vao); dummy_vao = 0; }
    pm_destroy_locked();
    (void)area;
}

static void drain_audio_into_projectm_locked() {
    if (!pm) return;
    constexpr size_t frames = 512;
    drain_buf.resize(frames * 2);
    size_t got = audio_ring.pop(drain_buf.data(), frames);
    if (got < frames)
        std::fill(drain_buf.begin() + got * 2, drain_buf.end(), 0.0f);
    projectm_pcm_add_float(pm, drain_buf.data(), (unsigned)frames, PROJECTM_STEREO);
}

gboolean on_gl_render(GtkGLArea* area, GdkGLContext*, gpointer) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        g_message("GL_VERSION=%s",  glGetString(GL_VERSION));
        g_message("GL_RENDERER=%s", glGetString(GL_RENDERER));
    }

    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area)) return TRUE;

    std::lock_guard<std::mutex> lock(pm_mutex);
    if (!pm) return TRUE;

    // Deferred: apply changed preferences (GL context must be current).
    // Preset dir change also rebuilds the playlist.
    if (settings_changed.exchange(false, std::memory_order_relaxed)) {
        apply_live_settings_locked();
        init_playlist_locked();
    }

    // Deferred: preset skip requested from spacebar key handler.
    if (request_next_preset.exchange(false, std::memory_order_relaxed)) {
        if (playlist)
            projectm_playlist_play_next(playlist, true);
    }

    GLint gtk_default_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &gtk_default_fbo);

    int scale = current_scale_factor(GTK_WIDGET(area));
    int w = std::max(gtk_widget_get_allocated_width(GTK_WIDGET(area))  * scale, 1);
    int h = std::max(gtk_widget_get_allocated_height(GTK_WIDGET(area)) * scale, 1);

    if (w != fb_w || h != fb_h) {
        fb_w = w; fb_h = h;
        projectm_set_window_size(pm, fb_w, fb_h);
    }

    drain_audio_into_projectm_locked();

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) gtk_default_fbo);
    glBindVertexArray(dummy_vao);
    glViewport(0, 0, fb_w, fb_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    projectm_opengl_render_frame_fbo(pm, (GLuint) gtk_default_fbo);

    static unsigned errn = 0;
    if ((++errn % 120) == 0) {
        for (GLenum e = glGetError(); e != GL_NO_ERROR; e = glGetError())
            g_warning("projectM: GL error after render: 0x%x", (unsigned)e);
    }

    return TRUE;
}

gboolean tick_cb(gpointer) {
    if (!running.load(std::memory_order_relaxed)) return G_SOURCE_REMOVE;
    if (gl_area) gtk_gl_area_queue_render(GTK_GL_AREA(gl_area));
    return G_SOURCE_CONTINUE;
}

static void toggle_fullscreen(GtkWidget* widget)
{
    GtkWidget* toplevel = gtk_widget_get_toplevel(widget);
    if (!GTK_IS_WINDOW(toplevel)) return;
    GdkWindow* gdk_window = gtk_widget_get_window(toplevel);
    if (!gdk_window) return;
    GdkWindowState state = gdk_window_get_state(gdk_window);
    if (state & GDK_WINDOW_STATE_FULLSCREEN)
        gtk_window_unfullscreen(GTK_WINDOW(toplevel));
    else
        gtk_window_fullscreen(GTK_WINDOW(toplevel));
}

static gboolean on_key_press(GtkWidget* widget, GdkEventKey* event, gpointer)
{
    switch (event->keyval) {
    case GDK_KEY_z: case GDK_KEY_Z: aud_drct_pl_prev();  return TRUE;
    case GDK_KEY_x: case GDK_KEY_X: aud_drct_play();     return TRUE;
    case GDK_KEY_c: case GDK_KEY_C: aud_drct_pause();    return TRUE;
    case GDK_KEY_v: case GDK_KEY_V: aud_drct_stop();     return TRUE;
    case GDK_KEY_b: case GDK_KEY_B: aud_drct_pl_next();  return TRUE;
    case GDK_KEY_F11: toggle_fullscreen(widget);          return TRUE;
    case GDK_KEY_space:
        // Never call projectM here — no GL context in event handlers.
        request_next_preset.store(true, std::memory_order_relaxed);
        return TRUE;
    default: break;
    }
    return FALSE;
}


class ProjectMQtWidget final : public QOpenGLWidget {
public:
    explicit ProjectMQtWidget(QWidget* parent = nullptr) : QOpenGLWidget(parent)
    {
        QSurfaceFormat fmt = format();
        fmt.setVersion(3, 2);
        fmt.setProfile(QSurfaceFormat::CoreProfile);
        setFormat(fmt);
        setFocusPolicy(Qt::StrongFocus);

        tick.setSingleShot(false);
        QObject::connect(&tick, &QTimer::timeout, this, [this]() { update(); });
        tick.start(std::max(1, 1000 / get_fps()));
    }

    ~ProjectMQtWidget() override {
        std::lock_guard<std::mutex> lock(pm_mutex);
        pm_destroy_locked();
    }

    void refresh_tick_rate() {
        tick.start(std::max(1, 1000 / get_fps()));
    }

protected:
    void initializeGL() override {
        if (!dummy_vao) glGenVertexArrays(1, &dummy_vao);
        glBindVertexArray(dummy_vao);

        std::lock_guard<std::mutex> lock(pm_mutex);
        pm_destroy_locked();
        pm_create_locked(width() * devicePixelRatio(), height() * devicePixelRatio());
    }

    void paintGL() override {
        std::lock_guard<std::mutex> lock(pm_mutex);
        if (!pm) return;

        if (settings_changed.exchange(false, std::memory_order_relaxed)) {
            apply_live_settings_locked();
            init_playlist_locked();
            refresh_tick_rate();
        }

        if (request_next_preset.exchange(false, std::memory_order_relaxed) && playlist)
            projectm_playlist_play_next(playlist, true);

        int w = std::max((int) (width() * devicePixelRatio()), 1);
        int h = std::max((int) (height() * devicePixelRatio()), 1);
        if (w != fb_w || h != fb_h) {
            fb_w = w;
            fb_h = h;
            projectm_set_window_size(pm, fb_w, fb_h);
        }

        drain_audio_into_projectm_locked();

        const GLuint qt_fbo = (GLuint) defaultFramebufferObject();
        glBindFramebuffer(GL_FRAMEBUFFER, qt_fbo);
        glBindVertexArray(dummy_vao);
        glViewport(0, 0, fb_w, fb_h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        projectm_opengl_render_frame_fbo(pm, qt_fbo);
    }

    void keyPressEvent(QKeyEvent* event) override {
        switch (event->key()) {
        case Qt::Key_Z: aud_drct_pl_prev(); return;
        case Qt::Key_X: aud_drct_play(); return;
        case Qt::Key_C: aud_drct_pause(); return;
        case Qt::Key_V: aud_drct_stop(); return;
        case Qt::Key_B: aud_drct_pl_next(); return;
        case Qt::Key_F11:
            if (window()->isFullScreen()) window()->showNormal();
            else window()->showFullScreen();
            return;
        case Qt::Key_Space:
            request_next_preset.store(true, std::memory_order_relaxed);
            return;
        default:
            QOpenGLWidget::keyPressEvent(event);
            return;
        }
    }

    void resizeGL(int w, int h) override {
        std::lock_guard<std::mutex> lock(pm_mutex);
        if (pm) {
            fb_w = std::max((int) (w * devicePixelRatio()), 1);
            fb_h = std::max((int) (h * devicePixelRatio()), 1);
            projectm_set_window_size(pm, fb_w, fb_h);
        }
    }

private:
    QTimer tick;
};

GtkWidget* create_gl_area() {
    GtkWidget* area = gtk_gl_area_new();
    gtk_widget_set_hexpand(area, TRUE);
    gtk_widget_set_vexpand(area, TRUE);
    gtk_gl_area_set_auto_render(GTK_GL_AREA(area), FALSE);
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(area), TRUE);
    gtk_gl_area_set_required_version(GTK_GL_AREA(area), 3, 2);
    gtk_gl_area_set_use_es(GTK_GL_AREA(area), FALSE);
    gtk_widget_set_can_focus(area, TRUE);
    gtk_widget_add_events(area, GDK_KEY_PRESS_MASK);

    g_signal_connect(area, "create-context",  G_CALLBACK(on_gl_create_context), nullptr);
    g_signal_connect(area, "realize",         G_CALLBACK(on_gl_realize),        nullptr);
    g_signal_connect(area, "unrealize",       G_CALLBACK(on_gl_unrealize),      nullptr);
    g_signal_connect(area, "render",          G_CALLBACK(on_gl_render),         nullptr);
    g_signal_connect(area, "key-press-event", G_CALLBACK(on_key_press),         nullptr);

    return area;
}

void push_multi_pcm_as_stereo(const float* pcm, int channels) {
    if (!pcm || channels <= 0) return;
    constexpr int frames = 512;
    if (channels == 2) { audio_ring.push(pcm, frames); return; }
    std::vector<float> tmp(frames * 2);
    for (int i = 0; i < frames; ++i) {
        tmp[i * 2 + 0] = pcm[i * channels + 0];
        tmp[i * 2 + 1] = (channels > 1) ? pcm[i * channels + 1]
                                         : pcm[i * channels + 0];
    }
    audio_ring.push(tmp.data(), frames);
}

// Hook handler — fired by Audacious on the main thread whenever any
// PLUGIN_DOMAIN config key changes (i.e. user clicks Apply in prefs).
static void on_config_changed(void*, void*)
{
    settings_changed.store(true, std::memory_order_relaxed);
}

} // namespace

// ---------------------------------------------------------------------------
// Plugin descriptor
// ---------------------------------------------------------------------------

static constexpr PluginInfo projectm_info = {
    N_("projectM Visualizer"),
    N_("projectM visualizer using GTK3/Qt6 OpenGL widgets"),
    N_("Renders projectM visuals (MilkDrop-style) using OpenGL."),
    &prefs_page
};

class ProjectMVis final : public VisPlugin {
public:
    ProjectMVis() : VisPlugin(projectm_info,
                              Visualizer::MonoPCM | Visualizer::MultiPCM) {}

    void clear() override {
        audio_ring.clear();
        std::lock_guard<std::mutex> lock(pm_mutex);
    }

    void render_multi_pcm(const float* pcm, int channels) override {
        push_multi_pcm_as_stereo(pcm, channels);
    }

    bool init() override {
        // Register defaults so aud_get_int/str return sane values first call.
        aud_config_set_defaults(PLUGIN_DOMAIN, plugin_defaults);

        // Watch for any config key change in our domain.
        hook_associate("set " PLUGIN_DOMAIN, on_config_changed, nullptr);
        return true;
    }

    void* get_gtk_widget() override {
        if (!gl_area) {
            gtk_init_check(nullptr, nullptr);
            gl_area = create_gl_area();
            gtk_widget_show(gl_area);
            gtk_widget_grab_focus(gl_area);
            running.store(true, std::memory_order_relaxed);
            if (!tick_id)
                tick_id = g_timeout_add(1000 / get_fps(), tick_cb, nullptr);
        }
        return gl_area;
    }

    void* get_qt_widget() override {
        if (!qt_widget) {
            qt_widget = new ProjectMQtWidget();
            QObject::connect(qt_widget, &QObject::destroyed, [](QObject*) {
                qt_widget = nullptr;
            });
        }
        return qt_widget;
    }

    void cleanup() override {
        hook_dissociate("set " PLUGIN_DOMAIN, on_config_changed);

        running.store(false, std::memory_order_relaxed);
        request_next_preset.store(false, std::memory_order_relaxed);
        settings_changed.store(false, std::memory_order_relaxed);

        if (tick_id) { g_source_remove(tick_id); tick_id = 0; }

        if (gl_area) {
            gtk_widget_destroy(gl_area);
            gl_area = nullptr;
        }

        if (qt_widget)
            qt_widget->close();
        qt_widget = nullptr;

        std::lock_guard<std::mutex> lock(pm_mutex);
        pm_destroy_locked();
    }
};

extern "C" {
__attribute__((visibility("default")))
ProjectMVis aud_plugin_instance;
} // extern "C"
