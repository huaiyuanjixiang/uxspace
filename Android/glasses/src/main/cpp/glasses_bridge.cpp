// JNI bridge between UxSpace and the open viture-v2 driver.
//
// This replaces the bridge to VITURE's proprietary libglasses.so. The Kotlin
// side (NativeGlasses, HeadTracking, everything above them) is untouched — only
// what sits under it changed, from a closed binary to a Rust driver that speaks
// the Gen2 wire protocol directly.
//
// What that buys: no developer-access request, no vendored .so, and the driver
// works on any device whose product id speaks protocol V2.
//
// What it does not cover: Carina (VIO / 6DOF, i.e. Luma Ultra). Those entry
// points stay present so the Kotlin side keeps linking, but they report "not
// supported". Gen2 devices such as the Pro 2 do not have native DOF anyway —
// the device itself reports that, and the driver confirms it.

#include <android/log.h>
#include <jni.h>

#include <atomic>
#include <cstring>

extern "C" {

// ---- viture-v2 C ABI -------------------------------------------------------

typedef struct XrTracker XrTracker;

typedef struct {
    float head_q[4];
    float predicted_q[4];
    float cursor_x;
    float cursor_y;
    uint32_t cursor_valid;
    uint64_t head_samples;
    uint64_t phone_samples;
} XrState;

XrTracker *xr_open(int fd, unsigned int rate_hz);
XrTracker *xr_open_simulated(unsigned int rate_hz, unsigned int motion);
int xr_has_simulation();
void xr_close(XrTracker *t);
void xr_set_phone_quat(XrTracker *t, float w, float x, float y, float z);
void xr_recentre(XrTracker *t);
void xr_set_lookahead_ms(XrTracker *t, float ms);
void xr_set_distance(XrTracker *t, float distance);
int xr_state(XrTracker *t, XrState *out);
int xr_pose7(XrTracker *t, float *out);
int xr_pose_fresh(XrTracker *t);
int xr_brightness(XrTracker *t);
int xr_volume(XrTracker *t);
int xr_display_mode(XrTracker *t);
int xr_firmware(XrTracker *t, unsigned char *out, size_t cap);
int xr_diag(XrTracker *t, long long *out);
int xr_set_display_mode(XrTracker *t, unsigned char mode);
int xr_predict(XrTracker *t, float w, float x, float y, float z, float dt_seconds,
               float *out);
int xr_angular_rate(XrTracker *t, float *out);

// Mesh projections, from the file's own sv3d/proj box.
typedef struct XrMesh XrMesh;
int xr_proj_kind(const unsigned char *data, size_t len, float *out);

// Reading a media library's listings, from the crate's `library` module.
int xr_library_infer(const unsigned char *blob, size_t blob_len, const unsigned int *meta,
                     size_t meta_len, unsigned int *out, size_t out_len);
unsigned int xr_library_infer_one(const unsigned char *name, size_t name_len,
                                  const unsigned char *mode, size_t mode_len,
                                  unsigned int width, unsigned int height);
void xr_library_calibrate();
void xr_library_recalibrate();
int xr_library_plan(unsigned int *out);
XrMesh *xr_mesh_parse(const unsigned char *data, size_t len);
int xr_mesh_count(const XrMesh *p);
int xr_mesh_submesh_count(const XrMesh *p, int mesh);
int xr_mesh_submesh_info(const XrMesh *p, int mesh, int submesh, int *out);
int xr_mesh_copy(const XrMesh *p, int mesh, int submesh, float *out, size_t cap);
void xr_mesh_free(XrMesh *p);

// Panorama geometry and camera maths, from the crate's `render` feature.
unsigned int xr_pano_vertex_count(unsigned int rings, unsigned int sectors);
unsigned int xr_pano_index_count(unsigned int rings, unsigned int sectors);
int xr_pano_bounds(unsigned int projection, float *out);
int xr_pano_mesh(unsigned int rings, unsigned int sectors, float radius, const float *bounds,
                 float *out, size_t cap_floats);
int xr_pano_indices(unsigned int rings, unsigned int sectors, const float *bounds,
                    unsigned short *out, size_t cap);
unsigned int xr_cube_vertex_count(unsigned int cells);
unsigned int xr_cube_index_count(unsigned int cells);
int xr_cube_mesh(unsigned int cells, float radius, float padding, float *out, size_t cap_floats);
int xr_cube_indices(unsigned int cells, unsigned short *out, size_t cap);
int xr_pano_uv(unsigned int layout, int eye, float *out);
int xr_anaglyph_mix(unsigned int pair, int eye, float *out);
int xr_pano_mvp(float w, float x, float y, float z, float fov_y_deg, float aspect,
                float eye_offset, float *out);
unsigned int xr_screen_vertex_count(unsigned int segments);
int xr_screen_mesh(unsigned int segments, float distance, float width_deg, float aspect,
                   float curvature, float *out, size_t cap_floats);
float xr_pano_fov_for_zoom(float zoom);
float xr_fov_for_panel(unsigned int product_id, unsigned int eye_w, unsigned int eye_h);
int xr_model_info(unsigned int product_id, float *out);

} // extern "C"

