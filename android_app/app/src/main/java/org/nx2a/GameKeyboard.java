package org.nx2a;

import android.app.Activity;
import android.app.Dialog;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Looper;
import android.text.Editable;
import android.text.InputFilter;
import android.text.InputType;
import android.text.TextWatcher;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CountDownLatch;

/**
 * Top-anchored Landscape Software Keyboard Dialog for nn::swkbd::ShowKeyboard (Doraemon Story of Seasons).
 * Designed for Android 13–16 (API 33–36) full-screen landscape games:
 * - Pinned to Gravity.TOP so the soft keyboard in the bottom half of the screen never hides the input or OK button.
 * - Uses IME_FLAG_NO_EXTRACT_UI | IME_FLAG_NO_FULLSCREEN | IME_ACTION_DONE.
 * - Never auto-dismissed by transient onPause()/onWindowFocusChanged(false) when the IME opens.
 */
final class GameKeyboard {
    private static final String TAG = "nx2a-swkbd";
    private final Activity activity;
    private Dialog dialog;
    private boolean paused = false;

    GameKeyboard(Activity activity) {
        this.activity = activity;
    }

    void pause(boolean value) {
        paused = value;
        // Do NOT dismiss an active swkbd dialog on transient onPause() (some OEMs / Android 15 IMEs
        // trigger focus/pause transitions when the input method window appears).
    }

    void destroy() {
        paused = true;
        if (dialog != null) {
            try {
                dialog.dismiss();
            } catch (Throwable ignored) {}
            dialog = null;
        }
    }

    private static final class Request {
        final CountDownLatch done = new CountDownLatch(1);
        byte[] result;
    }

