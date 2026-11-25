package org.nx2a;

import android.app.NativeActivity;
import android.os.Build;
import android.os.Bundle;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.WindowManager;
import java.nio.charset.StandardCharsets;
import org.nx2a.doraemon.RuntimeConfigManager;
import org.nx2a.doraemon.SaveAndLogManager;

/** Optional Java text entry + fullscreen cutout, LTPO refresh lock & WQHD+/720p surface cap. */
public class GameActivity extends NativeActivity {
    private final GameKeyboard keyboard = new GameKeyboard(this);
    private volatile int mTouchViewW = 0;
    private volatile int mTouchViewH = 0;
    private volatile Surface mActiveSurface = null;

    public int getTouchViewWidth() { return mTouchViewW; }
    public int getTouchViewHeight() { return mTouchViewH; }

    private void updateTouchViewMetrics(int surfW, int surfH) {
        try {
            View decor = getWindow() != null ? getWindow().getDecorView() : null;
            int vw = decor != null ? decor.getWidth() : 0;
            int vh = decor != null ? decor.getHeight() : 0;
            if (vw < 64 || vh < 64) {
                DisplayMetrics dm = new DisplayMetrics();
                getWindowManager().getDefaultDisplay().getRealMetrics(dm);
                vw = dm.widthPixels;
                vh = dm.heightPixels;
            }
            if (surfW >= 64 && surfH >= 64 && ((surfW > surfH) != (vw > vh))) {
                int tmp = vw; vw = vh; vh = tmp;
            }
            if (vw >= 64 && vh >= 64) {
                mTouchViewW = vw;
                mTouchViewH = vh;
            }
        } catch (Throwable ignored) {
        }
    }