namespace {

/// Everything the bridge remembers between calls. `create` only records the
/// descriptor; the device is opened in `openImu`, which is where the Kotlin
/// lifecycle actually wants the stream to start.
struct Bridge {
    std::atomic<XrTracker *> tracker{nullptr};
    int fd = -1;
    int pid = 0;
    /// Pose reporting rate. Pose tops out at 240 Hz on Gen2 hardware.
    unsigned int rate_hz = 120;
};

Bridge g;

jfloatArray make_pose_array(JNIEnv *env, const float *values) {
    jfloatArray arr = env->NewFloatArray(7);
    if (arr != nullptr) {
        env->SetFloatArrayRegion(arr, 0, 7, values);
    }
    return arr;
}

#define BRIDGE_LOG(level, ...) \
    __android_log_print(level, "UxSpace/Bridge", __VA_ARGS__)

} // namespace

extern "C" {

JNIEXPORT jstring JNICALL
Java_com_uxspace_glasses_NativeGlasses_getVersion(JNIEnv *env, jobject) {
    unsigned char buf[64] = {0};
    XrTracker *t = g.tracker.load(std::memory_order_acquire);
    if (t != nullptr && xr_firmware(t, buf, sizeof(buf)) > 0) {
        return env->NewStringUTF(reinterpret_cast<const char *>(buf));
    }
    return env->NewStringUTF("viture-v2 (open driver)");
}

// ---- Lifecycle -------------------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_uxspace_glasses_NativeGlasses_create(JNIEnv *, jobject, jint pid, jint fd) {
    if (fd < 0) {
        return JNI_FALSE;
    }
    g.fd = fd;
    g.pid = pid;
    return JNI_TRUE;
}

JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_getDeviceType(JNIEnv *, jobject) {
    // -1 marks Gen1/Gen2 hardware; 2 would be Carina, which this bridge does
    // not serve.
    return -1;
}

JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_registerStateCallback(JNIEnv *, jobject) {
    // Brightness and volume are read once when the device opens; there is no
    // asynchronous state channel to subscribe to yet.
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_initialize(JNIEnv *, jobject) {
    // The wire protocol has no handshake — a captured SDK session sends nothing
    // on the endpoint until the stream is switched on.
    return g.fd >= 0 ? 0 : -1;
}

JNIEXPORT jint JNICALL Java_com_uxspace_glasses_NativeGlasses_start(JNIEnv *, jobject) {
    return 0;
}

JNIEXPORT jint JNICALL Java_com_uxspace_glasses_NativeGlasses_stop(JNIEnv *, jobject) {
    return 0;
}

JNIEXPORT jint JNICALL Java_com_uxspace_glasses_NativeGlasses_shutdown(JNIEnv *, jobject) {
    return 0;
}

JNIEXPORT void JNICALL Java_com_uxspace_glasses_NativeGlasses_destroy(JNIEnv *, jobject) {
    XrTracker *t = g.tracker.exchange(nullptr, std::memory_order_acq_rel);
    if (t != nullptr) {
        xr_close(t);
    }
    g.fd = -1;
}

// ---- Gen1/Gen2 head tracking ----------------------------------------------

/// Whether this build can run without glasses attached.
JNIEXPORT jboolean JNICALL
Java_com_uxspace_glasses_NativeGlasses_hasSimulation(JNIEnv *, jobject) {
    return xr_has_simulation() != 0 ? JNI_TRUE : JNI_FALSE;
}

/// Opens a tracker with nothing on the other end of the cable.
///
/// Everything above the wire runs against it, answering what the hardware was
/// measured answering. `motion`: 0 still, 1 turning, 2 turning then stopping.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_openSimulatedImu(JNIEnv *, jobject, jint motion) {
    if (g.tracker.load(std::memory_order_acquire) != nullptr) {
        return 0; // already streaming
    }
    if (xr_has_simulation() == 0) {
        BRIDGE_LOG(ANDROID_LOG_ERROR, "openSimulatedImu: this build has no simulation");
        return -1;
    }
    BRIDGE_LOG(ANDROID_LOG_INFO, "openSimulatedImu: no glasses, simulating (motion=%d)", motion);
    XrTracker *t = xr_open_simulated(g.rate_hz, static_cast<unsigned int>(motion));
    if (t == nullptr) {
        BRIDGE_LOG(ANDROID_LOG_ERROR, "openSimulatedImu: the simulation would not start");
        return -1;
    }
    g.tracker.store(t, std::memory_order_release);
    return 0;
}

