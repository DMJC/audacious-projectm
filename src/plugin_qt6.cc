#include <epoxy/gl.h>

#include <QDebug>
#include <QFileInfo>
#include <QKeyEvent>
#include <QOpenGLWidget>
#include <QPointer>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWidget>

#include <cstdlib>

#include <libaudcore/drct.h>
#include <libaudcore/hook.h>
#include <libaudcore/i18n.h>
#include <libaudcore/plugin.h>
#include <libaudcore/preferences.h>
#include <libaudcore/runtime.h>

#include <projectM-4/audio.h>
#include <projectM-4/parameters.h>
#include <projectM-4/playlist.h>
#include <projectM-4/playlist_callbacks.h>
#include <projectM-4/playlist_core.h>
#include <projectM-4/playlist_playback.h>
#include <projectM-4/projectM.h>
#include <projectM-4/render_opengl.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

#include "ringbuf.h"

#define PLUGIN_DOMAIN "projectm-vis"

static const char * const PREF_PRESET_DIR = "preset_dir";
static const char * const PREF_FPS = "fps";
static const char * const PREF_PRESET_DUR = "preset_duration";
static const char * const PREF_MESH_W = "mesh_width";
static const char * const PREF_MESH_H = "mesh_height";

static const char * const plugin_defaults[] = {
    PREF_PRESET_DIR, "/usr/share/projectM/presets",
    PREF_FPS, "60",
    PREF_PRESET_DUR, "15",
    PREF_MESH_W, "64",
    PREF_MESH_H, "48",
    nullptr
};

static const PreferencesWidget prefs_widgets[] = {
    WidgetLabel(N_("<b>Presets</b>")),
    WidgetEntry(N_("Preset folder:"), WidgetString(PLUGIN_DOMAIN, PREF_PRESET_DIR)),

    WidgetLabel(N_("<b>Rendering</b>")),
    WidgetSpin(N_("Frame rate (FPS):"), WidgetInt(PLUGIN_DOMAIN, PREF_FPS), {10, 120, 1, N_("fps")}),
    WidgetSpin(N_("Preset duration:"), WidgetInt(PLUGIN_DOMAIN, PREF_PRESET_DUR), {5, 300, 5, N_("seconds")}),

    WidgetLabel(N_("<b>Complexity (mesh size)</b>")),
    WidgetSpin(N_("Mesh width:"), WidgetInt(PLUGIN_DOMAIN, PREF_MESH_W), {8, 256, 8, N_("vertices")}),
    WidgetSpin(N_("Mesh height:"), WidgetInt(PLUGIN_DOMAIN, PREF_MESH_H), {8, 192, 8, N_("vertices")}),
    WidgetLabel(N_("<small>Recommended: 64×48 (medium), 128×96 (high).\n"
                   "Higher mesh = more detail, higher CPU cost.</small>")),
};

static const PluginPreferences prefs_page = {{prefs_widgets}};

