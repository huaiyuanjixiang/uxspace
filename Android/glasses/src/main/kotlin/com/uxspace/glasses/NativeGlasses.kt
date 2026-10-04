package com.uxspace.glasses

/**
 * Kotlin side of the JNI bridge to the native VITURE SDK.
 *
 * Every method is implemented in `glasses_bridge.cpp`. This fork forwards to the open
 * viture-v2 driver rather than VITURE's proprietary `libglasses.so`. The
 * library is loaded the first time this object is touched; do not reference it unless the
 * native build is wired up and the glasses are connected.
 *
 * Threading: drive the lifecycle from a single background thread. Pose can be read from any
 * thread — [getPose] is internally locked.
 */
object NativeGlasses {

    init {
        System.loadLibrary("glasses_bridge")
    }

    /** Version string of the bundled `libglasses.so`. */
    external fun getVersion(): String

    // Device lifecycle -------------------------------------------------------

    /** Open the glasses. [fd] is a USB file descriptor; [pid] the USB product id. */
    external fun create(pid: Int, fd: Int): Boolean

    /** [DEVICE_TYPE_CARINA] for VITURE Carina, or -1 for older Gen1/2 hardware. */
    external fun getDeviceType(): Int

    external fun registerStateCallback(): Int
    external fun initialize(): Int
    external fun start(): Int
    external fun stop(): Int
    external fun shutdown(): Int
    external fun destroy()

    // Gen1/Gen2 head tracking (host-side IMU) --------------------------------

    external fun openImu(): Int

    /**
     * Whether this build can run with no glasses attached.
     *
     * Asked rather than assumed, so a build without the simulation says so
     * instead of failing to open something that looks like it should have
     * opened.
     */
    external fun hasSimulation(): Boolean

    /**
     * Opens a tracker with nothing on the other end of the cable.
     *
     * Everything above the wire runs against it — the reader thread, the ring,
     * recentring, rate estimation, prediction, and every command the host sends
     * — and what it answers is what the hardware was measured answering, down
     * to the 118.9 Hz pose rate and the display modes the panel really offers
     * in each of its own modes.
     *
     * The glasses are one cable and one battery. This is how the rest of the
     * work carries on when either runs out, and how the cases hardware cannot
     * be asked for get tested: a head turning at exactly 45 degrees a second,
     * a pose stream that dies mid-session.
     *
     * [motion] picks what the simulated head does: 0 still, 1 turning steadily,
     * 2 turning and then stopping.
     */
    external fun openSimulatedImu(motion: Int): Int
    external fun closeImu(): Int

    // Native-DOF devices (on-glasses tracking) -------------------------------

    /** True if [pid]'s product tracks head motion on the glasses themselves. */
    external fun isProductSupportNativeDof(pid: Int): Boolean
    external fun setupNativeDofDevice()
    external fun nativeRecenterDof(): Int

    // Carina (VIO tracking) --------------------------------------------------

    /** Call after [create] and before [initialize]. */
    external fun setDofTypeCarina(is6dof: Boolean): Int
    external fun startCarinaPollThread()
    external fun stopCarinaPollThread()
    external fun carinaStopForModeSwitch(): Boolean
    external fun resetOriginCarina(): Int
    external fun resetPoseCarina(): Int

    // Pose -------------------------------------------------------------------

    /**
     * The latest pose as 7 floats.
     * Gen1/2: `[roll, pitch, yaw, qw, qx, qy, qz]`; Carina: `[px, py, pz, qw, qx, qy, qz]`.
     */
    external fun getPose(): FloatArray

    /**
     * The same, written into a caller-owned array of at least seven floats.
     * Returns the number written, or 0 when nothing is tracking.
     *
     * Preferred anywhere running at the pose rate. The allocating form puts a
     * hundred and twenty arrays a second into the heap the GL thread collects
     * from, and the collection is felt as a dropped frame.
     */
    external fun getPoseInto(out: FloatArray): Int

    /** True once, when a pose has arrived since the previous call. */
    external fun isPoseFresh(): Boolean

    /** Carina 6DOF only: `0` = stable, `1` = unstable. */
    external fun getPoseStatus(): Int

    // Cached device state ----------------------------------------------------

    external fun getBrightness(): Int
    external fun getVolume(): Int
    external fun getFilm(): Int