JNIEXPORT jint JNICALL Java_com_uxspace_glasses_NativeGlasses_openImu(JNIEnv *, jobject) {
    if (g.tracker.load(std::memory_order_acquire) != nullptr) {
        return 0; // already streaming
    }
    if (g.fd < 0) {
        return -1;
    }
    BRIDGE_LOG(ANDROID_LOG_INFO, "openImu: xr_open(fd=%d, %u Hz)", g.fd, g.rate_hz);
    XrTracker *t = xr_open(g.fd, g.rate_hz);
    if (t == nullptr) {
        BRIDGE_LOG(ANDROID_LOG_ERROR, "openImu: xr_open failed for fd=%d", g.fd);
        return -1;
    }
    unsigned char fw[64] = {0};
    xr_firmware(t, fw, sizeof(fw));
    const int mode = xr_display_mode(t);
    BRIDGE_LOG(ANDROID_LOG_INFO,
               "openImu: tracker up, firmware='%s' brightness=%d displayMode=0x%02X",
               reinterpret_cast<const char *>(fw), xr_brightness(t), mode);

    // The panel can end up in a mode the host cannot use — pressing the physical
    // 2D/3D button cycles through the low-resolution EDID entries as well, and
    // Android then settles on 640x480 with no way back from its side. Nudge it
    // home. Known-good modes are left alone, including the 3D ones.
    switch (mode) {
        case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
        case 0x41: case 0x42: case 0x43: case 0x44: case 0x45:
            break;
        default:
            BRIDGE_LOG(ANDROID_LOG_WARN,
                       "display mode 0x%02X is not one this host can drive — "
                       "restoring 1920x1080", mode);
            xr_set_display_mode(t, 0x31);
            break;
    }
    g.tracker.store(t, std::memory_order_release);
    return 0;
}

JNIEXPORT jint JNICALL Java_com_uxspace_glasses_NativeGlasses_closeImu(JNIEnv *, jobject) {
    XrTracker *t = g.tracker.exchange(nullptr, std::memory_order_acq_rel);
    if (t != nullptr) {
        xr_close(t);
    }
    return 0;
}

// ---- Native DOF ------------------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_uxspace_glasses_NativeGlasses_isProductSupportNativeDof(JNIEnv *, jobject, jint) {
    // Gen2 glasses fuse nothing on board — the host does the tracking. The
    // vendor SDK answers the same for a Pro 2.
    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_uxspace_glasses_NativeGlasses_setupNativeDofDevice(JNIEnv *, jobject) {}

JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_nativeRecenterDof(JNIEnv *, jobject) {
    XrTracker *t = g.tracker.load(std::memory_order_acquire);
    if (t == nullptr) {
        return -1;
    }
    xr_recentre(t);
    return 0;
}

// ---- Carina: present so linking succeeds, not supported --------------------

JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_setDofTypeCarina(JNIEnv *, jobject, jboolean) {
    return -1;
}

JNIEXPORT void JNICALL
Java_com_uxspace_glasses_NativeGlasses_startCarinaPollThread(JNIEnv *, jobject) {}

JNIEXPORT void JNICALL
Java_com_uxspace_glasses_NativeGlasses_stopCarinaPollThread(JNIEnv *, jobject) {}