namespace {
std::mutex pm_mutex;
projectm_handle pm = nullptr;
projectm_playlist_handle playlist = nullptr;
int fb_w = 0;
int fb_h = 0;
GLuint dummy_vao = 0;

std::atomic<bool> settings_changed{false};
std::atomic<bool> request_next_preset{false};

StereoFloatRing audio_ring(48000 * 2);
std::vector<float> drain_buf;

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

void pm_destroy_locked() {
    if (playlist) { projectm_playlist_destroy(playlist); playlist = nullptr; }
    if (pm) { projectm_destroy(pm); pm = nullptr; }
}

static void on_preset_switched(bool hard_cut, unsigned int index, void *) {
    qInfo("projectM(Qt6): preset switched -> index=%u hard_cut=%d", index, (int) hard_cut);
}

static void on_preset_switch_failed(const char * preset_filename, const char * message, void *) {
    qWarning("projectM(Qt6): preset switch FAILED -> %s (%s)",
        preset_filename ? preset_filename : "(null)",
        message ? message : "(no message)");
}

static void init_playlist_locked() {
    String preset_dir = get_preset_dir();
    const char * env_dir = getenv("PROJECTM_PRESET_DIR");
    if (env_dir && env_dir[0] != '\0' && QFileInfo::exists(env_dir) && QFileInfo(env_dir).isDir())
        preset_dir = String(env_dir);

    if (!QFileInfo::exists((const char *) preset_dir) || !QFileInfo((const char *) preset_dir).isDir()) {
        qWarning("projectM(Qt6): preset directory not found: %s", (const char *) preset_dir);
        return;
    }

    if (playlist) { projectm_playlist_destroy(playlist); playlist = nullptr; }

    playlist = projectm_playlist_create(pm);
    if (!playlist) {
        qWarning("projectM(Qt6): projectm_playlist_create() failed");
        return;
    }

    projectm_playlist_set_preset_switched_event_callback(playlist, on_preset_switched, nullptr);
    projectm_playlist_set_preset_switch_failed_event_callback(playlist, on_preset_switch_failed, nullptr);

    uint32_t added = projectm_playlist_add_path(playlist, (const char *) preset_dir, true, false);
    if (added == 0) {
        qWarning("projectM(Qt6): no presets added from %s", (const char *) preset_dir);
        return;
    }

    projectm_set_preset_duration(pm, (double) get_preset_duration());
    projectm_playlist_play_next(playlist, true);
}

static void apply_live_settings_locked() {
    if (!pm) return;
    projectm_set_fps(pm, (float) get_fps());
    projectm_set_preset_duration(pm, (double) get_preset_duration());
    projectm_set_mesh_size(pm, (unsigned) get_mesh_w(), (unsigned) get_mesh_h());
}

static bool pm_create_locked(int w, int h) {
    pm = projectm_create();
    if (!pm) return false;

    fb_w = std::max(w, 1);
    fb_h = std::max(h, 1);

    projectm_set_window_size(pm, fb_w, fb_h);
    projectm_set_mesh_size(pm, (unsigned) get_mesh_w(), (unsigned) get_mesh_h());
    projectm_set_fps(pm, (float) get_fps());

    init_playlist_locked();
    return true;
}

static void drain_audio_into_projectm_locked() {
    if (!pm) return;
    constexpr size_t frames = 512;
    drain_buf.resize(frames * 2);
    size_t got = audio_ring.pop(drain_buf.data(), frames);
    if (got < frames)
        std::fill(drain_buf.begin() + got * 2, drain_buf.end(), 0.0f);
    projectm_pcm_add_float(pm, drain_buf.data(), (unsigned) frames, PROJECTM_STEREO);
}

static void on_config_changed(void *, void *) {
    settings_changed.store(true, std::memory_order_relaxed);
}

class ProjectMQtWidget final : public QOpenGLWidget {
public:
    explicit ProjectMQtWidget(QWidget * parent = nullptr) : QOpenGLWidget(parent) {
        QSurfaceFormat fmt = format();
        fmt.setVersion(3, 2);
        fmt.setProfile(QSurfaceFormat::CoreProfile);
        fmt.setRenderableType(QSurfaceFormat::OpenGL);
        setFormat(fmt);
        setFocusPolicy(Qt::StrongFocus);

        tick.setSingleShot(false);
        connect(&tick, &QTimer::timeout, this, [this]() { update(); });
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

    void keyPressEvent(QKeyEvent * event) override {
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

QPointer<ProjectMQtWidget> qt_widget;

void push_multi_pcm_as_stereo(const float * pcm, int channels) {
    if (!pcm || channels <= 0) return;
    constexpr int frames = 512;
    if (channels == 2) {
        audio_ring.push(pcm, frames);
        return;
    }

    std::vector<float> tmp(frames * 2);
    for (int i = 0; i < frames; ++i) {
        tmp[i * 2 + 0] = pcm[i * channels + 0];
        tmp[i * 2 + 1] = (channels > 1) ? pcm[i * channels + 1] : pcm[i * channels + 0];
    }
    audio_ring.push(tmp.data(), frames);
}
} // namespace

static constexpr PluginInfo projectm_info = {
    N_("projectM Visualizer (Qt6)"),
    N_("projectM visualizer using QOpenGLWidget/OpenGL"),
    N_("Renders projectM visuals (MilkDrop-style) using OpenGL."),
    &prefs_page
};

class ProjectMVisQt final : public VisPlugin {
public:
    ProjectMVisQt() : VisPlugin(projectm_info, Visualizer::MonoPCM | Visualizer::MultiPCM) {}

    void clear() override {
        audio_ring.clear();
        std::lock_guard<std::mutex> lock(pm_mutex);
    }

    void render_multi_pcm(const float * pcm, int channels) override {
        push_multi_pcm_as_stereo(pcm, channels);
    }

    bool init() override {
        aud_config_set_defaults(PLUGIN_DOMAIN, plugin_defaults);
        hook_associate("set " PLUGIN_DOMAIN, on_config_changed, nullptr);
        return true;
    }

    void * get_qt_widget() override {
        if (!qt_widget) {
            qt_widget = new ProjectMQtWidget();
            QObject::connect(qt_widget, &QObject::destroyed, [](QObject *) {
                qt_widget = nullptr;
            });
        }
        return qt_widget;
    }

    void cleanup() override {
        hook_dissociate("set " PLUGIN_DOMAIN, on_config_changed);

        // Host UI owns the widget lifetime. Avoid deleting here because cleanup()
        // can be called while a close event/destruction chain is already in flight.
        if (qt_widget)
            qt_widget->close();
        qt_widget = nullptr;

        std::lock_guard<std::mutex> lock(pm_mutex);
        pm_destroy_locked();
    }
};

extern "C" {
__attribute__((visibility("default")))
ProjectMVisQt aud_plugin_instance;
}
