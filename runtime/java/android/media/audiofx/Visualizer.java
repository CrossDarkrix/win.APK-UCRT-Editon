// android.media.audiofx.Visualizer -- pulls waveform/FFT data from the output of
// an audio session. AOSP frameworks/base media/java/android/media/audiofx/Visualizer.java.
//
// Phase20-B2.3.6.4.21: this used to be an honest stub -- getWaveForm filled
// the mid-level (0x80) silence value, getFft filled zeros, and the data-
// capture listener never fired, because upstream's mixer had no tap.
// Apps that draw a spectrum for ordinary playback have no other Android
// API to get it from, so their analyser starved (flat or near-frozen)
// while USB-DAC playback, which such apps tap themselves, looked fine.
// The Windows host now taps its final output mix (audio_engine.cpp's
// viz::feed, fed from Mixer::mix) and serves real data through
// com.macapk.loader.NativeAudio's nativeViz* natives. Session ids are
// not distinguished: AudioTrack.getAudioSessionId() is 0 on this host
// and session 0 IS the output mix, which is exactly what the tap holds.
// The capture listener is driven by a daemon thread polling the tap at
// the requested rate, mirroring AOSP's delivery on its own thread.
package android.media.audiofx;

public class Visualizer {
    public static final int SUCCESS = 0;
    public static final int ERROR = -1;
    public static final int ERROR_NO_INIT = -3;
    public static final int ERROR_BAD_VALUE = -4;
    public static final int ERROR_INVALID_OPERATION = -5;
    public static final int ERROR_DEAD_OBJECT = -7;

    public static final int STATE_UNINITIALIZED = 0;
    public static final int STATE_INITIALIZED = 1;
    public static final int STATE_ENABLED = 2;

    public static final int SCALING_MODE_NORMALIZED = 0;
    public static final int SCALING_MODE_AS_PLAYED = 1;
    public static final int MEASUREMENT_MODE_NONE = 0;
    public static final int MEASUREMENT_MODE_PEAK_RMS = 1;

    private static final int CAPTURE_SIZE = 1024;
    private int mCaptureSize = CAPTURE_SIZE;
    private boolean mEnabled = false;
    private boolean mReleased = false;

    private OnDataCaptureListener mListener;
    private int mCaptureRateMilliHz = 10000;  // AOSP default: 10 Hz
    private boolean mCaptureWaveform = false;
    private boolean mCaptureFft = false;
    private Thread mCaptureThread;

    public Visualizer(int audioSession) {}

    public synchronized int setEnabled(boolean enabled) {
        mEnabled = enabled;
        updateCaptureThread();
        return SUCCESS;
    }
    public boolean getEnabled() { return mEnabled; }
    public int getSamplingRate() {
        try { return com.macapk.loader.NativeAudio.nativeVizGetSamplingRateMilliHz(); }
        catch (Throwable t) { return 44100000; }  // milliHz, AOSP units
    }
    public static int[] getCaptureSizeRange() { return new int[] { 128, CAPTURE_SIZE }; }
    public static int getMaxCaptureRate() { return 20000; }
    public synchronized int setCaptureSize(int size) {
        if (size < 128 || size > CAPTURE_SIZE || (size & (size - 1)) != 0) return ERROR_BAD_VALUE;
        mCaptureSize = size;
        return SUCCESS;
    }
    public int getCaptureSize() { return mCaptureSize; }
    public int setScalingMode(int mode) { return SUCCESS; }
    public int getScalingMode() { return SCALING_MODE_NORMALIZED; }
    public int setMeasurementMode(int mode) { return SUCCESS; }
    public int getMeasurementMode() { return MEASUREMENT_MODE_NONE; }