JNIEXPORT jboolean JNICALL
Java_com_uxspace_glasses_NativeGlasses_carinaStopForModeSwitch(JNIEnv *, jobject) {
    return JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_resetOriginCarina(JNIEnv *, jobject) {
    return -1;
}

JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_resetPoseCarina(JNIEnv *, jobject) {
    return -1;
}

// ---- Pose ------------------------------------------------------------------

JNIEXPORT jfloatArray JNICALL
Java_com_uxspace_glasses_NativeGlasses_getPose(JNIEnv *env, jobject) {
    float pose[7] = {0, 0, 0, 1, 0, 0, 0};
    XrTracker *t = g.tracker.load(std::memory_order_acquire);
    if (t != nullptr) {
        xr_pose7(t, pose);
    }
    return make_pose_array(env, pose);
}

JNIEXPORT jboolean JNICALL
Java_com_uxspace_glasses_NativeGlasses_isPoseFresh(JNIEnv *, jobject) {
    XrTracker *t = g.tracker.load(std::memory_order_acquire);
    if (t == nullptr) {
        return JNI_FALSE;
    }
    // Report the driver's own counters now and then. A pose stream that never
    // starts looks identical from Kotlin whether the reader thread died, the
    // device sent nothing, or every event landed in the "unknown" bucket.
    static std::atomic<int> ticks{0};
    if ((ticks.fetch_add(1, std::memory_order_relaxed) % 200) == 0) {
        long long d[5] = {0, 0, 0, 0, 0};
        if (xr_diag(t, d) == 0) {
            BRIDGE_LOG(ANDROID_LOG_INFO,
                       "diag: head=%lld phone=%lld readerAlive=%lld errno=%lld other=%lld",
                       d[0], d[1], d[2], d[3], d[4]);
        }
    }
    return xr_pose_fresh(t) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_getPoseStatus(JNIEnv *, jobject) {
    return 0; // Carina-only notion of stability; always "stable" here.
}

// ---- Cached device state ---------------------------------------------------

JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_getBrightness(JNIEnv *, jobject) {
    XrTracker *t = g.tracker.load(std::memory_order_acquire);
    return t != nullptr ? xr_brightness(t) : -1;
}

JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_getVolume(JNIEnv *, jobject) {
    XrTracker *t = g.tracker.load(std::memory_order_acquire);
    return t != nullptr ? xr_volume(t) : -1;
}

JNIEXPORT jint JNICALL Java_com_uxspace_glasses_NativeGlasses_getFilm(JNIEnv *, jobject) {
    // Electrochromic film exists on the Luma models, not on the Pro 2. The
    // device answers "not supported" and sends no command at all.
    return -1;
}

// ---- Extras beyond the vendor bridge ---------------------------------------
//
// The phone as a pointer needs the phone's own orientation, which only the
// Android side can read. These are additions, so nothing in the existing Kotlin
// depends on them yet.

JNIEXPORT void JNICALL Java_com_uxspace_glasses_NativeGlasses_setPhoneQuat(
    JNIEnv *, jobject, jfloat w, jfloat x, jfloat y, jfloat z) {
    XrTracker *t = g.tracker.load(std::memory_order_acquire);
    if (t != nullptr) {
        xr_set_phone_quat(t, w, x, y, z);
    }
}

/// Returns [head w,x,y,z, predicted w,x,y,z, cursor x,y, cursorValid,
/// headSamples, phoneSamples] — one array so the render loop crosses JNI once.
JNIEXPORT jfloatArray JNICALL
Java_com_uxspace_glasses_NativeGlasses_getXrState(JNIEnv *env, jobject) {
    XrState s = {};
    XrTracker *t = g.tracker.load(std::memory_order_acquire);
    if (t != nullptr) {
        xr_state(t, &s);
    }
    float out[13] = {
        s.head_q[0],      s.head_q[1],      s.head_q[2],      s.head_q[3],
        s.predicted_q[0], s.predicted_q[1], s.predicted_q[2], s.predicted_q[3],
        s.cursor_x,       s.cursor_y,       static_cast<float>(s.cursor_valid),
        static_cast<float>(s.head_samples), static_cast<float>(s.phone_samples),
    };
    jfloatArray arr = env->NewFloatArray(13);
    if (arr != nullptr) {
        env->SetFloatArrayRegion(arr, 0, 13, out);
    }
    return arr;
}

/// Fills a caller-owned array instead of allocating one. The polling path runs
/// at the pose rate, so an allocation here is 120 objects a second landing in
/// the same heap the GL thread collects from.
///
/// Returns the number of floats written, or 0 when there is no tracker.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_getXrStateInto(JNIEnv *env, jobject, jfloatArray out) {
    XrTracker *t = g.tracker.load(std::memory_order_acquire);
    if (t == nullptr || out == nullptr || env->GetArrayLength(out) < 13) {
        return 0;
    }
    XrState s = {};
    if (xr_state(t, &s) != 0) {
        return 0;
    }
    const float values[13] = {
        s.head_q[0],      s.head_q[1],      s.head_q[2],      s.head_q[3],
        s.predicted_q[0], s.predicted_q[1], s.predicted_q[2], s.predicted_q[3],
        s.cursor_x,       s.cursor_y,       static_cast<float>(s.cursor_valid),
        static_cast<float>(s.head_samples), static_cast<float>(s.phone_samples),
    };
    env->SetFloatArrayRegion(out, 0, 13, values);
    return 13;
}

/// Switches the panel: 0x31 = 1920x1080 2D, 0x32 = 3840x1080 side-by-side 3D.
/// The video link renegotiates, so the display drops out briefly.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_setDisplayMode(JNIEnv *, jobject, jint mode) {
    XrTracker *t = g.tracker.load(std::memory_order_acquire);
    if (t == nullptr) {
        return -1;
    }
    BRIDGE_LOG(ANDROID_LOG_INFO, "setDisplayMode(0x%02X)", mode);
    return xr_set_display_mode(t, static_cast<unsigned char>(mode));
}

JNIEXPORT void JNICALL
Java_com_uxspace_glasses_NativeGlasses_setLookaheadMs(JNIEnv *, jobject, jfloat ms) {
    XrTracker *t = g.tracker.load(std::memory_order_acquire);
    if (t != nullptr) {
        xr_set_lookahead_ms(t, ms);
    }
}

JNIEXPORT void JNICALL
Java_com_uxspace_glasses_NativeGlasses_setPointerDistance(JNIEnv *, jobject, jfloat d) {
    XrTracker *t = g.tracker.load(std::memory_order_acquire);
    if (t != nullptr) {
        xr_set_distance(t, d);
    }
}

// ---- Panorama ---------------------------------------------------------------
//
// Every one of these writes into a direct java.nio buffer. GetDirectBufferAddress
// hands back the same memory the GL driver will read, so the mesh goes from Rust
// to glBufferData without a copy and the per-frame matrix costs one JNI call and
// sixteen stores. Passing a jfloatArray instead would add a copy in each
// direction, per frame, for the matrix.