    /**
     * The display mode the glasses reported when the session opened, or -1.
     *
     * Read once, at open time. Enough to know which state the panel came back in
     * after a restart — which is the one thing the display itself cannot say,
     * because mid-negotiation it is whatever it happens to be.
     */
    external fun getDisplayMode(): Int

    // Phone as a pointer ------------------------------------------------------
    //
    // Additions of this fork. The glasses give the head, the phone gives the
    // hand; the driver combines them, because a head-locked image needs the
    // phone's orientation expressed in the head's frame.

    /** Feed the phone orientation, e.g. from TYPE_GAME_ROTATION_VECTOR. */
    external fun setPhoneQuat(w: Float, x: Float, y: Float, z: Float)

    /**
     * One snapshot for the render loop, 13 floats:
     * `[headQ w,x,y,z, predictedQ w,x,y,z, cursorX, cursorY, cursorValid,
     * headSamples, phoneSamples]`.
     */
    external fun getXrState(): FloatArray

    /**
     * Same snapshot, written into a caller-owned array of at least 13 floats.
     * Returns the number written, or 0 when tracking is not running.
     *
     * Preferred over [getXrState] anywhere that runs at the pose or frame rate:
     * allocating per sample puts garbage in the same heap the GL thread collects
     * from, which is felt as micro-stutter rather than seen as cost.
     */
    external fun getXrStateInto(out: FloatArray): Int

    /**
     * Extrapolates orientation `[w, x, y, z]` forward by [dtSeconds] at the head's
     * current angular rate, writing `[w, x, y, z]` into [out]. Returns 0 on success.
     *
     * This is what closes the gap between when a pose was sampled and when the
     * frame built from it reaches the panel. Uncorrected, that gap shows up as
     * the whole scene sliding a little behind every head turn.
     *
     * The orientation is passed in rather than read here so a renderer can
     * predict from the pose it has already recentred; body-frame angular rate is
     * unaffected by recentring.
     */
    external fun predict(
        w: Float,
        x: Float,
        y: Float,
        z: Float,
        dtSeconds: Float,
        out: FloatArray,
    ): Int

    /**
     * The head's current angular rate in radians per second, body frame, written
     * into [out] as `[x, y, z]`. Returns 0 on success.
     *
     * Measured from the device's raw stream where that runs, and differentiated
     * from the pose stream otherwise.
     */
    external fun getAngularRate(out: FloatArray): Int

    /** How far ahead to extrapolate the head pose, in milliseconds. */
    external fun setLookaheadMs(ms: Float)

    /** Virtual screen distance; larger means a less sensitive pointer. */
    external fun setPointerDistance(distance: Float)

    /**
     * Switches the panel. `0x31` is 1920×1080 2D; `0x32` is 3840×1080 side-by-side
     * 3D, where the glasses send the left half of the frame to the left eye and
     * the right half to the right, each a full 1920×1080.
     *
     * The video link renegotiates, so the display drops out for a moment and the
     * host may pick a different mode when it returns.
     */
    external fun setDisplayMode(mode: Int): Int

    // 360° panorama ----------------------------------------------------------
    //
    // Geometry and camera maths for equirectangular playback, computed in the
    // driver. The mesh and the matrices go through direct java.nio buffers, so
    // nothing is copied between here and the GL driver — see PanoramaLayer.

    /** Vertices [panoMesh] will write for this tessellation. */
    external fun panoVertexCount(rings: Int, sectors: Int): Int

    /**
     * How many indices to make room for. An upper bound, not an exact count:
     * how many of the polar triangles collapse depends on the bounds. Draw with
     * what [panoIndices] returns.
     */
    external fun panoIndexCount(rings: Int, sectors: Int): Int

    /**
     * Writes `[top, bottom, left, right]` — the proportion of the sphere a
     * projection leaves uncovered on each side. Returns 0, or -1 for a
     * projection that is not a patch of a sphere.
     *
     * Converting here, once, means everything downstream deals in bounds. A
     * file that states its own coverage in an `equi` box and one that merely
     * says "180" then travel the same path instead of two.
     */
    external fun panoBounds(projection: Int, out: FloatArray): Int

