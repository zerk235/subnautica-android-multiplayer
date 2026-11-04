package io.github.zerk235.snmp.installer;

import android.app.*;
import android.content.*;
import android.content.pm.PackageInfo;
import android.graphics.Color;
import android.net.Uri;
import android.os.*;
import android.provider.Settings;
import android.view.View;
import android.widget.*;
import org.json.JSONObject;

public final class MainActivity extends Activity {
    TextView game, status, note;
    Button prepare, install, launch, cancel;
    final Handler handler = new Handler(Looper.getMainLooper());
    final Runnable refresh = new Runnable() { public void run() { render(); handler.postDelayed(this, 1000); } };

    public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(Color.rgb(10, 22, 32));
        getWindow().setNavigationBarColor(Color.rgb(10, 22, 32));
        ScrollView scroll = new ScrollView(this); scroll.setFillViewport(true);
        LinearLayout body = new LinearLayout(this); body.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(24); body.setPadding(pad, pad, pad, pad); body.setBackgroundColor(Color.rgb(10, 22, 32));
        scroll.addView(body); setContentView(scroll);
        text(body, "SUBNAUTICA", 14, Color.rgb(79, 216, 206));
        text(body, "Android Multiplayer", 28, Color.WHITE);
        text(body, "Установка мода · предварительная версия", 14, Color.LTGRAY);
        game = text(body, "", 16, Color.WHITE);
        text(body, "1  Подготовка", 20, Color.rgb(79, 216, 206));
        text(body, "Нужна установленная собственная копия игры 1.22.83315. Установщик возьмёт её APK с телефона и добавит мод. Root и компьютер не нужны.", 16, Color.LTGRAY);
        prepare = button(body, "Подготовить мод");
        prepare.setOnClickListener(v -> new AlertDialog.Builder(this).setTitle("Подготовить файлы?")
            .setMessage("Сначала сохраните игру и выйдите в её главное меню. При первой подготовке понадобится место для копии ресурсов. Игра и миры на этом шаге не меняются.\n\nНе удаляйте установщик и не очищайте его данные: в нём хранится ключ для будущих обновлений.")
            .setNegativeButton("Отмена", null).setPositiveButton("Подготовить", (d, w) -> start("prepare")).show());
        text(body, "2  Установка", 20, Color.rgb(79, 216, 206));
        note = text(body, "", 16, Color.LTGRAY);
        install = button(body, "Установить подготовленный мод");
        install.setOnClickListener(v -> {
            try {
                PatchEngine engine = new PatchEngine(this);
                String blocked = engine.installBlocker(engine.plan());
                if (blocked != null) { alert(blocked); return; }
                if (!getPackageManager().canRequestPackageInstalls()) {
                    startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + getPackageName())));
                    return;
                }
                new AlertDialog.Builder(this).setTitle("Установить мод?")
                    .setMessage("Сохраните игру и выйдите в главное меню. Android попросит подтвердить установку и может перезапустить игру. Существующая игра автоматически не удаляется.")
                    .setNegativeButton("Отмена", null).setPositiveButton("Установить", (d,w) -> start("install")).show();
            } catch (Exception e) { alert(e.getMessage()); }
        });
        launch = button(body, "Открыть игру");
        launch.setOnClickListener(v -> {
            Intent intent = getPackageManager().getLaunchIntentForPackage(PatchEngine.GAME);
            if (intent != null) startActivity(intent); else alert("Игра пока не установлена");
        });
        cancel = button(body, "Отменить ожидающую установку Android");
        cancel.setOnClickListener(v -> {
            int id = PatchService.pendingSession(this);
            if (id >= 0) {
                try {
                    getPackageManager().getPackageInstaller().abandonSession(id);
                    getSharedPreferences("status", 0).edit().remove("session").apply();
                    PatchService.status(this, "Ожидающая установка отменена. Подготовленные файлы сохранены.");
                } catch (Exception e) { alert(e.getMessage()); }
            }
            render();
        });
        text(body, "Состояние", 20, Color.rgb(79, 216, 206));
        status = text(body, "", 15, Color.WHITE); status.setTextIsSelectable(true);
        Button report = button(body, "Скопировать отчёт");
        report.setOnClickListener(v -> {
            String value = game.getText() + "\n" + status.getText();
            ((android.content.ClipboardManager)getSystemService(CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("SNMP", value));
            Toast.makeText(this, "Отчёт скопирован", Toast.LENGTH_SHORT).show();
        });
        text(body, "Первое подключение: хост сохраняет мир, гость остаётся в главном меню. Оба телефона должны использовать одну сборку мода и одну Wi-Fi сеть.\n\nУстановщик не содержит игру, не читает миры и не отправляет данные в интернет.", 14, Color.LTGRAY);
        render();
    }

    void start(String action) {
        startForegroundService(new Intent(this, PatchService.class).setAction(action));
        status.setText("Запуск…");
    }
    void alert(String message) { new AlertDialog.Builder(this).setMessage(message).setPositiveButton("Понятно", null).show(); }
    public void onResume() { super.onResume(); handler.post(refresh); }
    public void onPause() { handler.removeCallbacks(refresh); super.onPause(); }

    void render() {
        try {
            PatchEngine engine = new PatchEngine(this); PackageInfo info = engine.installed();
            JSONObject mod = engine.payload();
            game.setText("\nИгра: " + (info == null ? "не найдена" : info.versionName) + "\nМод: " + mod.getString("buildId").substring(0,12) + " · протокол 23\n");
            boolean busy = PatchService.BUSY.get(); JSONObject plan = engine.plan();
            int pending = PatchService.pendingSession(this);
            prepare.setEnabled(!busy && info != null && pending < 0);
            String blocker = engine.installBlocker(plan);
            install.setEnabled(!busy && plan != null && blocker == null && pending < 0);
            cancel.setVisibility(pending < 0 ? View.GONE : View.VISIBLE);
            cancel.setEnabled(!busy);
            note.setText(plan == null ? "После подготовки здесь станет доступна установка." : (blocker == null
                ? "Файлы проверены. Android запросит разрешение и подтверждение установки."
                : blocker));
            launch.setEnabled(!busy && info != null);
            status.setText(getSharedPreferences("status", 0).getString("message", "Готов к подготовке. Сначала сохраните игру."));
        } catch (Exception e) {
            prepare.setEnabled(!PatchService.BUSY.get()); install.setEnabled(false);
            note.setText(e.getMessage());
            status.setText(getSharedPreferences("status", 0).getString("message", "Нужна повторная подготовка."));
        }
    }
    int dp(int value) { return (int)(getResources().getDisplayMetrics().density * value + .5f); }
    TextView text(LinearLayout body, String value, int size, int color) {
        TextView view = new TextView(this); view.setText(value); view.setTextSize(size); view.setTextColor(color);
        view.setPadding(0, dp(8), 0, dp(8)); body.addView(view); return view;
    }
    Button button(LinearLayout body, String title) {
        Button button = new Button(this); button.setText(title); button.setAllCaps(false);
        body.addView(button, new LinearLayout.LayoutParams(-1, -2)); return button;
    }
}