/// Reads an optional [top, bottom, left, right] bounds array. A null array
/// means the whole sphere, which the driver also accepts as a null pointer.
class BoundsArg {
public:
    BoundsArg(JNIEnv *env, jfloatArray bounds) : env_(env), array_(bounds) {
        if (array_ != nullptr && env_->GetArrayLength(array_) >= 4) {
            values_ = env_->GetFloatArrayElements(array_, nullptr);
        }
    }
    ~BoundsArg() {
        if (values_ != nullptr) {
            env_->ReleaseFloatArrayElements(array_, values_, JNI_ABORT);
        }
    }
    BoundsArg(const BoundsArg &) = delete;
    BoundsArg &operator=(const BoundsArg &) = delete;

    const float *get() const { return values_; }

private:
    JNIEnv *env_;
    jfloatArray array_;
    jfloat *values_ = nullptr;
};

/// Writes the bounds a projection stands for into a four-element array.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_panoBounds(
        JNIEnv *env, jobject, jint projection, jfloatArray out) {
    if (out == nullptr || env->GetArrayLength(out) < 4) {
        return -1;
    }
    float b[4];
    if (xr_pano_bounds(static_cast<unsigned int>(projection), b) != 0) {
        return -1;
    }
    env->SetFloatArrayRegion(out, 0, 4, b);
    return 0;
}

/// Writes an inside-out sphere as interleaved [x, y, z, u, v] into a direct
/// FloatBuffer. Returns the vertex count, or -1 if the buffer is too small.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_panoMesh(
        JNIEnv *env, jobject, jint rings, jint sectors, jfloat radius, jfloatArray bounds,
        jobject out) {
    if (out == nullptr) {
        return -1;
    }
    auto *ptr = static_cast<float *>(env->GetDirectBufferAddress(out));
    const jlong cap = env->GetDirectBufferCapacity(out);
    if (ptr == nullptr || cap <= 0) {
        BRIDGE_LOG(ANDROID_LOG_ERROR, "panoMesh: buffer is not direct");
        return -1;
    }
    BoundsArg b(env, bounds);
    return xr_pano_mesh(static_cast<unsigned int>(rings), static_cast<unsigned int>(sectors),
                        radius, b.get(), ptr, static_cast<size_t>(cap) / sizeof(float));
}

/// Writes the matching triangle indices into a direct ShortBuffer. Returns the
/// index count, or -1.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_panoIndices(
        JNIEnv *env, jobject, jint rings, jint sectors, jfloatArray bounds, jobject out) {
    if (out == nullptr) {
        return -1;
    }
    auto *ptr = static_cast<unsigned short *>(env->GetDirectBufferAddress(out));
    const jlong cap = env->GetDirectBufferCapacity(out);
    if (ptr == nullptr || cap <= 0) {
        BRIDGE_LOG(ANDROID_LOG_ERROR, "panoIndices: buffer is not direct");
        return -1;
    }
    BoundsArg b(env, bounds);
    return xr_pano_indices(static_cast<unsigned int>(rings), static_cast<unsigned int>(sectors),
                           b.get(), ptr, static_cast<size_t>(cap) / sizeof(unsigned short));
}

JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_cubeVertexCount(JNIEnv *, jobject, jint cells) {
    return static_cast<jint>(xr_cube_vertex_count(static_cast<unsigned int>(cells)));
}

JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_cubeIndexCount(JNIEnv *, jobject, jint cells) {
    return static_cast<jint>(xr_cube_index_count(static_cast<unsigned int>(cells)));
}

/// Writes the six faces of a cube map into a direct FloatBuffer.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_cubeMesh(
        JNIEnv *env, jobject, jint cells, jfloat radius, jfloat padding, jobject out) {
    if (out == nullptr) {
        return -1;
    }
    auto *ptr = static_cast<float *>(env->GetDirectBufferAddress(out));
    const jlong cap = env->GetDirectBufferCapacity(out);
    if (ptr == nullptr || cap <= 0) {
        BRIDGE_LOG(ANDROID_LOG_ERROR, "cubeMesh: buffer is not direct");
        return -1;
    }
    return xr_cube_mesh(static_cast<unsigned int>(cells), radius, padding, ptr,
                        static_cast<size_t>(cap) / sizeof(float));
}

JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_cubeIndices(
        JNIEnv *env, jobject, jint cells, jobject out) {
    if (out == nullptr) {
        return -1;
    }
    auto *ptr = static_cast<unsigned short *>(env->GetDirectBufferAddress(out));
    const jlong cap = env->GetDirectBufferCapacity(out);
    if (ptr == nullptr || cap <= 0) {
        BRIDGE_LOG(ANDROID_LOG_ERROR, "cubeIndices: buffer is not direct");
        return -1;
    }
    return xr_cube_indices(static_cast<unsigned int>(cells), ptr,
                           static_cast<size_t>(cap) / sizeof(unsigned short));
}