    /**
     * Locks LTPO / high-refresh displays (e.g. Galaxy S24 120Hz LTPO) to the game's target FPS (60/45/30 Hz)
     * to prevent 8.33ms (120Hz) <-> 16.67ms (60Hz) refresh-rate oscillation and uneven frame pacing.
     */
    @SuppressWarnings("deprecation")
    private void applyDisplayRefreshLock(Surface surface) {
        try {
            int targetFps = RuntimeConfigManager.getEffectiveTargetFps(this);
            float targetHz = (float) targetFps;
            if (surface != null && surface.isValid()) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    surface.setFrameRate(
                            targetHz,
                            Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                            Surface.CHANGE_FRAME_RATE_ALWAYS);
                } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    surface.setFrameRate(
                            targetHz,
                            Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
                }
            }
            if (getWindow() == null) return;
            Display display = (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
                    ? getDisplay()
                    : getWindowManager().getDefaultDisplay();
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            boolean changed = false;
            if (Math.abs(lp.preferredRefreshRate - targetHz) > 0.25f) {
                lp.preferredRefreshRate = targetHz;
                changed = true;
            }
            if (display != null) {
                Display.Mode cur = display.getMode();
                Display.Mode[] modes = display.getSupportedModes();
                if (cur != null && modes != null && modes.length > 0) {
                    int cw = cur.getPhysicalWidth();
                    int ch = cur.getPhysicalHeight();
                    Display.Mode best = null;
                    float bestScore = Float.MAX_VALUE;
                    for (Display.Mode m : modes) {
                        if (m.getPhysicalWidth() != cw || m.getPhysicalHeight() != ch) continue;
                        float hz = m.getRefreshRate();
                        // Strongly prefer a mode at or near targetHz (e.g. 60Hz over 120Hz on LTPO).
                        float diff = Math.abs(hz - targetHz);
                        float score = diff + (hz > targetHz + 2.0f ? 100.0f : 0.0f);
                        if (score < bestScore) {
                            bestScore = score;
                            best = m;
                        }
                    }
                    if (best != null && lp.preferredDisplayModeId != best.getModeId()) {
                        lp.preferredDisplayModeId = best.getModeId();
                        changed = true;
                    }
                }
            }
            if (changed) {
                getWindow().setAttributes(lp);
            }
        } catch (Throwable ignored) {
        }
    }

    @Override protected void onCreate(Bundle savedInstanceState) {
        SaveAndLogManager.installJavaCrashHandler(this);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
        }
        super.onCreate(savedInstanceState);
        applyImmersiveFullscreen();
        applyDisplayRefreshLock(mActiveSurface);
    }

    @SuppressWarnings("deprecation")
    private void applyImmersiveFullscreen() {
        View decor = getWindow() != null ? getWindow().getDecorView() : null;
        if (decor != null) {
            decor.setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                            | View.SYSTEM_UI_FLAG_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    @Override public void surfaceCreated(SurfaceHolder holder) {
        try {
            DisplayMetrics dm = new DisplayMetrics();
            getWindowManager().getDefaultDisplay().getRealMetrics(dm);
            int w = Math.max(dm.widthPixels, dm.heightPixels);
            int h = Math.min(dm.widthPixels, dm.heightPixels);
            updateTouchViewMetrics(w, h);
            int maxShort = RuntimeConfigManager.getSurfaceMaxShortEdge(this);
            if (maxShort > 0 && holder != null) {
                if (h > maxShort && w >= 64 && h >= 64) {
                    int scaledH = (maxShort + 1) & ~1;
                    int scaledW = ((int) Math.round((double) w * scaledH / (double) h) + 1) & ~1;
                    holder.setFixedSize(scaledW, scaledH);
                    Log.i("nx2a", "[surface] fixed size applied: " + w + "x" + h + " -> " + scaledW + "x" + scaledH + " (maxShort=" + maxShort + ")");
                }
            }
            if (holder != null) {
                mActiveSurface = holder.getSurface();
                applyDisplayRefreshLock(mActiveSurface);
            }
        } catch (Throwable ignored) {
        }
        super.surfaceCreated(holder);
    }

    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        try {
            if (width >= 64 && height >= 64) {
                int shortE = Math.min(width, height);
                int maxShort = RuntimeConfigManager.getSurfaceMaxShortEdge(this);
                if (maxShort > 0 && holder != null && shortE > maxShort) {
                    int w = Math.max(width, height);
                    int h = shortE;
                    int scaledH = (maxShort + 1) & ~1;
                    int scaledW = ((int) Math.round((double) w * scaledH / (double) h) + 1) & ~1;
                    holder.setFixedSize(scaledW, scaledH);
                    Log.i("nx2a", "[surface] fixed size re-applied in surfaceChanged: " + w + "x" + h + " -> " + scaledW + "x" + scaledH + " (maxShort=" + maxShort + ")");
                }
                updateTouchViewMetrics(width, height);
            }
            if (holder != null) {
                mActiveSurface = holder.getSurface();
                applyDisplayRefreshLock(mActiveSurface);
            }
        } catch (Throwable ignored) {
        }
        super.surfaceChanged(holder, format, width, height);
    }

    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        mActiveSurface = null;
        super.surfaceDestroyed(holder);
    }

    private static String text(byte[] value) {
        return value == null ? "" : new String(value, StandardCharsets.UTF_8);
    }

    // Guest callers wait off the UI thread. Ordinary UTF-8, not JNI modified UTF-8.
    public byte[] readKeyboard(byte[] title, byte[] subtitle, byte[] guide, byte[] initial, int min, int max) {
        return keyboard.read(text(title), text(subtitle), text(guide), text(initial), min, max);
    }

    @Override public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applyImmersiveFullscreen();
            applyDisplayRefreshLock(mActiveSurface);
        }
    }

    @Override protected void onResume() {
        super.onResume();
        applyImmersiveFullscreen();
        applyDisplayRefreshLock(mActiveSurface);
        keyboard.pause(false);
    }
    @Override protected void onPause() { keyboard.pause(true); super.onPause(); }
    @Override protected void onDestroy() { mActiveSurface = null; keyboard.destroy(); super.onDestroy(); }
}