    public int getWaveForm(byte[] waveform) {
        if (waveform == null) return ERROR_BAD_VALUE;
        try {
            byte[] buf = new byte[mCaptureSize];
            int rc = com.macapk.loader.NativeAudio.nativeVizGetWaveForm(buf);
            if (rc != SUCCESS) return rc;
            System.arraycopy(buf, 0, waveform, 0, Math.min(waveform.length, buf.length));
            return SUCCESS;
        } catch (Throwable t) {
            // No backend (should not happen): fall back to honest silence.
            java.util.Arrays.fill(waveform, (byte) 0x80);
            return SUCCESS;
        }
    }
    public int getFft(byte[] fft) {
        if (fft == null) return ERROR_BAD_VALUE;
        try {
            byte[] buf = new byte[mCaptureSize];
            int rc = com.macapk.loader.NativeAudio.nativeVizGetFft(buf);
            if (rc != SUCCESS) return rc;
            System.arraycopy(buf, 0, fft, 0, Math.min(fft.length, buf.length));
            return SUCCESS;
        } catch (Throwable t) {
            java.util.Arrays.fill(fft, (byte) 0);
            return SUCCESS;
        }
    }
    public int getMeasurementPeakRms(MeasurementPeakRms measurement) {
        if (measurement != null) { measurement.mPeak = -9600; measurement.mRms = -9600; }
        return SUCCESS;
    }

    public synchronized int setDataCaptureListener(OnDataCaptureListener listener, int rate,
                                      boolean waveform, boolean fft) {
        if (rate < 0 || rate > getMaxCaptureRate()) return ERROR_BAD_VALUE;
        mListener = listener;
        mCaptureRateMilliHz = rate;
        mCaptureWaveform = waveform;
        mCaptureFft = fft;
        updateCaptureThread();
        return SUCCESS;
    }

    // Starts/stops the polling thread to match (enabled && listener set).
    // Delivery happens on this daemon thread, as in AOSP.
    private void updateCaptureThread() {
        boolean want = mEnabled && !mReleased && mListener != null
                && (mCaptureWaveform || mCaptureFft) && mCaptureRateMilliHz > 0;
        if (want && mCaptureThread == null) {
            mCaptureThread = new Thread(new Runnable() { @Override public void run() {
                for (;;) {
                    final OnDataCaptureListener l;
                    final boolean wf, ft;
                    final int size, rateMilliHz;
                    synchronized (Visualizer.this) {
                        if (!mEnabled || mReleased || mListener == null) { mCaptureThread = null; return; }
                        l = mListener; wf = mCaptureWaveform; ft = mCaptureFft;
                        size = mCaptureSize; rateMilliHz = mCaptureRateMilliHz;
                    }
                    try {
                        int samplingRate = getSamplingRate();
                        if (wf) {
                            byte[] buf = new byte[size];
                            if (com.macapk.loader.NativeAudio.nativeVizGetWaveForm(buf) == SUCCESS)
                                l.onWaveFormDataCapture(Visualizer.this, buf, samplingRate);
                        }
                        if (ft) {
                            byte[] buf = new byte[size];
                            if (com.macapk.loader.NativeAudio.nativeVizGetFft(buf) == SUCCESS)
                                l.onFftDataCapture(Visualizer.this, buf, samplingRate);
                        }
                    } catch (Throwable ignored) { }
                    try { Thread.sleep(Math.max(10L, 1000000L / Math.max(1, rateMilliHz))); }
                    catch (InterruptedException e) { return; }
                }
            } }, "macapk-visualizer-capture");
            mCaptureThread.setDaemon(true);
            mCaptureThread.start();
        }
    }

    public synchronized void release() {
        mReleased = true;
        mEnabled = false;
        mListener = null;
        updateCaptureThread();
    }

    /** AOSP declares finalize() on this class so a dropped instance still
     *  releases whatever it holds. release() is idempotent here. */
    @Override protected void finalize() throws Throwable {
        try { release(); } finally { super.finalize(); }
    }


    public static final class MeasurementPeakRms {
        public int mPeak;
        public int mRms;
        public MeasurementPeakRms() {}
    }
    public interface OnDataCaptureListener {
        void onWaveFormDataCapture(Visualizer visualizer, byte[] waveform, int samplingRate);
        void onFftDataCapture(Visualizer visualizer, byte[] fft, int samplingRate);
    }
}