/// Writes the three channel weights one eye survives in an anaglyph frame.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_anaglyphMix(
        JNIEnv *env, jobject, jint pair, jint eye, jfloatArray out) {
    if (out == nullptr || env->GetArrayLength(out) < 3) {
        return -1;
    }
    float mix[3];
    if (xr_anaglyph_mix(static_cast<unsigned int>(pair), eye, mix) != 0) {
        return -1;
    }
    env->SetFloatArrayRegion(out, 0, 3, mix);
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_panoVertexCount(JNIEnv *, jobject, jint rings,
                                                       jint sectors) {
    return static_cast<jint>(
            xr_pano_vertex_count(static_cast<unsigned int>(rings),
                                 static_cast<unsigned int>(sectors)));
}

JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_panoIndexCount(JNIEnv *, jobject, jint rings,
                                                      jint sectors) {
    return static_cast<jint>(
            xr_pano_index_count(static_cast<unsigned int>(rings),
                                static_cast<unsigned int>(sectors)));
}

/// Writes [u_scale, u_offset, v_scale, v_offset] for one eye's half of a stereo
/// frame. `layout`: 0 mono, 1 over-under, 2 side-by-side. `eye`: 0 left, 1 right.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_panoUvWindow(
        JNIEnv *env, jobject, jint layout, jint eye, jfloatArray out) {
    if (out == nullptr || env->GetArrayLength(out) < 4) {
        return -1;
    }
    float w[4];
    if (xr_pano_uv(static_cast<unsigned int>(layout), eye, w) != 0) {
        return -1;
    }
    env->SetFloatArrayRegion(out, 0, 4, w);
    return 0;
}

/// Writes the panorama view-projection into a direct FloatBuffer, ready for
/// glUniformMatrix4fv. Both eyes share the matrix — stereo 360 depth lives in
/// the texture, not in a camera offset.
///
/// The head orientation is passed in rather than read back from the tracker, so
/// the panorama and everything drawn over it come from the same sample.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_panoViewProjection(
        JNIEnv *env, jobject, jfloat w, jfloat x, jfloat y, jfloat z, jfloat fovYDegrees,
        jfloat aspect, jfloat eyeOffset, jobject out) {
    if (out == nullptr) {
        return -1;
    }
    auto *ptr = static_cast<float *>(env->GetDirectBufferAddress(out));
    const jlong cap = env->GetDirectBufferCapacity(out);
    if (ptr == nullptr || cap < static_cast<jlong>(16 * sizeof(float))) {
        return -1;
    }
    return xr_pano_mvp(w, x, y, z, fovYDegrees, aspect, eyeOffset, ptr);
}

/// The vertical field of view to render at, for these glasses and this panel.
JNIEXPORT jfloat JNICALL
Java_com_uxspace_glasses_NativeGlasses_fovForPanel(
        JNIEnv *, jobject, jint productId, jint eyeWidth, jint eyeHeight) {
    return xr_fov_for_panel(static_cast<unsigned int>(productId),
                            static_cast<unsigned int>(eyeWidth),
                            static_cast<unsigned int>(eyeHeight));
}

/// What is known about these glasses: [diagonalFovDegrees, eyeWidth, eyeHeight,
/// verified]. Returns 0 when recognised, -1 when not.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_modelInfo(
        JNIEnv *env, jobject, jint productId, jfloatArray out) {
    if (out == nullptr || env->GetArrayLength(out) < 4) {
        return -1;
    }
    float info[4];
    if (xr_model_info(static_cast<unsigned int>(productId), info) != 0) {
        return -1;
    }
    env->SetFloatArrayRegion(out, 0, 4, info);
    return 0;
}

/// The vertical field of view a zoom factor corresponds to. Pure maths, no
/// device needed.
JNIEXPORT jfloat JNICALL
Java_com_uxspace_glasses_NativeGlasses_panoFovForZoom(JNIEnv *, jobject, jfloat zoom) {
    return xr_pano_fov_for_zoom(zoom);
}

/// Extrapolates an orientation forward by dtSeconds at the head's current
/// angular rate. Writes [w, x, y, z] into `out`; returns 0 on success.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_predict(
        JNIEnv *env, jobject, jfloat w, jfloat x, jfloat y, jfloat z, jfloat dtSeconds,
        jfloatArray out) {
    XrTracker *t = g.tracker.load(std::memory_order_acquire);
    if (t == nullptr || out == nullptr || env->GetArrayLength(out) < 4) {
        return -1;
    }
    float q[4];
    if (xr_predict(t, w, x, y, z, dtSeconds, q) != 0) {
        return -1;
    }
    env->SetFloatArrayRegion(out, 0, 4, q);
    return 0;
}

/// The head's current angular rate in radians per second, body frame, as
/// [x, y, z]. Zero when nothing is tracking.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_getAngularRate(JNIEnv *env, jobject, jfloatArray out) {
    XrTracker *t = g.tracker.load(std::memory_order_acquire);
    if (t == nullptr || out == nullptr || env->GetArrayLength(out) < 3) {
        return -1;
    }
    float rate[3];
    if (xr_angular_rate(t, rate) != 0) {
        return -1;
    }
    env->SetFloatArrayRegion(out, 0, 3, rate);
    return 0;
}

// ---- Mesh projections -------------------------------------------------------
//
// The handle is passed to Kotlin as a jlong. It lives from meshParse to
// meshFree, which bracket a single upload on the GL thread.