    /**
     * Writes an inside-out UV sphere into [out] as interleaved
     * `[x, y, z, u, v]`, ready for `glBufferData`. Returns the vertex count, or
     * -1 if [out] is not direct or is too small.
     *
     * The sphere is wound counter-clockwise as seen from its centre, so back-face
     * culling can stay enabled while the panorama is drawn.
     *
     * [bounds] is `[top, bottom, left, right]` as [panoBounds] writes them, or
     * null for the whole sphere.
     */
    external fun panoMesh(
        rings: Int,
        sectors: Int,
        radius: Float,
        bounds: FloatArray?,
        out: java.nio.Buffer,
    ): Int

    /**
     * Triangle indices for the same tessellation and bounds. Returns the count,
     * which is larger for a cropped sphere than a full one — the polar rows of
     * a full sphere collapse to a point and half of each is dropped.
     */
    external fun panoIndices(
        rings: Int,
        sectors: Int,
        bounds: FloatArray?,
        out: java.nio.Buffer,
    ): Int

    /** Vertices [cubeMesh] will write for this tessellation. */
    external fun cubeVertexCount(cells: Int): Int

    /** Indices [cubeIndices] will write for this tessellation. */
    external fun cubeIndexCount(cells: Int): Int

    /**
     * Writes the six faces of a cube map into [out], in the order and
     * orientation the `cbmp` box's layout 0 packs them. Returns the vertex
     * count, or -1.
     *
     * [padding] is the box's pixel padding divided by the pixel width of one
     * face — the caller has the frame's dimensions and the driver does not.
     */
    external fun cubeMesh(
        cells: Int,
        radius: Float,
        padding: Float,
        out: java.nio.Buffer,
    ): Int

    /** Triangle indices for the same tessellation. Returns the count, or -1. */
    external fun cubeIndices(cells: Int, out: java.nio.Buffer): Int

    /**
     * Writes `[uScale, uOffset, vScale, vOffset]` — the half of a stereo frame
     * one eye samples. [layout] is [PANO_MONO], [PANO_OVER_UNDER] or
     * [PANO_SIDE_BY_SIDE]; [eye] is 0 for left, 1 for right. Mono yields the
     * identity, so one shader serves both cases.
     *
     * [PANO_ANAGLYPH] and [PANO_ROW_INTERLEAVED] also yield the identity:
     * both eyes cover the whole frame and what separates them is resolved per
     * pixel, not by a window.
     */
    external fun panoUvWindow(layout: Int, eye: Int, out: FloatArray): Int

    /**
     * Writes the three colour-channel weights one eye's picture survives in,
     * for an anaglyph frame. [pair] is [ANAGLYPH_RED_CYAN],
     * [ANAGLYPH_GREEN_MAGENTA] or [ANAGLYPH_YELLOW_BLUE]; [eye] is 0 for left.
     * Returns 0, or -1.
     */
    external fun anaglyphMix(pair: Int, eye: Int, out: FloatArray): Int

    /**
     * Writes the panorama view-projection for head orientation `[w, x, y, z]`
     * into [out], column-major, ready for `glUniformMatrix4fv`. Returns 0 on
     * success.
     *
     * [out] must be a direct `ByteBuffer` of at least 64 bytes, not a typed view
     * of one: JNI reports a buffer's capacity in elements, so a `FloatBuffer`
     * looks four times too small to the capacity check on the other side.
     *
     * Both eyes share this matrix. Stereo 360° depth is encoded in the two images
     * of the frame; displacing the camera inside the sphere would add parallax
     * against geometry that is not really there and fight the footage.
     *
     * The orientation is passed in rather than read back from the tracker, so
     * that the panorama and everything drawn over it come from one sample: two
     * reads a frame apart shear the video against the windows in front of it.
     */
    external fun panoViewProjection(
        w: Float,
        x: Float,
        y: Float,
        z: Float,
        fovYDegrees: Float,
        aspect: Float,
        eyeOffset: Float,
        out: java.nio.Buffer,
    ): Int

    /** Vertices [screenMesh] will write for this many segments. */
    external fun screenVertexCount(segments: Int): Int

    /**
     * Writes a screen standing in the room into [out] as an interleaved
     * `[x, y, z, u, v]` triangle strip. Returns the vertex count, or -1.
     *
     * [widthDegrees] is how wide the screen should *appear*, which is the thing
     * a person has an opinion about; [distance] only decides how far away it
     * feels. [aspect] is the picture's shape after the stereo split — a
     * side-by-side film that is 3840×1080 on disc is 16:9 per eye.
     * [curvature] runs from 0 for a plane to 1 for an arc every part of which is
     * the same distance away.
     */
    external fun screenMesh(
        segments: Int,
        distance: Float,
        widthDegrees: Float,
        aspect: Float,
        curvature: Float,
        out: java.nio.Buffer,
    ): Int

