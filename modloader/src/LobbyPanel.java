package snmp.menu;

import android.app.Activity;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.lang.ref.WeakReference;
import java.util.Locale;

/** UI owned by the game activity. Native session actions run on the Unity thread. */
public final class LobbyPanel {
    private static final int CYAN = 0xff93eee9;
    private static final int MUTED = 0xffb0c7d5;
    private static WeakReference<FrameLayout> displayed = new WeakReference<>(null);

    private LobbyPanel() {}
    private static native void act(int action, int host, int room);

    public static void render(Activity activity, String data) {
        activity.runOnUiThread(() -> {
            FrameLayout previous = displayed.get();
            if (previous != null && previous.getParent() instanceof ViewGroup)
                ((ViewGroup) previous.getParent()).removeView(previous);
            displayed.clear();
            if (data.isEmpty() || activity.isFinishing() || activity.isDestroyed()) return;
            String[] fields = data.split(",");
            FrameLayout layer = build(activity, fields);
            activity.addContentView(layer, layer.getLayoutParams());
            displayed = new WeakReference<>(layer);
        });
    }

    private static FrameLayout build(Activity activity, String[] fields) {
        int mode = number(fields[0]);
        FrameLayout layer = new FrameLayout(activity);
        if (mode == 0 || mode == 4) {
            Button entry = button(activity, mode == 0 ? "Совместная игра" : "Отменить запуск", mode == 0);
            entry.setOnClickListener(v -> act(mode == 0 ? 1 : 6, 0, 0));
            FrameLayout.LayoutParams placement = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.END);
            placement.setMargins(dp(activity, 12), dp(activity, 12), dp(activity, 20), dp(activity, 12));
            layer.addView(entry, placement);
            layer.setLayoutParams(new FrameLayout.LayoutParams(-1, -1));
            return layer;
        }

        layer.setClickable(true);
        layer.setBackgroundColor(0xce041521);
        ViewGroup decor = (ViewGroup) activity.getWindow().getDecorView();
        int screenWidth = decor.getWidth();
        int margin = dp(activity, 16);
        int width = screenWidth > 2 * margin ? Math.min(dp(activity, 408), screenWidth - 2 * margin) : -1;
        ScrollView scroll = new ScrollView(activity);
        scroll.setFillViewport(false);
        FrameLayout.LayoutParams position = new FrameLayout.LayoutParams(width, -2, Gravity.CENTER);
        position.setMargins(margin, margin, margin, margin);
        layer.addView(scroll, position);

        LinearLayout panel = new LinearLayout(activity);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackground(surface(activity, 0xff082838, 0xff3d8b9d, 20));
        int inset = dp(activity, 18);
        panel.setPadding(inset, inset, inset, inset);
        scroll.addView(panel, new ScrollView.LayoutParams(-1, -2));
        TextView title = text(activity, mode == 1 ? "Совместная игра" : "Комната " + code(number(fields[8])), 23, Color.WHITE);
        title.setTypeface(null, Typeface.BOLD);
        title.setAccessibilityHeading(true);
        panel.addView(title);