// ---- Library listings -------------------------------------------------------
//
// A browse answers with hundreds of entries and each needs a geometry and a
// packing. Crossing once with all of them, in buffers neither side copies,
// replaces a call and a string allocation per entry.

/// Reads a whole listing. `blob` holds the names and stereo modes end to end,
/// `meta` describes each entry in six words, `out` receives one packed format
/// each. Returns the number written, or -1.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_libraryInfer(
        JNIEnv *env, jobject, jobject blob, jobject meta, jobject out) {
    if (meta == nullptr || out == nullptr) {
        return -1;
    }
    // An empty listing has an empty blob, and a zero-capacity direct buffer is
    // allowed to report a null address.
    auto *blobPtr = blob == nullptr
            ? nullptr
            : static_cast<const unsigned char *>(env->GetDirectBufferAddress(blob));
    const jlong blobCap = blob == nullptr ? 0 : env->GetDirectBufferCapacity(blob);
    auto *metaPtr = static_cast<const unsigned int *>(env->GetDirectBufferAddress(meta));
    const jlong metaCap = env->GetDirectBufferCapacity(meta);
    auto *outPtr = static_cast<unsigned int *>(env->GetDirectBufferAddress(out));
    const jlong outCap = env->GetDirectBufferCapacity(out);
    if (metaPtr == nullptr || outPtr == nullptr || metaCap <= 0 || outCap <= 0 || blobCap < 0) {
        BRIDGE_LOG(ANDROID_LOG_ERROR, "libraryInfer: buffers are not direct");
        return -1;
    }
    return xr_library_infer(blobPtr, static_cast<size_t>(blobCap), metaPtr,
                            static_cast<size_t>(metaCap) / sizeof(unsigned int), outPtr,
                            static_cast<size_t>(outCap) / sizeof(unsigned int));
}

/// Starts measuring how this machine should split a listing. Returns at once.
JNIEXPORT void JNICALL
Java_com_uxspace_glasses_NativeGlasses_libraryCalibrate(JNIEnv *, jobject) {
    xr_library_calibrate();
}

/// Measures again, because the machine may have become a different one: the
/// charger went in or out, or the battery saver changed its mind. The previous
/// answer stays in force until the new one lands.
JNIEXPORT void JNICALL
Java_com_uxspace_glasses_NativeGlasses_libraryRecalibrate(JNIEnv *, jobject) {
    xr_library_recalibrate();
}

/// Writes [threshold, threads] — what the measurement decided, or
/// [0xFFFFFFFF, 1] until it has. For logging.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_libraryPlan(JNIEnv *env, jobject, jintArray out) {
    if (out == nullptr || env->GetArrayLength(out) < 2) {
        return -1;
    }
    unsigned int plan[2];
    if (xr_library_plan(plan) != 0) {
        return -1;
    }
    jint values[2] = {static_cast<jint>(plan[0]), static_cast<jint>(plan[1])};
    env->SetIntArrayRegion(out, 0, 2, values);
    return 0;
}

/// Reads one entry, for the caller that has exactly one.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_libraryInferOne(
        JNIEnv *env, jobject, jbyteArray name, jbyteArray mode, jint width, jint height) {
    jbyte *namePtr = name == nullptr ? nullptr : env->GetByteArrayElements(name, nullptr);
    jbyte *modePtr = mode == nullptr ? nullptr : env->GetByteArrayElements(mode, nullptr);
    const jsize nameLen = name == nullptr ? 0 : env->GetArrayLength(name);
    const jsize modeLen = mode == nullptr ? 0 : env->GetArrayLength(mode);

    const unsigned int packed = xr_library_infer_one(
            reinterpret_cast<const unsigned char *>(namePtr), static_cast<size_t>(nameLen),
            reinterpret_cast<const unsigned char *>(modePtr), static_cast<size_t>(modeLen),
            static_cast<unsigned int>(width), static_cast<unsigned int>(height));

    if (namePtr != nullptr) env->ReleaseByteArrayElements(name, namePtr, JNI_ABORT);
    if (modePtr != nullptr) env->ReleaseByteArrayElements(mode, modePtr, JNI_ABORT);
    return static_cast<jint>(packed);
}