    /**
     * The vertical field of view, in degrees, for a zoom factor. Zooming a
     * panorama narrows the view angle rather than moving the camera, since the
     * image is at infinity.
     */
    external fun panoFovForZoom(zoom: Float): Float

    /**
     * The vertical field of view to render at, in degrees, for these glasses
     * and this panel.
     *
     * The two halves of the answer come from different places, deliberately.
     * The panel's **shape** is read from the display, which knows it. The
     * **angle** comes from a table keyed by USB product id, because nothing on
     * the wire reports it and no sensor can see the virtual image — a field of
     * view is a property of lenses.
     *
     * Glasses this build has never met get the Pro 2's angle with their own
     * shape. That is wrong by a few degrees of scale rather than by a stretch,
     * which is the mistake nobody notices instead of the one everybody does.
     * The zoom control corrects the rest by hand.
     */
    external fun fovForPanel(productId: Int, eyeWidth: Int, eyeHeight: Int): Float

    /**
     * What is known about these glasses: `[diagonalFovDegrees, eyeWidth,
     * eyeHeight, verified]`, where verified is 1 when the numbers were checked
     * against hardware and 0 when they come from the manufacturer's published
     * specifications. Returns 0 when the glasses are recognised, -1 when not.
     */
    external fun modelInfo(productId: Int, out: FloatArray): Int

    // Mesh projections --------------------------------------------------------
    //
    // Fisheye rigs ship the mapping itself rather than an equirectangular frame:
    // a triangle mesh whose vertices carry both a direction and the texture
    // coordinate belonging there. Played back from that mesh the projection is
    // exact, because the file states it instead of the player assuming a lens.
    //
    // The handle lives from [meshParse] to [meshFree], which bracket one upload.

    // Library listings --------------------------------------------------------
    //
    // A browse answers with hundreds of entries, each needing a geometry and a
    // packing worked out from its name, its stereo mode and its frame size. The
    // whole listing crosses in one call, in buffers neither side copies.

    /**
     * Reads a listing. [blob] holds every name and stereo mode end to end as
     * UTF-8; [meta] describes each entry in [LIBRARY_META_WORDS] words —
     * name offset, name length, mode offset, mode length, width, height; [out]
     * receives one packed format each.
     *
     * Returns the number written, or -1. Nothing is written at all if the
     * description is inconsistent: a listing read halfway cannot be told from
     * one read fully.
     *
     * All three must be direct buffers in native byte order.
     */
    external fun libraryInfer(
        blob: java.nio.Buffer?,
        meta: java.nio.Buffer,
        out: java.nio.Buffer,
    ): Int

    /**
     * Starts measuring, on a thread of its own, whether splitting a listing
     * across cores pays on this machine. Returns at once.
     *
     * Worth calling once at startup and never again. Until it finishes,
     * listings are read on one core, which on the hardware this was written for
     * is the right answer anyway — but the next generation gets to disagree
     * without anybody editing a constant.
     */
    external fun libraryCalibrate()

    /**
     * Measures again, because the machine may have become a different one.
     *
     * A phone on a charger and a phone at eight per cent with the battery saver
     * on schedule differently and are willing to run at different speeds, so a
     * plan measured under one of them is a statement about that one. Worth
     * re-asking whenever the power situation changes.
     *
     * On the hardware this was written for the answer does not in fact move —
     * measured both ways, within a few per cent — which was worth finding out
     * rather than assuming in either direction. The re-measurement stays
     * because that is a fact about one phone.
     *
     * Returns at once; the previous answer stays in force until the new one
     * lands, and calls arriving while a measurement is running are dropped.
     */
    external fun libraryRecalibrate()

    /**
     * Writes `[threshold, threads]` — what that measurement decided, or
     * `[-1, 1]` until it has. For logging; nothing depends on it.
     */
    external fun libraryPlan(out: IntArray): Int

    /** The same for a single entry, packed the same way. */
    external fun libraryInferOne(
        name: ByteArray?,
        stereoMode: ByteArray?,
        width: Int,
        height: Int,
    ): Int