    private int dp(int v) {
        return Math.round(TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, v, activity.getResources().getDisplayMetrics()));
    }

    private static GradientDrawable roundedBox(int fillColor, int strokeColor, int strokeWidthPx, int radiusPx) {
        GradientDrawable gd = new GradientDrawable();
        gd.setShape(GradientDrawable.RECTANGLE);
        gd.setColor(fillColor);
        gd.setCornerRadius(radiusPx);
        if (strokeWidthPx > 0) {
            gd.setStroke(strokeWidthPx, strokeColor);
        }
        return gd;
    }

    private void showSoftKeyboard(final EditText edit, final Window win) {
        if (edit == null) return;
        try {
            edit.requestFocus();
            if (Build.VERSION.SDK_INT >= 30 && win != null) {
                WindowInsetsController wic = win.getInsetsController();
                if (wic != null) {
                    wic.show(WindowInsets.Type.ime());
                }
            }
            InputMethodManager imm = (InputMethodManager) activity.getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm != null) {
                imm.showSoftInput(edit, InputMethodManager.SHOW_IMPLICIT);
            }
        } catch (Throwable t) {
            Log.w(TAG, "swkbd: showSoftKeyboard warning: " + t);
        }
    }

    private static String sanitizeText(String raw, int limit) {
        if (raw == null) return "";
        StringBuilder sb = new StringBuilder(raw.length());
        for (int i = 0; i < raw.length(); i++) {
            char c = raw.charAt(i);
            if (c == '\r' || c == '\n' || c == '\t' || c < 0x20 || c == 0x7f) continue;
            sb.append(c);
            if (sb.length() >= limit) break;
        }
        return sb.toString().trim();
    }

    byte[] read(String title, String subtitle, String guide, String initial, int min, int max) {
        if (Looper.myLooper() == Looper.getMainLooper()) return null;
        final int limit = max > 0 ? Math.min(max, 1002) : 12;
        final int minimum = Math.max(1, Math.max(0, min));
        if (minimum > limit) return null;

        Log.i(TAG, "swkbd: show request title='" + title + "' sub='" + subtitle
                + "' guide='" + guide + "' init='" + initial + "' min=" + minimum + " max=" + limit);

        final Request req = new Request();
        activity.runOnUiThread(() -> {
            if (activity.isFinishing() || activity.isDestroyed()) {
                Log.w(TAG, "swkbd: activity finishing/destroyed, aborting dialog");
                req.done.countDown();
                return;
            }
            if (dialog != null) {
                try {
                    dialog.dismiss();
                } catch (Throwable ignored) {}
                dialog = null;
            }
            try {
                final Dialog current = new Dialog(activity);
                current.requestWindowFeature(Window.FEATURE_NO_TITLE);
                current.setCanceledOnTouchOutside(false);
                current.setCancelable(false);

                // Root card anchored at top of landscape screen
                LinearLayout card = new LinearLayout(activity);
                card.setOrientation(LinearLayout.VERTICAL);
                card.setPadding(dp(14), dp(10), dp(14), dp(10));
                card.setBackground(roundedBox(
                        Color.parseColor("#FFFDF6"),
                        Color.parseColor("#8D5B34"),
                        dp(2),
                        dp(14)));

                // Row 1: Header (Title + subtitle/guide + char counter + [Keyboard] button)
                LinearLayout headerRow = new LinearLayout(activity);
                headerRow.setOrientation(LinearLayout.HORIZONTAL);
                headerRow.setGravity(Gravity.CENTER_VERTICAL);

                TextView titleView = new TextView(activity);
                String headerTitle = (title != null && !title.trim().isEmpty())
                        ? title.trim()
                        : ((subtitle != null && !subtitle.trim().isEmpty())
                                ? subtitle.trim()
                                : "Masukkan Nama / Enter Name");
                titleView.setText(headerTitle + "  (maks " + limit + " karakter)");
                titleView.setTextColor(Color.parseColor("#4E321B"));
                titleView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
                titleView.setTypeface(Typeface.DEFAULT_BOLD);
                LinearLayout.LayoutParams titleLp = new LinearLayout.LayoutParams(
                        0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f);
                headerRow.addView(titleView, titleLp);

                final TextView counterView = new TextView(activity);
                counterView.setTextColor(Color.parseColor("#7A5334"));
                counterView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
                counterView.setTypeface(Typeface.DEFAULT_BOLD);
                counterView.setPadding(dp(8), 0, dp(8), 0);
                headerRow.addView(counterView);

                Button kbdBtn = new Button(activity);
                kbdBtn.setAllCaps(false);
                kbdBtn.setText("⌨ Keyboard");
                kbdBtn.setTextColor(Color.parseColor("#4E321B"));
                kbdBtn.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
                kbdBtn.setPadding(dp(10), dp(2), dp(10), dp(2));
                kbdBtn.setMinimumHeight(dp(28));
                kbdBtn.setMinHeight(dp(28));
                kbdBtn.setBackground(roundedBox(
                        Color.parseColor("#F5E6C8"),
                        Color.parseColor("#A5734A"),
                        dp(1),
                        dp(8)));
                headerRow.addView(kbdBtn);

                card.addView(headerRow);

                // Row 2: 1-line Landscape Input Bar: [ EditText ] [ OK / Simpan ] [ Batal ]
                LinearLayout inputRow = new LinearLayout(activity);
                inputRow.setOrientation(LinearLayout.HORIZONTAL);
                inputRow.setGravity(Gravity.CENTER_VERTICAL);
                LinearLayout.LayoutParams inputRowLp = new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                inputRowLp.topMargin = dp(6);

                final EditText edit = new EditText(activity);
                edit.setSingleLine(true);
                edit.setMaxLines(1);
                edit.setInputType(InputType.TYPE_CLASS_TEXT
                        | InputType.TYPE_TEXT_FLAG_CAP_WORDS
                        | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
                edit.setImeOptions(EditorInfo.IME_ACTION_DONE
                        | EditorInfo.IME_FLAG_NO_EXTRACT_UI
                        | EditorInfo.IME_FLAG_NO_FULLSCREEN);
                edit.setFilters(new InputFilter[]{new InputFilter.LengthFilter(limit)});
                String hintText = (guide != null && !guide.trim().isEmpty())
                        ? guide.trim()
                        : "Ketik nama di sini...";
                edit.setHint(hintText);
                edit.setHintTextColor(Color.parseColor("#A78E75"));
                edit.setTextColor(Color.parseColor("#2E1C0E"));
                edit.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
                edit.setPadding(dp(12), dp(7), dp(12), dp(7));
                edit.setBackground(roundedBox(
                        Color.parseColor("#FFFFFF"),
                        Color.parseColor("#8D5B34"),
                        dp(2),
                        dp(10)));
                String cleanInit = sanitizeText(initial, limit);
                edit.setText(cleanInit);
                edit.setSelection(edit.length());

                LinearLayout.LayoutParams editLp = new LinearLayout.LayoutParams(
                        0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f);
                editLp.rightMargin = dp(8);
                inputRow.addView(edit, editLp);

                final Button okBtn = new Button(activity);
                okBtn.setAllCaps(false);
                okBtn.setText("✓ OK / Simpan");
                okBtn.setTextColor(Color.WHITE);
                okBtn.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
                okBtn.setTypeface(Typeface.DEFAULT_BOLD);
                okBtn.setPadding(dp(14), dp(7), dp(14), dp(7));
                okBtn.setBackground(roundedBox(
                        Color.parseColor("#3E8E3A"),
                        Color.parseColor("#2A6827"),
                        dp(1),
                        dp(10)));
                LinearLayout.LayoutParams okLp = new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                okLp.rightMargin = dp(6);
                inputRow.addView(okBtn, okLp);

                final Button cancelBtn = new Button(activity);
                cancelBtn.setAllCaps(false);
                String fallbackCancel = !cleanInit.isEmpty() ? cleanInit : "Nobi";
                if (fallbackCancel.length() > limit) fallbackCancel = fallbackCancel.substring(0, limit);
                final String defaultIfCancelled = fallbackCancel;
                cancelBtn.setText("Default (" + defaultIfCancelled + ")");
                cancelBtn.setTextColor(Color.parseColor("#5B3A1E"));
                cancelBtn.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
                cancelBtn.setPadding(dp(10), dp(7), dp(10), dp(7));
                cancelBtn.setBackground(roundedBox(
                        Color.parseColor("#EFE2C9"),
                        Color.parseColor("#9C7048"),
                        dp(1),
                        dp(10)));
                inputRow.addView(cancelBtn);

                card.addView(inputRow, inputRowLp);

                // Row 3: Quick preset chips (1-tap fill if soft keyboard fails to appear on esoteric OEM ROMs)
                HorizontalScrollView chipScroll = new HorizontalScrollView(activity);
                chipScroll.setHorizontalScrollBarEnabled(false);
                LinearLayout chipRow = new LinearLayout(activity);
                chipRow.setOrientation(LinearLayout.HORIZONTAL);
                chipRow.setGravity(Gravity.CENTER_VERTICAL);

                TextView chipLabel = new TextView(activity);
                chipLabel.setText("Cepat isi: ");
                chipLabel.setTextColor(Color.parseColor("#7A5334"));
                chipLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
                chipRow.addView(chipLabel);

                final String[] presets = new String[]{"Nobi", "Doraemon", "Haru", "Natura", "Sora", "Kibou"};
                for (String p : presets) {
                    final String clipped = p.length() > limit ? p.substring(0, limit) : p;
                    if (clipped.length() < minimum) continue;
                    Button chip = new Button(activity);
                    chip.setAllCaps(false);
                    chip.setText(clipped);
                    chip.setTextColor(Color.parseColor("#4E321B"));
                    chip.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
                    chip.setPadding(dp(10), dp(2), dp(10), dp(2));
                    chip.setMinimumHeight(dp(26));
                    chip.setMinHeight(dp(26));
                    chip.setMinimumWidth(0);
                    chip.setMinWidth(0);
                    chip.setBackground(roundedBox(
                            Color.parseColor("#F7ECD5"),
                            Color.parseColor("#C29B70"),
                            dp(1),
                            dp(12)));
                    LinearLayout.LayoutParams chipLp = new LinearLayout.LayoutParams(
                            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                    chipLp.leftMargin = dp(4);
                    chip.setOnClickListener(v -> {
                        edit.setText(clipped);
                        edit.setSelection(edit.length());
                    });
                    chipRow.addView(chip, chipLp);
                }
                chipScroll.addView(chipRow);
                LinearLayout.LayoutParams chipScrollLp = new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                chipScrollLp.topMargin = dp(6);
                card.addView(chipScroll, chipScrollLp);

                current.setContentView(card);

                final Window win = current.getWindow();
                if (win != null) {
                    win.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
                    win.clearFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                            | WindowManager.LayoutParams.FLAG_ALT_FOCUSABLE_IM);
                    win.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
                    win.setDimAmount(0.45f);
                    win.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE
                            | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING);
                    WindowManager.LayoutParams wlp = win.getAttributes();
                    wlp.gravity = Gravity.TOP | Gravity.CENTER_HORIZONTAL;
                    wlp.y = dp(8);
                    int screenW = activity.getResources().getDisplayMetrics().widthPixels;
                    wlp.width = Math.min(screenW - dp(24), dp(640));
                    wlp.height = WindowManager.LayoutParams.WRAP_CONTENT;
                    win.setAttributes(wlp);
                }

                final Runnable updateCounter = () -> {
                    String cur = sanitizeText(edit.getText().toString(), limit);
                    int len = cur.length();
                    counterView.setText(len + " / " + limit);
                    boolean valid = len >= minimum && len <= limit;
                    okBtn.setAlpha(valid ? 1.0f : 0.65f);
                };
                updateCounter.run();

                final Runnable confirmAction = () -> {
                    String value = sanitizeText(edit.getText().toString(), limit);
                    if (value.length() < minimum) {
                        Toast.makeText(activity,
                                "Masukkan nama minimal " + minimum + " karakter (atau pilih tombol Cepat isi).",
                                Toast.LENGTH_SHORT).show();
                        showSoftKeyboard(edit, win);
                        return;
                    }
                    req.result = value.getBytes(StandardCharsets.UTF_8);
                    Log.i(TAG, "swkbd: confirmed OK text='" + value + "' bytes=" + req.result.length);
                    try {
                        current.dismiss();
                    } catch (Throwable ignored) {}
                };

                okBtn.setOnClickListener(v -> confirmAction.run());
                cancelBtn.setOnClickListener(v -> {
                    // Use the clean initial/default name if the user taps Default, so the game never gets stuck
                    String value = sanitizeText(edit.getText().toString(), limit);
                    if (value.length() < minimum) {
                        value = defaultIfCancelled;
                    }
                    req.result = value.getBytes(StandardCharsets.UTF_8);
                    Log.i(TAG, "swkbd: default/fallback selected text='" + value + "'");
                    try {
                        current.dismiss();
                    } catch (Throwable ignored) {}
                });
                kbdBtn.setOnClickListener(v -> showSoftKeyboard(edit, win));

                edit.addTextChangedListener(new TextWatcher() {
                    @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
                    @Override public void onTextChanged(CharSequence s, int start, int before, int count) {
                        updateCounter.run();
                    }
                    @Override public void afterTextChanged(Editable s) {}
                });

                edit.setOnEditorActionListener((v, actionId, event) -> {
                    boolean isEnterKey = event != null
                            && event.getAction() == KeyEvent.ACTION_DOWN
                            && (event.getKeyCode() == KeyEvent.KEYCODE_ENTER
                                || event.getKeyCode() == KeyEvent.KEYCODE_NUMPAD_ENTER);
                    if (actionId == EditorInfo.IME_ACTION_DONE
                            || actionId == EditorInfo.IME_ACTION_GO
                            || actionId == EditorInfo.IME_ACTION_SEND
                            || actionId == EditorInfo.IME_ACTION_NEXT
                            || isEnterKey) {
                        confirmAction.run();
                        return true;
                    }
                    return false;
                });

                current.setOnKeyListener((d, keyCode, event) -> {
                    if (keyCode == KeyEvent.KEYCODE_BACK && event.getAction() == KeyEvent.ACTION_UP) {
                        // Do not silently cancel and trap the player in the naming loop; if text is valid confirm it,
                        // otherwise remind the player to press OK or Default.
                        String cur = sanitizeText(edit.getText().toString(), limit);
                        if (cur.length() >= minimum) {
                            confirmAction.run();
                        } else {
                            Toast.makeText(activity,
                                    "Ketik nama lalu tekan '✓ OK / Simpan' atau pilih 'Default'.",
                                    Toast.LENGTH_SHORT).show();
                        }
                        return true;
                    }
                    return false;
                });

                current.setOnDismissListener(d -> {
                    if (dialog == current) dialog = null;
                    req.done.countDown();
                });

                dialog = current;
                current.show();
                Log.i(TAG, "swkbd: dialog shown on UI thread (top-anchored landscape)");

                showSoftKeyboard(edit, win);
                edit.postDelayed(() -> showSoftKeyboard(edit, win), 120);
                edit.postDelayed(() -> showSoftKeyboard(edit, win), 300);
            } catch (Throwable e) {
                Log.e(TAG, "swkbd: failed to show dialog", e);
                if (dialog != null) {
                    try { dialog.dismiss(); } catch (Throwable ignored) {}
                    dialog = null;
                }
                req.done.countDown();
            }
        });

        boolean interrupted = false;
        for (;;) {
            try {
                req.done.await();
                break;
            } catch (InterruptedException e) {
                interrupted = true;
            }
        }
        if (interrupted) Thread.currentThread().interrupt();
        Log.i(TAG, "swkbd: read finished result=" + (req.result != null ? ("OK (" + req.result.length + " bytes)") : "null/cancel"));
        return req.result;
    }
}