/// What a `proj` box says: 0 equirectangular, 1 cube map, 2 a carried mesh,
/// -1 for absent or malformed. Four floats of detail come back in `out`.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_projKind(
        JNIEnv *env, jobject, jbyteArray data, jfloatArray out) {
    if (data == nullptr || out == nullptr || env->GetArrayLength(out) < 4) {
        return -1;
    }
    const jsize len = env->GetArrayLength(data);
    if (len <= 0) {
        return -1;
    }
    jbyte *bytes = env->GetByteArrayElements(data, nullptr);
    if (bytes == nullptr) {
        return -1;
    }
    float detail[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const int kind = xr_proj_kind(reinterpret_cast<const unsigned char *>(bytes),
                                  static_cast<size_t>(len), detail);
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    if (kind >= 0) {
        env->SetFloatArrayRegion(out, 0, 4, detail);
    }
    return kind;
}

/// Parses a `proj` box. Returns a handle, or 0 if there is no mesh in it.
JNIEXPORT jlong JNICALL
Java_com_uxspace_glasses_NativeGlasses_meshParse(JNIEnv *env, jobject, jbyteArray data) {
    if (data == nullptr) {
        return 0;
    }
    const jsize len = env->GetArrayLength(data);
    if (len <= 0) {
        return 0;
    }
    jbyte *bytes = env->GetByteArrayElements(data, nullptr);
    if (bytes == nullptr) {
        return 0;
    }
    XrMesh *mesh = xr_mesh_parse(reinterpret_cast<const unsigned char *>(bytes),
                                 static_cast<size_t>(len));
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    BRIDGE_LOG(ANDROID_LOG_INFO, "meshParse: %d bytes -> %s", len, mesh ? "mesh" : "no mesh");
    return reinterpret_cast<jlong>(mesh);
}

JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_meshCount(JNIEnv *, jobject, jlong handle) {
    return xr_mesh_count(reinterpret_cast<const XrMesh *>(handle));
}

JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_meshSubmeshCount(JNIEnv *, jobject, jlong handle,
                                                        jint mesh) {
    return xr_mesh_submesh_count(reinterpret_cast<const XrMesh *>(handle), mesh);
}

/// Writes [drawMode, vertexCount, textureId].
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_meshSubmeshInfo(
        JNIEnv *env, jobject, jlong handle, jint mesh, jint submesh, jintArray out) {
    if (out == nullptr || env->GetArrayLength(out) < 3) {
        return -1;
    }
    int info[3];
    if (xr_mesh_submesh_info(reinterpret_cast<const XrMesh *>(handle), mesh, submesh, info) != 0) {
        return -1;
    }
    env->SetIntArrayRegion(out, 0, 3, info);
    return 0;
}

/// Copies one sub-mesh as interleaved [x, y, z, u, v] into a direct buffer.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_meshCopy(
        JNIEnv *env, jobject, jlong handle, jint mesh, jint submesh, jobject out) {
    if (out == nullptr) {
        return -1;
    }
    auto *ptr = static_cast<float *>(env->GetDirectBufferAddress(out));
    const jlong cap = env->GetDirectBufferCapacity(out);
    if (ptr == nullptr || cap <= 0) {
        return -1;
    }
    return xr_mesh_copy(reinterpret_cast<const XrMesh *>(handle), mesh, submesh, ptr,
                        static_cast<size_t>(cap) / sizeof(float));
}

JNIEXPORT void JNICALL
Java_com_uxspace_glasses_NativeGlasses_meshFree(JNIEnv *, jobject, jlong handle) {
    xr_mesh_free(reinterpret_cast<XrMesh *>(handle));
}

/// Writes a screen standing in the room into a direct FloatBuffer, as an
/// interleaved [x, y, z, u, v] triangle strip. Returns the vertex count, or -1.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_screenMesh(
        JNIEnv *env, jobject, jint segments, jfloat distance, jfloat widthDegrees, jfloat aspect,
        jfloat curvature, jobject out) {
    if (out == nullptr) {
        return -1;
    }
    auto *ptr = static_cast<float *>(env->GetDirectBufferAddress(out));
    const jlong cap = env->GetDirectBufferCapacity(out);
    if (ptr == nullptr || cap <= 0) {
        return -1;
    }
    return xr_screen_mesh(static_cast<unsigned int>(segments), distance, widthDegrees, aspect,
                          curvature, ptr, static_cast<size_t>(cap) / sizeof(float));
}

JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_screenVertexCount(JNIEnv *, jobject, jint segments) {
    return static_cast<jint>(xr_screen_vertex_count(static_cast<unsigned int>(segments)));
}

/// The display mode the glasses reported when the session opened, or -1.
///
/// Read once, at open time, because the reader thread owns the transport from
/// then on. Enough to know which state the panel came back in after a restart,
/// which is the thing that cannot be inferred from the display: mid-negotiation
/// it is whatever it happens to be.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_getDisplayMode(JNIEnv *, jobject) {
    XrTracker *t = g.tracker.load(std::memory_order_acquire);
    return t == nullptr ? -1 : xr_display_mode(t);
}

/// The pose, written into a caller-owned array of at least seven floats.
///
/// Returns the number written, or 0 when nothing is tracking. The allocating
/// form is left for callers that read a pose occasionally; anything running at
/// the pose rate should use this. A hundred and twenty arrays a second land in
/// the same heap the GL thread collects from, and the collection is felt as a
/// dropped frame rather than seen as a cost.
JNIEXPORT jint JNICALL
Java_com_uxspace_glasses_NativeGlasses_getPoseInto(JNIEnv *env, jobject, jfloatArray out) {
    XrTracker *t = g.tracker.load(std::memory_order_acquire);
    if (t == nullptr || out == nullptr || env->GetArrayLength(out) < 7) {
        return 0;
    }
    float pose[7];
    if (xr_pose7(t, pose) != 0) {
        return 0;
    }
    env->SetFloatArrayRegion(out, 0, 7, pose);
    return 7;
}

} // extern "C"