    /**
     * What a `proj` box says, without building anything: [PROJ_EQUIRECT],
     * [PROJ_CUBEMAP], [PROJ_MESH], or -1 when it says nothing readable.
     *
     * [out] receives four floats whose meaning follows the return value:
     * `[top, bottom, left, right]` bounds for equirectangular, `[layout,
     * padding, 0, 0]` for a cube map, zeroes for a mesh. The mesh itself still
     * comes from [meshParse]; this is the cheap question asked first.
     */
    external fun projKind(projectionData: ByteArray, out: FloatArray): Int

    /**
     * Parses a `proj` box — `Format.projectionData` from ExoPlayer. Returns a
     * handle, or 0 when the box carries no mesh this can read.
     */
    external fun meshParse(projectionData: ByteArray): Long

    /** 1 when both eyes share a mesh, 2 when the file gives each its own. */
    external fun meshCount(handle: Long): Int

    /** How many drawable runs one mesh has. */
    external fun meshSubmeshCount(handle: Long, mesh: Int): Int

    /**
     * Writes `[drawMode, vertexCount, textureId]` into [out]. `drawMode` is
     * 0 triangles, 1 strip, 2 fan. Returns 0 on success.
     */
    external fun meshSubmeshInfo(handle: Long, mesh: Int, submesh: Int, out: IntArray): Int

    /**
     * Copies one sub-mesh into [out] as interleaved `[x, y, z, u, v]` — the same
     * layout the sphere uses, so one attribute setup serves both. Returns the
     * vertex count, or -1 if [out] is not direct or is too small.
     */
    external fun meshCopy(handle: Long, mesh: Int, submesh: Int, out: java.nio.Buffer): Int

    /** Releases a handle from [meshParse]. */
    external fun meshFree(handle: Long)

    const val DEVICE_TYPE_CARINA: Int = 2

    /** One image for both eyes. */
    const val PANO_MONO: Int = 0

    /** Left eye in the top half of the frame, right eye in the bottom. */
    const val PANO_OVER_UNDER: Int = 1

    /** Left eye in the left half of the frame, right eye in the right. */
    const val PANO_SIDE_BY_SIDE: Int = 2

    /**
     * Both eyes in every pixel, separated by colour. Only the brightness of
     * each eye's picture survives; see [anaglyphMix].
     */
    const val PANO_ANAGLYPH: Int = 3

    /** Eyes on alternating rows of the frame. */
    const val PANO_ROW_INTERLEAVED: Int = 4

    /**
     * Left eye red, right eye cyan — the pair almost all anaglyph material
     * uses, because red-cyan glasses are worn with the red lens over the left
     * eye.
     *
     * Matroska labels this one "anaglyph (cyan/red)", which read as
     * left-then-right says the opposite. The label is mapped onto this rather
     * than believed: the pixels of an actual encode disagree with it. A file
     * that really is the other way round is handled by swapping the eyes.
     */
    const val ANAGLYPH_RED_CYAN: Int = 0

    const val ANAGLYPH_GREEN_MAGENTA: Int = 1

    const val ANAGLYPH_YELLOW_BLUE: Int = 2

    /** [projKind]: an equirectangular image over the bounds it wrote out. */
    const val PROJ_EQUIRECT: Int = 0

    /** [projKind]: six cube faces, with the layout and padding it wrote out. */
    const val PROJ_CUBEMAP: Int = 1

    /** [projKind]: geometry the file carries itself; fetch it with [meshParse]. */
    const val PROJ_MESH: Int = 2

    /** Words of description per entry in a [libraryInfer] batch. */
    const val LIBRARY_META_WORDS: Int = 6

    /** The footage covers the whole sphere. */
    const val PANO_EQUIRECT_360: Int = 0

    /**
     * The footage covers the front hemisphere only — VR180, which is most of
     * what exists in stereoscopic 3D, because two forward-facing lenses can be
     * given a real interocular distance where a full-sphere rig cannot.
     */
    const val PANO_EQUIRECT_180: Int = 1

    /**
     * Not a panorama: an ordinary picture, shown on a screen standing in the
     * room. What most video is, and what a sphere smears across the world.
     */
    const val PANO_FLAT: Int = 2

    /**
     * Six cube faces packed into one frame, as the `cbmp` box's layout 0 packs
     * them. Spends its pixels far more evenly than an equirectangular image,
     * which crowds them into the poles and starves the horizon.
     */
    const val PANO_CUBEMAP: Int = 3
}