        if (mode == 1) {
            browse(activity, panel, fields);
        } else {
            room(activity, panel, fields, mode);
        }
        addButton(activity, panel, mode == 1 ? "Закрыть" : "Выйти из лобби", false, true,
            () -> act(6, 0, 0));
        layer.setLayoutParams(new FrameLayout.LayoutParams(-1, -1));
        return layer;
    }

    private static void browse(Activity activity, LinearLayout panel, String[] fields) {
        note(activity, panel, number(fields[6]) != 0
            ? "Комната закрыта или место уже занято. Выберите другую комнату."
            : "Подключите оба телефона к одной сети Wi-Fi.");
        addButton(activity, panel, "Создать комнату", true, true, () -> act(2, 0, 0));
        note(activity, panel, fields.length == 9 ? "Ищем доступные комнаты…" : "Доступные комнаты");
        for (int i = 9; i + 1 < fields.length; i += 2) {
            int host = number(fields[i]);
            int room = number(fields[i + 1]);
            addButton(activity, panel, "Комната " + code(room), false, true, () -> act(3, host, room));
        }
    }

    private static void room(Activity activity, LinearLayout panel, String[] fields, int mode) {
        boolean hosting = mode == 2;
        boolean hostReady = number(fields[3]) != 0;
        boolean guestReady = number(fields[4]) != 0;
        boolean admitted = number(fields[2]) != 0;
        note(activity, panel, "Хост " + code(number(fields[1])) + (hosting ? " (вы)" : "") + suffix(hostReady));
        String guest = admitted ? "Гость " + code(number(fields[2])) + (hosting ? "" : " (вы)") + suffix(guestReady)
            : hosting ? "Ожидаем второго игрока…" : "Подключаемся к комнате…";
        note(activity, panel, guest);
        if (mode == 5) {
            int session = number(fields[7]);
            String status;
            switch (session) {
                case 4: status = "Получаем мир от хоста…"; break;
                case 5: status = "Мир получен. Начинаем загрузку…"; break;
                case 6:
                case 7: status = "Не удалось загрузить мир. Если ошибка повторяется, выйдите из лобби и попробуйте снова."; break;
                default: status = "Ждём, пока хост откроет и сохранит мир…"; break;
            }
            note(activity, panel, status);
            return;
        }
        boolean ready = hosting ? hostReady : guestReady;
        addButton(activity, panel, ready ? "Снять готовность" : "Готов", true, hosting || admitted,
            () -> act(4, 0, 0));
        if (hosting) {
            boolean start = number(fields[5]) != 0;
            if (!start) note(activity, panel, "Выбор мира откроется, когда оба игрока будут готовы.");
            addButton(activity, panel, "Выбрать и запустить мир", true, start, () -> act(5, 0, 0));
        }
    }

    private static int number(String value) { return Integer.parseUnsignedInt(value); }

    private static String code(int value) {
        return String.format(Locale.ROOT, "%08X", value);
    }

    private static String suffix(boolean ready) { return ready ? " · Готов" : " · Не готов"; }

    private static void note(Activity activity, LinearLayout parent, String value) {
        TextView label = text(activity, value, 15, MUTED);
        label.setPadding(0, dp(activity, 9), 0, dp(activity, 3));
        parent.addView(label);
    }

    private static TextView text(Activity activity, String value, int size, int color) {
        TextView view = new TextView(activity);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        return view;
    }

    private static void addButton(Activity activity, LinearLayout parent, String label, boolean primary,
                                  boolean enabled, Runnable onClick) {
        Button view = button(activity, label, primary);
        view.setEnabled(enabled);
        if (!enabled) view.setAlpha(0.5f);
        view.setOnClickListener(v -> onClick.run());
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
        params.topMargin = dp(activity, 8);
        parent.addView(view, params);
    }

    private static Button button(Activity activity, String label, boolean primary) {
        Button view = new Button(activity);
        view.setAllCaps(false);
        view.setText(label);
        view.setTextColor(primary ? Color.WHITE : CYAN);
        view.setTextSize(15);
        view.setMinHeight(dp(activity, 48));
        int padding = dp(activity, 14);
        view.setPadding(padding, dp(activity, 7), padding, dp(activity, 7));
        view.setBackground(new RippleDrawable(ColorStateList.valueOf(0x4493eee9),
            surface(activity, primary ? 0xff126579 : 0xff123949, primary ? CYAN : 0xff3d8b9d, 14), null));
        return view;
    }

    private static GradientDrawable surface(Activity activity, int color, int stroke, int radius) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(color);
        drawable.setStroke(dp(activity, 1), stroke);
        drawable.setCornerRadius(dp(activity, radius));
        return drawable;
    }

    private static int dp(Activity activity, int value) {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }
}
