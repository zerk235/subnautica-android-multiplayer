package io.github.zerk235.snmp.installer;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

/** Palette and view factories for the themed installer interface. */
final class Ui {
    static final int BG = Color.rgb(10, 22, 32);
    static final int CARD = Color.argb(255, 16, 38, 54);
    static final int CARD_STROKE = Color.argb(255, 34, 82, 104);
    static final int CYAN = Color.rgb(79, 216, 206);
    static final int CYAN_DIM = Color.argb(255, 30, 96, 106);
    static final int AMBER = Color.rgb(232, 148, 42);
    static final int AMBER_DARK = Color.rgb(122, 70, 14);
    static final int TEXT = Color.WHITE;
    static final int MUTED = Color.rgb(143, 172, 192);
    static final int GREEN = Color.rgb(46, 204, 133);
    static final int RED = Color.rgb(224, 92, 92);
    static final int RED_DIM = Color.argb(255, 84, 34, 38);

    private Ui() {}

    static int dp(Context c, int v) { return (int)(c.getResources().getDisplayMetrics().density * v + .5f); }

    static GradientDrawable round(Context c, int fill, int radius, int strokeColor, int strokePx) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setCornerRadius(dp(c, radius));
        if (strokeColor != 0) { d.setStroke(strokePx == 0 ? dp(c, 1) : dp(c, strokePx), strokeColor); }
        return d;
    }

    /** Card container with themed background and padding. */
    static LinearLayout card(Context c) {
        LinearLayout card = new LinearLayout(c);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(round(c, CARD, 18, CARD_STROKE, 1));
        int pad = dp(c, 16);
        card.setPadding(pad, pad, pad, pad);
        return card;
    }

    static TextView text(Context c, String value, int sizeSp, int color) {
        TextView view = new TextView(c);
        view.setText(value);
        view.setTextSize(sizeSp);
        view.setTextColor(color);
        view.setLineSpacing(dp(c, 2), 1f);
        return view;
    }

    static TextView header(Context c, String value) {
        TextView view = text(c, value, 19, TEXT);
        view.setTypeface(Typeface.DEFAULT_BOLD);
        return view;
    }

    static TextView caption(Context c, String value) {
        return text(c, value, 13, MUTED);
    }

    static LinearLayout gap(Context c, int h) {
        LinearLayout g = new LinearLayout(c);
        g.setMinimumHeight(dp(c, h));
        return g;
    }

    /** Pill button. style: 0 primary amber, 1 cyan outline, 2 danger, 3 cyan filled. */
    static Button button(Context c, String title, int style) {
        Button b = new Button(c);
        b.setText(title);
        b.setAllCaps(false);
        b.setTextSize(15);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setTransformationMethod(null);
        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(c, 26));
        switch (style) {
            case 0:
                bg.setColor(AMBER); b.setTextColor(Color.rgb(38, 22, 2)); break;
            case 1:
                bg.setColor(Color.argb(40, 79, 216, 206)); bg.setStroke(dp(c, 1), CYAN_DIM); b.setTextColor(CYAN); break;
            case 2:
                bg.setColor(Color.argb(60, 224, 92, 92)); bg.setStroke(dp(c, 1), RED); b.setTextColor(RED); break;
            default:
                bg.setColor(Color.argb(90, 79, 216, 206)); b.setTextColor(TEXT); break;
        }
        b.setBackground(bg);
        b.setPadding(dp(c, 20), dp(c, 12), dp(c, 20), dp(c, 12));
        b.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return b;
    }

    /** Small status chip, e.g. "Активный" / "Не подключено". */
    static TextView chip(Context c, String label, int color) {
        TextView chip = text(c, label, 12, color);
        chip.setBackground(round(c, Color.argb(60, Color.red(color), Color.green(color), Color.blue(color)), 20, 0, 0));
        int px = dp(c, 8);
        chip.setPadding(px, dp(c, 3), px, dp(c, 3));
        return chip;
    }

    /** Circular step indicator: number, or check mark when done. */
    static TextView stepBadge(Context c, int number, int state) {
        // state 0 pending, 1 active, 2 done
        TextView badge = new TextView(c);
        GradientDrawable bg = new GradientDrawable();
        bg.setShape(GradientDrawable.OVAL);
        if (state == 2) { bg.setColor(GREEN); badge.setText("✓"); badge.setTextColor(Color.rgb(4, 26, 20)); }
        else if (state == 1) { bg.setColor(CYAN); badge.setText(String.valueOf(number)); badge.setTextColor(Color.rgb(4, 26, 20)); }
        else { bg.setColor(CARD); bg.setStroke(dp(c, 1), CARD_STROKE); badge.setText(String.valueOf(number)); badge.setTextColor(MUTED); }
        badge.setBackground(bg);
        badge.setGravity(Gravity.CENTER);
        badge.setTextSize(15);
        badge.setTypeface(Typeface.DEFAULT_BOLD);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(c, 34), dp(c, 34));
        lp.gravity = Gravity.TOP;
        badge.setLayoutParams(lp);
        return badge;
    }

    /** Horizontal progress strip of five dots mirroring the install steps. */
    static LinearLayout progressStrip(Context c, int states[]) {
        LinearLayout row = new LinearLayout(c);
        row.setGravity(Gravity.CENTER_VERTICAL);
        for (int i = 0; i < states.length; i++) {
            if (i > 0) {
                View line = new View(c);
                line.setBackground(round(c, states[i] > 0 ? CYAN : CARD_STROKE, 2, 0, 0));
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, dp(c, 3), 1f);
                lp.setMargins(dp(c, 4), 0, dp(c, 4), 0);
                line.setLayoutParams(lp);
                row.addView(line);
            }
            TextView dot = new TextView(c);
            GradientDrawable bg = new GradientDrawable();
            bg.setShape(GradientDrawable.OVAL);
            if (states[i] == 2) { bg.setColor(GREEN); dot.setText("✓"); dot.setTextColor(Color.rgb(4, 26, 20)); }
            else if (states[i] == 1) { bg.setColor(CYAN); dot.setText(String.valueOf(i + 1)); dot.setTextColor(Color.rgb(4, 26, 20)); }
            else { bg.setColor(CARD); bg.setStroke(dp(c, 1), CARD_STROKE); dot.setText(String.valueOf(i + 1)); dot.setTextColor(MUTED); }
            dot.setBackground(bg);
            dot.setGravity(Gravity.CENTER);
            dot.setTextSize(11);
            dot.setTypeface(Typeface.DEFAULT_BOLD);
            row.addView(dot, new LinearLayout.LayoutParams(dp(c, 24), dp(c, 24)));
        }
        return row;
    }

    /** Info row with label left and value right (settings style). */
    static LinearLayout infoRow(Context c, String label, String value) {
        LinearLayout row = new LinearLayout(c);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        TextView l = text(c, label, 14, MUTED);
        TextView v = text(c, value, 14, TEXT);
        v.setGravity(Gravity.RIGHT);
        row.addView(l, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        row.addView(v, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return row;
    }

    /** Bottom navigation tab. */
    static LinearLayout tab(Context c, String icon, String label) {
        LinearLayout tab = new LinearLayout(c);
        tab.setOrientation(LinearLayout.VERTICAL);
        tab.setGravity(Gravity.CENTER);
        TextView ic = text(c, icon, 17, MUTED);
        ic.setGravity(Gravity.CENTER);
        TextView lb = text(c, label, 11, MUTED);
        lb.setGravity(Gravity.CENTER);
        tab.addView(ic);
        tab.addView(lb);
        return tab;
    }
}
