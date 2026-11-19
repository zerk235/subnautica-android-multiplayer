package io.github.zerk235.snmp.installer;

import android.app.*;
import android.content.*;
import android.content.pm.PackageInfo;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.*;
import android.provider.Settings;
import android.text.SpannableString;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.text.format.Formatter;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.*;
import org.json.JSONObject;
import rikka.shizuku.Shizuku;
import java.io.*;
import java.util.UUID;

public final class MainActivity extends Activity {
    // Home screen views.
    TextView game, homeChip, status;
    Button launchHost, launchGuest, recoverGame, quickBackup, quickDiagnose, quickJournal;
    // Install screen views.
    TextView installStage, installGame, note, progressSize;
    ProgressBar installProgress;
    final TextView[] installPhases = new TextView[3];
    int shownPhase = -1;
    ImageView ocean;
    Button installError;
    Button install, cancel, homeInstall, nextInstall;
    Button installShizukuHelp, installImport, installExport, installWithoutBackup;
    LinearLayout installRoles;
    Runnable nextInstallAction;
    String modCheckKey;
    Boolean installedModMatches;
    // Worlds screen views.
    TextView savesNote, removalHint;
    Button backup, restore, export, importCopy, remove;
    // Settings screen views.
    TextView diagnosticsNote;
    LinearLayout infoVersion, infoGame, infoProtocol;
    Button diagnose, shareReport, saveReport, errorDetails, connect, guide;
    // Navigation.
    LinearLayout tabs[] = new LinearLayout[4];
    ScrollView screens[] = new ScrollView[4];
    LinearLayout bodies[] = new LinearLayout[4];
    TextView tabIcons[], tabLabels[];
    int activeTab;

    JSONObject profileState;
    Button recoverAssets;
    String profileError;
    boolean profilePending;
    long profileCheckedAt;
    String removalToken;
    final Shizuku.OnRequestPermissionResultListener permissionResult=(request,result)->{
        profileCheckedAt=0;
        render();
    };
    final Handler handler = new Handler(Looper.getMainLooper());
    final Runnable refresh = new Runnable() { public void run() { render(); handler.postDelayed(this, 1000); } };

    public void onCreate(Bundle state) {
        super.onCreate(state);
        if (state != null) removalToken = state.getString("removalToken");
        getWindow().setStatusBarColor(0x66041422);
        getWindow().setNavigationBarColor(Ui.BG);
        getWindow().getDecorView().setSystemUiVisibility(
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);

        FrameLayout backdrop = new FrameLayout(this);
        backdrop.setBackgroundColor(Ui.BG);
        ocean = installArt("patcher_ocean");
        backdrop.addView(ocean, new FrameLayout.LayoutParams(-1, -1));

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setOnApplyWindowInsetsListener((view, insets) -> {
            view.setPadding(0, insets.getSystemWindowInsetTop(), 0, 0);
            return insets;
        });
        FrameLayout content = new FrameLayout(this);
        for (int i = 0; i < 4; i++) {
            ScrollView scroll = new ScrollView(this);
            scroll.setFillViewport(true);
            if (i != 1) scroll.setBackgroundColor(Ui.BG);
            bodies[i] = screen();
            scroll.addView(bodies[i]);
            screens[i] = scroll;
            content.addView(scroll, new FrameLayout.LayoutParams(-1, -1));
        }
        root.addView(content, new LinearLayout.LayoutParams(-1, 0, 1f));
        LinearLayout.LayoutParams navigationParams = new LinearLayout.LayoutParams(-1, -2);
        navigationParams.setMargins(dp(8), dp(5), dp(8), dp(6));
        root.addView(bottomNav(), navigationParams);
        backdrop.addView(root, new FrameLayout.LayoutParams(-1, -1));
        setContentView(backdrop);
        buildHome(bodies[0]);
        buildInstall(bodies[1]);
        buildWorlds(bodies[2]);
        buildSettings(bodies[3]);
        selectTab(state == null ? 1 : state.getInt("tab", 1));
        Shizuku.addRequestPermissionResultListener(permissionResult);
        render();
    }

    // ---------- Screen builders ----------

    /** Content column for one tab; the caller wraps it in a ScrollView. */
    LinearLayout screen() {
        LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(18);
        body.setPadding(pad, pad, pad, dp(20));
        return body;
    }

    LinearLayout bottomNav() {
        LinearLayout nav = new LinearLayout(this);
        nav.setOrientation(LinearLayout.HORIZONTAL);
        nav.setGravity(Gravity.CENTER);
        nav.setBackground(Ui.lagoonSurface(this, 23, false));
        nav.setPadding(dp(4), dp(4), dp(4), dp(4));
        String[] icons = {"⌂", "↧", "▦", "⚙"};
        String[] labels = {"Главная", "Установка", "Миры", "Настройки"};
        tabIcons = new TextView[4]; tabLabels = new TextView[4];
        for (int i = 0; i < 4; i++) {
            LinearLayout tab = Ui.tab(this, icons[i], labels[i]);
            tab.setPadding(dp(2), dp(5), dp(2), dp(5));
            tab.setMinimumHeight(dp(48));
            final int index = i;
            tab.setOnClickListener(v -> selectTab(index));
            nav.addView(tab, new LinearLayout.LayoutParams(0, -2, 1f));
            tabIcons[i] = (TextView) tab.getChildAt(0);
            tabLabels[i] = (TextView) tab.getChildAt(1);
            tabs[i] = tab;
        }
        return nav;
    }

    void selectTab(int index) {
        activeTab = index;
        ocean.setVisibility(index == 1 ? View.VISIBLE : View.GONE);
        for (int i = 0; i < 4; i++) {
            screens[i].setVisibility(i == index ? View.VISIBLE : View.GONE);
            tabs[i].setSelected(i == index);
            tabs[i].setBackground(i == index ? Ui.lagoonSurface(this, 18, true) : null);
            int color = i == index ? Ui.CYAN : Ui.MUTED;
            tabIcons[i].setTextColor(color);
            tabLabels[i].setTextColor(i == index ? Color.WHITE : color);
        }
    }

    void buildHome(LinearLayout body) {
        LinearLayout title = new LinearLayout(this);
        title.setOrientation(LinearLayout.HORIZONTAL);
        title.setGravity(Gravity.CENTER_VERTICAL);
        TextView brand = new TextView(this);
        brand.setText("SUB");
        brand.setTextSize(26); brand.setTypeface(Typeface.DEFAULT_BOLD); brand.setTextColor(Ui.AMBER);
        TextView rest = new TextView(this);
        rest.setText("NAUTICA");
        rest.setTextSize(26); rest.setTypeface(Typeface.DEFAULT_BOLD); rest.setTextColor(Color.WHITE);
        String version = "";
        try { version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName; } catch (Exception ignored) { }
        TextView ver = Ui.chip(this, "v" + version, Ui.CYAN);
        LinearLayout.LayoutParams vp = new LinearLayout.LayoutParams(-2, -2);
        vp.setMargins(dp(10), 0, 0, 0);
        title.addView(brand); title.addView(rest); title.addView(ver, vp);
        body.addView(title);
        body.addView(Ui.text(this, "Android Multiplayer", 20, Ui.TEXT));
        body.addView(Ui.caption(this, "LAN Co-op мод для Android"));
        body.addView(gap(18));

        // Status card.
        LinearLayout statusCard = Ui.card(this);
        statusCard.addView(Ui.text(this, "Статус мода", 15, Ui.MUTED));
        statusCard.addView(gap(6));
        LinearLayout statusRow = new LinearLayout(this);
        statusRow.setOrientation(LinearLayout.HORIZONTAL);
        statusRow.setGravity(Gravity.CENTER_VERTICAL);
        TextView dot = new TextView(this); dot.setText("●"); dot.setTextColor(Ui.GREEN); dot.setTextSize(16);
        statusRow.addView(dot, new LinearLayout.LayoutParams(-2, -2));
        game = Ui.text(this, "", 14, Ui.TEXT);
        game.setPadding(dp(6), 0, 0, 0);
        statusRow.addView(game, new LinearLayout.LayoutParams(0, -2, 1f));
        statusCard.addView(statusRow);
        statusCard.addView(gap(6));
        homeChip = Ui.chip(this, "Проверка…", Ui.MUTED);
        homeChip.setLayoutParams(new LinearLayout.LayoutParams(-2, -2));
        statusCard.addView(homeChip);
        statusCard.addView(gap(8));
        homeInstall = Ui.button(this, "Установить или обновить мод", 0);
        homeInstall.setOnClickListener(v -> {
            selectTab(1);
            screens[1].scrollTo(0, 0);
        });
        statusCard.addView(homeInstall);
        recoverGame = Ui.button(this, "↩  Вернуть игру в текущий профиль", 1);
        recoverGame.setVisibility(View.GONE);
        recoverGame.setOnClickListener(v -> new AlertDialog.Builder(this).setTitle("Вернуть установленную копию игры?")
            .setMessage("Через Shizuku вернём в этот профиль уже имеющийся APK с прежней подписью. Клон и его миры останутся на месте. Это не перенос сохранений: после возвращения игры восстановите проверенную ZIP-копию миров.")
            .setNegativeButton("Отмена", null).setPositiveButton("Вернуть игру", (d, w) -> start("recover-game")).show());
        statusCard.addView(recoverGame);
        body.addView(statusCard);
        body.addView(gap(12));

        // Co-op card.
        LinearLayout coop = Ui.card(this);
        coop.addView(Ui.header(this, "Совместная игра"));
        coop.addView(gap(4));
        coop.addView(Ui.caption(this, "Играйте вместе по одной Wi-Fi сети."));
        coop.addView(gap(10));
        launchHost = roleButton(true);
        coop.addView(launchHost);
        coop.addView(gap(8));
        launchGuest = roleButton(false);
        coop.addView(launchGuest);
        body.addView(coop);
        body.addView(gap(12));

        // Connection state card.
        LinearLayout conn = Ui.card(this);
        conn.addView(Ui.text(this, "Последнее действие", 15, Ui.MUTED));
        conn.addView(gap(4));
        status = Ui.text(this, "Операций пока нет", 14, Ui.TEXT);
        conn.addView(status);
        body.addView(conn);
        body.addView(gap(12));

        // Quick actions.
        LinearLayout quick = Ui.card(this);
        quick.addView(Ui.text(this, "Быстрые действия", 15, Ui.MUTED));
        quick.addView(gap(10));
        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        quickBackup = Ui.button(this, "⟳\nКопия миров", 1);
        quickDiagnose = Ui.button(this, "✚\nДиагностика", 1);
        quickJournal = Ui.button(this, "≡\nЖурнал", 1);
        quickBackup.setOnClickListener(v -> confirmBackup());
        quickDiagnose.setOnClickListener(v -> diagnosticForm());
        quickJournal.setOnClickListener(v -> showLastError());
        int weight = 1;
        actions.addView(quickBackup, new LinearLayout.LayoutParams(0, -2, 1f));
        actions.addView(gapCol(dp(8)));
        actions.addView(quickDiagnose, new LinearLayout.LayoutParams(0, -2, 1f));
        actions.addView(gapCol(dp(8)));
        actions.addView(quickJournal, new LinearLayout.LayoutParams(0, -2, 1f));
        quick.addView(actions);
        body.addView(quick);
        body.addView(gap(12));
    }

    View gapCol(int widthPx) {
        View g = new View(this);
        g.setLayoutParams(new LinearLayout.LayoutParams(widthPx, 0));
        return g;
    }

    Button roleButton(boolean host) {
        Button button = Ui.button(this, host ? "Создать игру (хост)" : "Присоединиться (гость)", host ? 0 : 1);
        button.setOnClickListener(v -> new AlertDialog.Builder(this)
            .setTitle(host ? "Этот телефон — хост" : "Этот телефон — гость")
            .setMessage(host
                ? "Сохранитесь и выйдите в меню, если игра открыта. После запуска загрузите и сохраните мир, которым хотите поделиться. На втором телефоне выберите «Присоединиться (гость)»."
                : "Оставьте игру в главном меню. После того как хост загрузит мир, мод получит его и подключится. Если локальный мир с таким именем отличается, его папка будет сохранена отдельно.")
            .setNegativeButton("Отмена", null)
            .setPositiveButton(host ? "Запустить хостом" : "Подключиться гостем", (d, w) -> launchGame(host ? "host" : "guest"))
            .show());
        return button;
    }

    ImageView installArt(String name) {
        ImageView view = new ImageView(this);
        view.setScaleType(ImageView.ScaleType.CENTER_CROP);
        view.setImageResource(getResources().getIdentifier(name, "drawable", getPackageName()));
        view.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        return view;
    }

    LinearLayout installMasthead() {
        LinearLayout masthead = new LinearLayout(this);
        masthead.setOrientation(LinearLayout.VERTICAL);
        masthead.setGravity(Gravity.CENTER);
        masthead.setMinimumHeight(dp(184));
        masthead.setPadding(0, 0, 0, dp(15));
        SpannableString brand = new SpannableString("SUBNAUTICA");
        brand.setSpan(new ForegroundColorSpan(0xffffa24b), 0, 3, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        TextView name = Ui.text(this, "", 31, 0xffe2f7ff);
        name.setText(brand);
        name.setTypeface(Typeface.create("sans-serif-black", Typeface.ITALIC));
        name.setShadowLayer(dp(4), 0, dp(2), 0xe7001421);
        masthead.addView(name, new LinearLayout.LayoutParams(-2, -2));
        TextView patcher = Ui.text(this, "Patcher", 17, 0xffb7f5ff);
        patcher.setTypeface(Typeface.DEFAULT_BOLD);
        patcher.setPadding(dp(22), dp(3), dp(22), dp(3));
        patcher.setBackground(Ui.lagoonSurface(this, 16, false));
        masthead.addView(patcher, new LinearLayout.LayoutParams(-2, -2));
        return masthead;
    }

    LinearLayout buildInstallPhases() {
        LinearLayout row = new LinearLayout(this);
        String[] labels = {"Подготовка", "Установка", "Готово"};
        for (int phase = 0; phase < labels.length; phase++) {
            TextView view = Ui.text(this, labels[phase], 12, Ui.TEXT);
            view.setGravity(Gravity.CENTER);
            view.setMinimumHeight(dp(36));
            view.setPadding(dp(4), dp(5), dp(4), dp(5));
            LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0, -1, 1f);
            if (phase > 0) params.setMarginStart(dp(5));
            row.addView(view, params);
            installPhases[phase] = view;
        }
        return row;
    }

    void showInstallPhase(int phase) {
        if (shownPhase == phase) return;
        shownPhase = phase;
        for (int index = 0; index < installPhases.length; index++) {
            TextView view = installPhases[index];
            boolean active = index == phase;
            view.setBackground(Ui.lagoonSurface(this, 18, active));
            view.setTextColor(active ? Color.WHITE : Ui.MUTED);
            view.setTypeface(active ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
            view.setSelected(active);
        }
    }

    void showInstallProgress(boolean busy, boolean ready, SharedPreferences statusPrefs) {
        long total = busy ? statusPrefs.getLong("progressTotal", 0) : 0;
        installProgress.setIndeterminate(busy && total <= 0);
        progressSize.setVisibility(total > 0 ? View.VISIBLE : View.GONE);
        if (total > 0) {
            long done = Math.max(0, Math.min(total, statusPrefs.getLong("progressDone", 0)));
            installProgress.setProgress((int) Math.min(100, done * 100 / total));
            updateText(progressSize, Formatter.formatShortFileSize(this, done) + " / "
                + Formatter.formatShortFileSize(this, total));
        } else if (!busy) {
            installProgress.setProgress(ready ? 100 : 0);
        }
    }

    LinearLayout buildInstallGameRow() {
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(9), dp(7), dp(10), dp(7));
        row.setBackground(Ui.lagoonSurface(this, 15, false));
        ImageView image = installArt("install_reef");
        image.setBackground(Ui.round(this, Ui.CARD, 10, 0, 0));
        image.setClipToOutline(true);
        row.addView(image, new LinearLayout.LayoutParams(dp(52), dp(44)));
        LinearLayout labels = new LinearLayout(this);
        labels.setOrientation(LinearLayout.VERTICAL);
        labels.setPaddingRelative(dp(10), 0, 0, 0);
        TextView title = Ui.header(this, "Subnautica вдвоём");
        title.setTextSize(16);
        title.setAccessibilityHeading(true);
        labels.addView(title);
        installGame = Ui.caption(this, "");
        installGame.setTextColor(Color.rgb(186, 224, 227));
        labels.addView(installGame);
        row.addView(labels, new LinearLayout.LayoutParams(0, -2, 1f));
        return row;
    }

    void buildInstall(LinearLayout body) {
        body.setPadding(dp(9), 0, dp(9), dp(12));
        body.addView(installMasthead());
        LinearLayout panel = Ui.card(this);
        panel.setBackground(Ui.lagoonSurface(this, 25, false));
        panel.setPadding(dp(9), dp(9), dp(9), dp(10));
        panel.addView(buildInstallPhases(), new LinearLayout.LayoutParams(-1, -2));
        panel.addView(gap(9));
        panel.addView(buildInstallGameRow());
        panel.addView(gap(8));
        LinearLayout next = Ui.card(this);
        next.setBackground(Ui.lagoonSurface(this, 17, false));
        next.setPadding(dp(12), dp(10), dp(12), dp(10));
        installStage = Ui.header(this, "Проверяем игру…");
        installStage.setAccessibilityHeading(true);
        next.addView(installStage);
        next.addView(gap(6));
        note = Ui.text(this, "", 15, Ui.TEXT);
        next.addView(note);
        next.addView(gap(12));
        progressSize = Ui.caption(this, "");
        progressSize.setGravity(Gravity.END);
        progressSize.setVisibility(View.GONE);
        next.addView(progressSize);
        installProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        installProgress.setContentDescription("Прогресс установки мода");
        installProgress.setMax(100);
        installProgress.setProgressTintList(ColorStateList.valueOf(0xff48e3f1));
        installProgress.setProgressBackgroundTintList(ColorStateList.valueOf(0xff154d62));
        installProgress.setIndeterminateTintList(ColorStateList.valueOf(0xff48e3f1));
        next.addView(installProgress, new LinearLayout.LayoutParams(-1, dp(12)));
        nextInstall = Ui.button(this, "Подготовить игру", 0);
        Ui.installPrimary(this, nextInstall);
        nextInstall.setOnClickListener(v -> {
            if (nextInstallAction != null) nextInstallAction.run();
        });
        next.addView(gap(8));
        next.addView(nextInstall);
        installRoles = new LinearLayout(this);
        installRoles.setOrientation(LinearLayout.VERTICAL);
        Button createGame = roleButton(true);
        Ui.installPrimary(this, createGame);
        installRoles.addView(createGame);
        installRoles.addView(gap(8));
        installRoles.addView(roleButton(false));
        next.addView(installRoles);
        installRoles.setVisibility(View.GONE);
        installShizukuHelp = Ui.button(this, "Как запустить Shizuku", 1);
        installShizukuHelp.setOnClickListener(v -> shizukuHelp());
        next.addView(installShizukuHelp);
        installWithoutBackup = Ui.button(this, "Без Shizuku · удалить миры", 2);
        installWithoutBackup.setOnClickListener(v -> confirmReplaceWithoutBackup());
        installWithoutBackup.setVisibility(View.GONE);
        next.addView(installWithoutBackup);
        panel.addView(next);
        body.addView(panel);
        body.addView(gap(8));

        installError = Ui.button(this, "", 1);
        installError.setOnClickListener(v -> {
            JSONObject error = Diagnostics.lastError(this);
            if (error.has("code")) new AlertDialog.Builder(this)
                .setTitle(error.optString("title"))
                .setMessage(error.optString("meaning") + "\n\n" + error.optString("steps"))
                .setPositiveButton("Понятно", null)
                .setNeutralButton("Подробности", (dialog, which) -> showLastError()).show();
        });
        installError.setVisibility(View.GONE);
        body.addView(installError);

        install = Ui.button(this, "Установить подготовленный мод", 0);
        install.setOnClickListener(v -> {
            try {
                PatchEngine engine = new PatchEngine(this);
                String blocked = engine.installBlocker(engine.plan());
                if (blocked != null) { alert(blocked); return; }
                if (!getPackageManager().canRequestPackageInstalls()) {
                    openInstallPermission();
                    return;
                }
                new AlertDialog.Builder(this).setTitle("Установить мод?")
                    .setMessage("Сохраните игру и выйдите в главное меню. Android попросит подтвердить установку и может перезапустить игру. Существующая игра автоматически не удаляется.")
                    .setNegativeButton("Отмена", null).setPositiveButton("Установить", (d, w) -> start("install")).show();
            } catch (Exception e) { showError("install-check", e); }
        });
        recoverAssets = Ui.button(this, "Восстановить пакет ресурсов", 1);
        recoverAssets.setVisibility(View.GONE);
        recoverAssets.setOnClickListener(v -> {
            if (!getPackageManager().canRequestPackageInstalls()) {
                openInstallPermission();
                return;
            }
            startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
                .setType("*/*"), 705);
        });
        installImport = Ui.button(this, "Открыть ZIP-копию миров", 1);
        installImport.setOnClickListener(v -> importCopy.performClick());
        body.addView(installImport);
        installExport = Ui.button(this, "Сохранить копию миров в ZIP", 1);
        installExport.setOnClickListener(v -> export.performClick());
        body.addView(installExport);
        cancel = Ui.button(this, "Отменить ожидающую установку Android", 1);
        cancel.setOnClickListener(v -> {
            int id = PatchService.pendingSession(this);
            if (id >= 0) {
                try {
                    getPackageManager().getPackageInstaller().abandonSession(id);
                    getSharedPreferences("status", 0).edit().remove("session").apply();
                    PatchService.status(this, "Ожидающая установка отменена. Подготовленные файлы сохранены.");
                } catch (Exception e) { showError("cancel-install", e); }
            }
            render();
        });
        cancel.setVisibility(View.GONE);
        LinearLayout.LayoutParams cancelParams = new LinearLayout.LayoutParams(-1, -2);
        cancelParams.topMargin = dp(8);
        panel.addView(cancel, cancelParams);
    }

    void openInstallPermission() {
        startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + getPackageName())));
    }

    void buildWorlds(LinearLayout body) {
        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        TextView icon = Ui.text(this, "▦", 22, Ui.CYAN);
        icon.setPadding(0, 0, dp(10), 0);
        head.addView(icon);
        head.addView(Ui.header(this, "Управление мирами"));
        body.addView(head);
        body.addView(gap(4));
        body.addView(Ui.caption(this, "Резервные копии, синхронизация\nи перенос сохранений."));
        body.addView(gap(14));

        LinearLayout worlds = Ui.card(this);
        savesNote = Ui.text(this, "", 13, Ui.MUTED);
        worlds.addView(savesNote);
        body.addView(worlds);
        body.addView(gap(12));

        backup = Ui.button(this, "Создать копию миров", 1);
        backup.setOnClickListener(v -> confirmBackup());
        body.addView(backup);
        body.addView(gap(10));

        export = Ui.button(this, "  ⤓  Создать ZIP-архив\n      Сохранить миры и настройки", 1);
        export.setOnClickListener(v -> {
            try {
                if (new SaveManager(this).latest() == null) { alert("Сначала создайте копию миров"); return; }
                startActivityForResult(new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("application/zip")
                    .addCategory(Intent.CATEGORY_OPENABLE).putExtra(Intent.EXTRA_TITLE, "Subnautica-worlds-" + System.currentTimeMillis() + ".zip"), 702);
            } catch (Exception error) { alert(error.getMessage()); }
        });
        body.addView(export);
        body.addView(gap(10));

        importCopy = Ui.button(this, "Открыть ZIP-копию миров", 1);
        importCopy.setOnClickListener(v -> startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
            .setType("application/zip").addCategory(Intent.CATEGORY_OPENABLE), 703));
        body.addView(importCopy);
        body.addView(gap(10));

        restore = Ui.button(this, "Восстановить проверенную копию", 3);
        restore.setOnClickListener(v -> new AlertDialog.Builder(this).setTitle("Восстановить проверенную копию?")
            .setMessage("Игра будет остановлена. Сначала запишем миры во временную папку, проверим каждый файл и только затем подключим их к игре. Если в игре уже есть миры, их папка останется рядом как резервная. Несохранённый прогресс запущенной игры потеряется — сохранитесь и выйдите в меню заранее.")
            .setNegativeButton("Отмена", null).setPositiveButton("Восстановить", (d, w) -> start("restore")).show());
        body.addView(restore);
        body.addView(gap(16));

        LinearLayout danger = Ui.card(this);
        danger.setBackground(Ui.round(this, Ui.RED_DIM, 18, Ui.RED, 1));
        remove = Ui.button(this, "   Удалить старую игру\nТолько после резервной копии", 2);
        remove.setOnClickListener(v -> confirmReplace());
        danger.addView(remove);
        removalHint = Ui.text(this, "", 13, Ui.MUTED);
        danger.addView(removalHint);
        body.addView(danger);
    }

    void buildSettings(LinearLayout body) {
        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        TextView icon = Ui.text(this, "⚙", 22, Ui.CYAN);
        icon.setPadding(0, 0, dp(10), 0);
        head.addView(icon);
        head.addView(Ui.header(this, "Настройки и диагностика"));
        body.addView(head);
        body.addView(gap(4));
        body.addView(Ui.caption(this, "Параметры мода, информация\nи инструменты для разработчиков."));
        body.addView(gap(14));

        LinearLayout general = Ui.card(this);
        general.addView(Ui.header(this, "Общее"));
        general.addView(gap(8));
        connect = Ui.button(this, "Подключить Shizuku", 1);
        connect.setOnClickListener(v -> connectShizuku());
        general.addView(connect);
        general.addView(gap(8));
        guide = Ui.button(this, "Как запустить Shizuku без компьютера", 1);
        guide.setOnClickListener(v -> startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse("https://shizuku.rikka.app/guide/setup/"))));
        general.addView(guide);
        general.addView(gap(8));
        Button help = Ui.button(this, "Помощь с Shizuku", 1);
        help.setOnClickListener(v -> shizukuHelp());
        general.addView(help);
        body.addView(general);
        body.addView(gap(12));

        LinearLayout info = Ui.card(this);
        info.addView(Ui.header(this, "Информация"));
        info.addView(gap(8));
        infoVersion = Ui.infoRow(this, "Версия мода", "…");
        info.addView(infoVersion);
        infoGame = Ui.infoRow(this, "Версия игры", "…");
        info.addView(infoGame);
        infoProtocol = Ui.infoRow(this, "Протокол", "…");
        info.addView(infoProtocol);
        body.addView(info);
        body.addView(gap(12));

        LinearLayout diag = Ui.card(this);
        diag.addView(Ui.header(this, "Диагностика"));
        diag.addView(gap(10));
        diagnose = Ui.button(this, "Собрать диагностический ZIP", 1);
        diagnose.setOnClickListener(v -> diagnosticForm());
        diag.addView(diagnose);
        diag.addView(gap(8));
        shareReport = Ui.button(this, "Отправить ZIP", 1);
        shareReport.setOnClickListener(v -> shareDiagnostic());
        diag.addView(shareReport);
        diag.addView(gap(8));
        saveReport = Ui.button(this, "Сохранить отчёт", 1);
        saveReport.setOnClickListener(v -> saveDiagnostic());
        diag.addView(saveReport);
        diag.addView(gap(8));
        errorDetails = Ui.button(this, "Последняя ошибка — подробности", 1);
        errorDetails.setOnClickListener(v -> showLastError());
        diag.addView(errorDetails);
        diag.addView(gap(8));
        Button copyReport = Ui.button(this, "Скопировать отчёт", 1);
        copyReport.setOnClickListener(v -> {
            String value = game.getText() + "\n" + status.getText() + "\n\n" + DiagnosticError.readable(Diagnostics.lastError(this))
                + "\n\nДля полного отчёта нажмите «Создать диагностический ZIP».";
            ((android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("SNMP", value));
            Toast.makeText(this, "Отчёт скопирован", Toast.LENGTH_SHORT).show();
        });
        diag.addView(copyReport);
        diag.addView(gap(10));
        diagnosticsNote = Ui.text(this, "", 13, Ui.MUTED);
        diag.addView(diagnosticsNote);
        body.addView(diag);
        body.addView(gap(12));

        LinearLayout about = Ui.card(this);
        about.addView(Ui.text(this, "Первое подключение: хост сохраняет мир, гость остаётся в главном меню. Нужны одна сборка мода и одна Wi-Fi сеть.\n\nС разрешения Shizuku установщик копирует и восстанавливает SavedGames, а по кнопке диагностики читает журналы Subnautica и logcat её текущего процесса в этом профиле. Копии остаются на телефоне. Установщик не содержит игру и не отправляет данные в интернет.", 13, Ui.MUTED));
        body.addView(about);
    }

    // ---------- Actions ----------

    void connectShizuku() {
        try {
            if (!Shizuku.pingBinder()) {
                Intent launch = getPackageManager().getLaunchIntentForPackage("moe.shizuku.privileged.api");
                if (launch != null) startActivity(launch);
                else startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse("https://shizuku.rikka.app/download/")));
            } else if (Shizuku.checkSelfPermission() != android.content.pm.PackageManager.PERMISSION_GRANTED) Shizuku.requestPermission(701);
            else {
                profileCheckedAt=0;
                refreshProfiles();
                render();
            }
        } catch (Exception error) { showError("shizuku-connect", error); }
    }

    void shizukuHelp() {
        new AlertDialog.Builder(this).setTitle("Помощь с Shizuku")
            .setMessage("ЗАЧЕМ НУЖЕН SHIZUKU\nAndroid защищает папку миров другой игры. С вашим разрешением Shizuku позволяет установщику скопировать и восстановить SavedGames. Для подготовки APK он не нужен.\n\n"
                + "ЗАПУСК БЕЗ КОМПЬЮТЕРА — ANDROID 11+\n1. Установите официальный Shizuku.\n2. Включите возможности разработчика и отладку по Wi-Fi.\n3. В Shizuku выполните сопряжение с кодом из окна «Подключить устройство с помощью кода» и запустите службу.\n4. Вернитесь сюда → «Подключить Shizuku» → разрешите доступ для SN Multiplayer Installer.\nПосле перезагрузки телефона Shizuku может потребоваться запустить заново. На Android 10 беспроводной запуск без компьютера и root не поддерживается.\n\n"
                + "ОШИБКА «ДОСТУП К ADB ОГРАНИЧЕН»\nНа некоторых realme / OPPO / OnePlus доступ ограничивает прошивка. Для realme GT8 с V17.0.0 найден пункт Disable system optimization («Выключить оптимизацию системы»). При активном ограничении он скрывается на русском языке.\n\n"
                + "Что сделать на этой прошивке:\n1. Временно смените системный язык на English.\n2. Откройте Settings → System & updates → Developer options.\n3. Ближе к концу списка найдите Disable system optimization и включите его. Прочитайте системное предупреждение: возможен повышенный расход батареи в фоне.\n4. Перезапустите Shizuku.\n5. Здесь снова нажмите «Подключить Shizuku» и разрешите доступ.\n6. Русский язык можно вернуть.\n\n"
                + "На других прошивках пункт может называться «Отключить мониторинг разрешений»; если он называется «Мониторинг разрешений», его требуется выключить. Не меняйте похожие настройки наугад. «Запретить доступ к отладке с помощью adb» и ограничение дочерних процессов — другие параметры. Установщик сам настройки системы не переключает.\n\n"
                + "ЕСЛИ ДОСТУП НЕ ПОЯВИЛСЯ\nПроверьте, что Shizuku сообщает о работающей службе. Закройте и снова откройте установщик, затем нажмите подключение. Данные установщика не очищайте: там находятся ключ подписи и копии миров.\n\n"
                + "Нужный результат: «Shizuku: доступ предоставлен». Затем сохраните игру, выйдите в её главное меню и создайте копию миров. До сообщения об успешной проверке копии игру не удаляйте.")
            .setPositiveButton("Понятно", null).setNeutralButton("Официальная инструкция", (d, w) -> startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse("https://shizuku.rikka.app/guide/setup/")))).show();
    }

    void confirmBackup() {
        new AlertDialog.Builder(this).setTitle("Сохранить все миры?")
            .setMessage("Сначала сохраните игру, дождитесь окончания сохранения и выйдите в главное меню. Будет скопирована папка SavedGames со всеми мирами, включая ячейки мира. Приватные настройки игры не копируются. Игру не удаляем.")
            .setNegativeButton("Отмена", null).setPositiveButton("Создать копию", (d, w) -> start("backup")).show();
    }

    void confirmPrepare() {
        new AlertDialog.Builder(this).setTitle("Подготовить файлы?")
            .setMessage("Сначала сохраните игру и выйдите в главное меню. Установщик подготовит отдельные файлы с модом. Если понадобится заменить игру, следующим шагом будет проверенная копия миров.")
            .setNegativeButton("Отмена", null).setPositiveButton("Подготовить", (d, w) -> start("prepare")).show();
    }

    void confirmReplace() {
        new AlertDialog.Builder(this).setTitle("Проверить копию перед заменой?")
            .setMessage("Сохранитесь и выйдите в главное меню заранее: для финальной проверки игра будет остановлена. Проверим APK, SHA256 копии и совпадение текущих миров с копией. Затем Android отдельно попросит подтвердить удаление Subnautica. После удаления установите подготовленный мод и восстановите миры. До конца замены игру не открывайте. Сам установщик не удаляйте.")
            .setNegativeButton("Отмена", null).setPositiveButton("Проверить", (d, w) -> checkReplacement("check-removal")).show();
    }

    void confirmReplaceWithoutBackup() {
        if (!getPackageManager().canRequestPackageInstalls()) {
            openInstallPermission();
            return;
        }
        CheckBox accept = new CheckBox(this);
        accept.setText("Я понимаю, что потеряю миры и прогресс");
        accept.setPadding(dp(20), dp(8), dp(20), dp(8));
        AlertDialog dialog = new AlertDialog.Builder(this).setTitle("Установить без Shizuku?")
            .setMessage("Android удалит Subnautica вместе с мирами и прогрессом в этом профиле. Патчер не создаст их копию. "
                + "Ранее сохранённые ZIP останутся доступны. Если игра есть в клоне или другом профиле, используйте способ с Shizuku. "
                + "После проверки APK Android отдельно попросит подтвердить удаление.")
            .setView(accept).setNegativeButton("Отмена", null)
            .setPositiveButton("Продолжить без копии", (d, w) -> checkReplacement("check-removal-without-backup"))
            .create();
        dialog.setOnShowListener(d -> {
            Button proceed = dialog.getButton(AlertDialog.BUTTON_POSITIVE);
            proceed.setEnabled(false);
            accept.setOnCheckedChangeListener((button, checked) -> proceed.setEnabled(checked));
        });
        dialog.show();
    }

    void checkReplacement(String action) {
        try {
            removalToken = UUID.randomUUID().toString();
            startForegroundService(new Intent(this, PatchService.class).setAction(action).putExtra("token", removalToken));
        } catch (Exception error) {
            removalToken = null;
            showError(action, error);
        }
    }

    void confirmExperimental(PackageInfo game) {
        new AlertDialog.Builder(this).setTitle("Проверить Subnautica " + game.versionName + "?")
            .setMessage(PatchEngine.EXPERIMENTAL_NOTICE
                + "\n\nПеред подготовкой патчер сверит файлы движка. Если они отличаются, установка остановится. "
                + "Сохраните миры отдельно, прежде чем тестировать мод.")
            .setNegativeButton("Отмена", null)
            .setPositiveButton("Проверить и подготовить", (d, w) -> {
                try {
                    startForegroundService(new Intent(this, PatchService.class).setAction("prepare-experimental")
                        .putExtra("acceptedVersion", game.versionName).putExtra("acceptedCode", game.getLongVersionCode()));
                } catch (Exception error) { showError("prepare-experimental", error); }
            }).show();
    }

    void start(String action) {
        try {
            startForegroundService(new Intent(this, PatchService.class).setAction(action));
            updateText(status, "Запуск…");
        } catch (Exception error) { showError(action, error); }
    }

    void launchGame(String role) {
        try {
            if (!PatchEngine.hasAssets(new PatchEngine(this).installed())) {
                alert("В игре отсутствует пакет ресурсов. Откройте вкладку «Установка» и нажмите «Восстановить пакет ресурсов».");
                return;
            }
        } catch (Exception e) { showError("launch-" + role, e); return; }
        Intent intent = getPackageManager().getLaunchIntentForPackage(PatchEngine.GAME);
        if (intent == null) { showError("launch-" + role, new IOException("Игра пока не установлена")); return; }
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        intent.putExtra("snmp_launch_role", role);
        intent.putExtra("snmp_launch_request", UUID.randomUUID().toString());
        try { startActivity(intent); Diagnostics.event(this, "launch_game", "requested role=" + role); }
        catch (Exception error) { showError("launch-" + role, error); }
    }

    void showError(String action, Throwable error) {
        String message = Diagnostics.failure(this, action, error); PatchService.status(this, message);
        new AlertDialog.Builder(this).setTitle("Не удалось завершить действие").setMessage(message)
            .setPositiveButton("Понятно", null).setNeutralButton("Подробности", (d, w) -> showLastError()).show();
    }

    void showLastError() {
        JSONObject error = Diagnostics.lastError(this);
        String message = error.has("code") ? DiagnosticError.readable(error) : "Ошибок установщика пока не записано. Это не исключает сбоя в игре — создайте диагностический ZIP.";
        TextView text = new TextView(this); text.setText(message); text.setTextColor(Color.WHITE); text.setPadding(dp(18), dp(12), dp(18), dp(12)); text.setTextIsSelectable(true);
        ScrollView scroll = new ScrollView(this); scroll.addView(text);
        new AlertDialog.Builder(this).setTitle("Последняя сохранённая ошибка").setView(scroll).setPositiveButton("Закрыть", null).show();
    }

    void diagnosticForm() {
        LinearLayout form = new LinearLayout(this); form.setOrientation(LinearLayout.VERTICAL); form.setPadding(dp(18), dp(8), dp(18), 0);
        Spinner category = new Spinner(this); category.setAdapter(new ArrayAdapter<String>(this, android.R.layout.simple_spinner_dropdown_item,
            new String[]{"Установка и обновление", "Подключение и мир", "Игрок исчез или застыл", "Строительство базы", "Предметы, анимации и свет", "Другое"})); form.addView(category);
        EditText description = new EditText(this); description.setTextColor(Color.WHITE); description.setHintTextColor(Color.LTGRAY);
        description.setHint("Что произошло и после какого действия? Кто хост? Примерное время ошибки? Что ожидали увидеть?");
        description.setMinLines(4); description.setMaxLines(7); description.setInputType(android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        description.setFilters(new android.text.InputFilter[]{new android.text.InputFilter.LengthFilter(4000)}); form.addView(description);
        new AlertDialog.Builder(this).setTitle("Отчёт разработчику").setView(form)
            .setMessage("Опишите проблему. Журналы соберутся без остановки игры. Если Shizuku недоступен, это будет явно указано в частичном отчёте. Отправка только по вашему выбору.")
            .setNegativeButton("Отмена", null).setPositiveButton("Собрать ZIP", (d, w) -> {
                try {
                    startForegroundService(new Intent(this, PatchService.class).setAction("diagnostics")
                        .putExtra("category", category.getSelectedItem().toString()).putExtra("description", description.getText().toString()));
                } catch (Exception error) { showError("diagnostics-start", error); }
            }).show();
    }

    void shareDiagnostic() {
        File report = Diagnostics.latest(this); if (report == null) { alert("Сначала создайте диагностический ZIP"); return; }
        Uri uri = ReportProvider.uri(this, report);
        Intent send = new Intent(Intent.ACTION_SEND).setType("application/zip").putExtra(Intent.EXTRA_STREAM, uri)
            .putExtra(Intent.EXTRA_SUBJECT, "SNMP · диагностический отчёт").putExtra(Intent.EXTRA_TEXT, "Отчёт SNMP. При проблеме синхронизации приложите ZIP со второго телефона и укажите хоста.")
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        send.setClipData(ClipData.newRawUri("SNMP diagnostic", uri));
        try { startActivity(Intent.createChooser(send, "Отправить диагностический ZIP")); }
        catch (Exception error) { showError("diagnostics-share", error); }
    }

    void saveDiagnostic() {
        File report = Diagnostics.latest(this); if (report == null) { alert("Сначала создайте диагностический ZIP"); return; }
        getSharedPreferences("diagnostics", 0).edit().putString("exportReport", report.getName()).apply();
        try { startActivityForResult(new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("application/zip").addCategory(Intent.CATEGORY_OPENABLE)
            .putExtra(Intent.EXTRA_TITLE, report.getName()), 704); }
        catch (Exception error) { showError("diagnostics-save", error); }
    }

    void alert(String message) { new AlertDialog.Builder(this).setMessage(message).setPositiveButton("Понятно", null).show(); }

    void updateText(TextView view, CharSequence value) {
        if (!android.text.TextUtils.equals(view.getText(), value)) view.setText(value);
    }

    public void onResume() { super.onResume(); handler.post(refresh); }
    public void onPause() { handler.removeCallbacks(refresh); super.onPause(); }
    public void onDestroy() { Shizuku.removeRequestPermissionResultListener(permissionResult); super.onDestroy(); }
    @Override protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state);
        state.putString("removalToken", removalToken);
        state.putInt("tab", activeTab);
    }

    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == 705) {
            if (result == RESULT_OK && data != null && data.getData() != null) {
                try {
                    startForegroundService(new Intent(this, PatchService.class).setAction("recover-assets")
                        .setData(data.getData()).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION));
                } catch (Exception error) { showError("recover-assets", error); }
            }
            return;
        }
        if ((request != 702 && request != 703 && request != 704) || result != RESULT_OK || data == null || data.getData() == null) return;
        if (!PatchService.BUSY.compareAndSet(false, true)) { alert("Дождитесь текущей операции"); return; }
        Uri uri = data.getData();
        final String action = request == 704 ? "diagnostics-save" : request == 703 ? "backup-import" : "backup-export";
        Diagnostics.begin(this, action);
        new Thread(() -> {
            try {
                if (request == 704) {
                    String name = getSharedPreferences("diagnostics", 0).getString("exportReport", "");
                    File report = ReportProvider.file(this, name);
                    try (InputStream in = new FileInputStream(report); OutputStream out = getContentResolver().openOutputStream(uri, "w")) {
                        if (out == null) throw new IOException("Папка не разрешила запись отчёта");
                        byte[] buffer = new byte[65536]; int n; while ((n = in.read(buffer)) != -1) out.write(buffer, 0, n);
                    }
                    String expected = ApkPatcher.sha256(report);
                    try (InputStream in = getContentResolver().openInputStream(uri)) {
                        java.security.MessageDigest digest = java.security.MessageDigest.getInstance("SHA-256"); byte[] buffer = new byte[65536]; int n;
                        while ((n = in.read(buffer)) != -1) digest.update(buffer, 0, n);
                        if (!expected.equals(ApkPatcher.hex(digest.digest()))) throw new IOException("Сохранённый отчёт не прошёл проверку SHA256");
                    }
                    PatchService.status(this, "Диагностический ZIP сохранён и проверен. Отправьте его разработчику вместе с описанием проблемы."); return;
                }
                if (request == 703) {
                    new SaveManager(this).importArchive(getContentResolver().openInputStream(uri));
                    PatchService.status(this, "ZIP-копия открыта и проверена. Нажмите «Восстановить миры», чтобы вернуть её в установленную игру.");
                    return;
                }
                SaveManager manager = new SaveManager(this); JSONObject record = manager.verify();
                try (InputStream in = new FileInputStream(manager.archive(record)); OutputStream out = getContentResolver().openOutputStream(uri, "w")) {
                    if (out == null) throw new IOException("Папка не разрешила запись");
                    byte[] buffer = new byte[128 * 1024]; int n; while ((n = in.read(buffer)) != -1) out.write(buffer, 0, n);
                }
                try (InputStream in = getContentResolver().openInputStream(uri)) { SaveArchive.inspect(in, null); }
                PatchService.status(this, "ZIP-копия сохранена в выбранную папку и проверена. Копия внутри установщика тоже сохранена.");
            } catch (Exception error) { PatchService.status(this, Diagnostics.failure(this, action, error)); }
            finally { Diagnostics.finish(this, action); PatchService.BUSY.set(false); }
        }, "snmp-export").start();
    }

    void render() {
        boolean reportBusy = PatchService.BUSY.get(); File diagnostic = Diagnostics.latest(this);
        diagnose.setEnabled(!reportBusy); shareReport.setEnabled(!reportBusy && diagnostic != null); saveReport.setEnabled(!reportBusy && diagnostic != null);
        errorDetails.setEnabled(Diagnostics.lastError(this).has("code"));
        updateText(diagnosticsNote, reportBusy ? "Выполняется операция. Состояние показано в уведомлении и внизу экрана." : diagnostic == null ? "Диагностический ZIP ещё не создан."
            : "Последний ZIP: " + new java.util.Date(diagnostic.lastModified()) + " · " + (diagnostic.length() / 1024) + " КиБ\n" + getSharedPreferences("diagnostics", 0).getString("reportCompleteness", "Состав и время игровых данных указаны внутри ZIP."));
        try {
            PatchEngine engine = new PatchEngine(this); PackageInfo info = engine.installed();
            JSONObject mod = engine.payload();
            updateInfoRow(infoVersion, getPackageManager().getPackageInfo(getPackageName(), 0).versionName);
            updateInfoRow(infoGame, info == null ? "не найдена" : info.versionName);
            updateInfoRow(infoProtocol, String.valueOf(mod.getInt("protocol")));
            String gameLabel = "Игра: " + (info == null ? "не найдена" : info.versionName) + "\nМод: " + mod.getString("buildId").substring(0, 12) + " · протокол " + mod.getInt("protocol");
            if (info == null && engine.retained() != null) gameLabel += "\nИгра осталась в другом профиле / клоне.";
            updateText(game, gameLabel);
            updateText(installGame, info == null ? "Игра в этом профиле не найдена"
                : "Версия игры · " + info.versionName);
            boolean busy = PatchService.BUSY.get();
            JSONObject plan = null; String planError = null;
            try { plan = engine.plan(); }
            catch (Exception invalidPlan) { planError = invalidPlan.getMessage(); }
            int pending = PatchService.pendingSession(this);
            boolean missingAssets = info != null && PatchEngine.GAME_VERSION.equals(info.versionName) && !PatchEngine.hasAssets(info);
            recoverAssets.setVisibility(missingAssets ? View.VISIBLE : View.GONE);
            recoverAssets.setEnabled(!busy && pending < 0);
            SaveManager worlds = new SaveManager(this); JSONObject saved = worlds.latest();
            if (info == null && saved != null && !getSharedPreferences("status", 0).getBoolean("restoreRequired", false))
                getSharedPreferences("status", 0).edit().putBoolean("restoreRequired", true).apply();
            String shizuku = SaveBridge.readiness();
            String access = shizuku;
            boolean needsRestore = worlds.needsRestore();
            if (access == null) {
                if (!busy) refreshProfiles();
                if (profileError != null) access = "Не удалось подтвердить доступ Shizuku: " + profileError;
                else if (profileState == null || profileCheckedAt == 0) access = "Проверяем доступ к службе Shizuku…";
            } else {
                profileState = null;
                profileError = null;
                profileCheckedAt = 0;
            }
            boolean gameInAnotherProfile = info == null && engine.retained() != null;
            recoverGame.setVisibility(gameInAnotherProfile ? View.VISIBLE : View.GONE);
            recoverGame.setEnabled(!busy && pending < 0 && access == null);
            updateText(savesNote, (access == null ? "Shizuku: доступ предоставлен." : access) + "\n" + (saved == null ? "Проверенной копии миров пока нет." : "Копия: " + saved.getInt("files") + " файлов, " + (saved.getLong("bytes") / 1024) + " КиБ. " + (needsRestore ? "После замены игры нужно восстановление." : "Копия хранится отдельно от игры.")));
            boolean supported = info != null && PatchEngine.GAME_VERSION.equals(info.versionName);
            boolean experimental = !supported && PatchEngine.matchesVersion(info, plan) && plan.optBoolean("experimental");
            backup.setEnabled(!busy && pending < 0 && supported && access == null && !needsRestore);
            restore.setEnabled(!busy && pending < 0 && supported && saved != null && access == null);
            export.setEnabled(!busy && saved != null);
            importCopy.setEnabled(!busy && pending < 0);
            installImport.setEnabled(importCopy.isEnabled());
            installExport.setEnabled(export.isEnabled());
            boolean launchable = launchReady(info, needsRestore, busy) && pending < 0;
            Boolean modMatch = supported || experimental ? checkInstalledMod(engine, info, mod) : Boolean.FALSE;
            boolean currentMod = Boolean.TRUE.equals(modMatch);
            String blocker = engine.installBlocker(plan);
            install.setEnabled(!busy && plan != null && blocker == null && pending < 0);
            String replacementBlocker = planError != null ? planError : engine.removalBlocker(plan);
            boolean replacement = replacementBlocker == null;
            String removalBlocker = replacementBlocker;
            if (replacement && !supported) removalBlocker = "Для этой версии доступна экспериментальная замена без копии миров.";
            else if (busy) removalBlocker = "Дождитесь завершения текущей операции.";
            else if (pending >= 0) removalBlocker = "Подтвердите или отмените текущую установку Android.";
            else if (replacement && shizuku != null) removalBlocker = shizuku;
            else if (replacement && saved == null) removalBlocker = "Создайте проверенную копию миров в патчере.";
            // Clicking starts the preflight. It checks the current Android profiles
            // through Shizuku before granting permission to open the uninstall dialog.
            remove.setEnabled(removalBlocker == null);
            updateText(removalHint, removalBlocker == null
                ? "Проверим APK, профили Android и копию миров перед системным подтверждением."
                : removalBlocker);
            cancel.setVisibility(pending < 0 ? View.GONE : View.VISIBLE);
            cancel.setEnabled(!busy);
            launchHost.setEnabled(launchable && currentMod);
            launchGuest.setEnabled(launchable && currentMod);
            installRoles.setVisibility(launchable && currentMod ? View.VISIBLE : View.GONE);
            updateStatusChip(busy, needsRestore, info, currentMod);
            homeInstall.setVisibility(launchable && currentMod ? View.GONE : View.VISIBLE);
            boolean requiresShizuku = (info != null && needsRestore && saved != null)
                || (replacement && supported) || (gameInAnotherProfile && !install.isEnabled());
            installWithoutBackup.setVisibility(!busy && pending < 0 && replacement && supported && !currentMod
                ? View.VISIBLE : View.GONE);
            installImport.setVisibility(needsRestore && saved != null ? View.VISIBLE : View.GONE);
            installExport.setVisibility(saved != null && (replacement || needsRestore) ? View.VISIBLE : View.GONE);
            installShizukuHelp.setVisibility(!busy && pending < 0 && requiresShizuku && access != null
                ? View.VISIBLE : View.GONE);
            SharedPreferences statusPrefs = getSharedPreferences("status", 0);
            String operation = busy ? getSharedPreferences("diagnostics", 0).getString("action", "") : "";
            boolean preparing = "prepare".equals(operation) || "prepare-experimental".equals(operation);
            boolean installing = pending >= 0 || needsRestore || (plan != null && (info == null || supported || experimental))
                || "install".equals(operation) || "recover-assets".equals(operation);
            int phase = launchable && currentMod ? 2 : installing && !preparing ? 1 : 0;
            showInstallPhase(phase);
            showInstallProgress(busy, phase == 2, statusPrefs);
            boolean installPermission = getPackageManager().canRequestPackageInstalls();
            if (busy) {
                String heading = preparing ? "Подготовка мода"
                    : "install".equals(operation) ? "Установка / обновление мода" : "Идёт операция";
                showNextInstall(heading, statusPrefs.getString("message", "Выполняется операция…"), null, null);
            } else if (pending >= 0) {
                showNextInstall("Ожидаем Android", "Подтвердите установку в окне Android. Если окно закрылось, отмените запрос и повторите установку.",
                    null, null);
            } else if (info != null && !supported && !experimental) {
                showNextInstall("Версия " + info.versionName + " не поддерживается", "Мод проверен на " + PatchEngine.GAME_VERSION
                    + ". Для этой версии можно запустить экспериментальную проверку.", "Проверить совместимость", () -> confirmExperimental(info));
            } else if (!installPermission && (missingAssets || (install.isEnabled() && Boolean.FALSE.equals(modMatch) && !(info != null && needsRestore)))) {
                showNextInstall("Разрешите установку мода", "Разрешите этому приложению устанавливать APK в настройках Android. Затем вернитесь сюда и продолжите.",
                    "Разрешить установку", this::openInstallPermission);
            } else if (missingAssets) {
                showNextInstall("Нужны файлы игры", "В игре отсутствуют ресурсы. Выберите split_base_assets.apk от установленной версии игры.",
                    "Восстановить ресурсы", () -> recoverAssets.performClick());
            } else if (info != null && needsRestore) {
                if (saved == null) {
                    showNextInstall("Нужна копия миров", "Откройте ZIP с копией миров, чтобы вернуть сохранения в игру.",
                        "Открыть ZIP-копию миров", () -> importCopy.performClick());
                } else if (access != null) {
                    showNextInstall("Дайте доступ к сохранениям", access, "Подключить Shizuku", this::connectShizuku);
                } else {
                    showNextInstall("Верните свои миры", "Игра установлена. Сначала верните сохранения из проверенной копии.",
                        "Восстановить миры", () -> restore.performClick());
                }
            } else if (modMatch == null) {
                showNextInstall("Проверяем мод…", "Сверяем установленную игру с этой сборкой.", null, null);
            } else if (launchable && currentMod) {
                String connection = "Подключите оба телефона к одной сети Wi-Fi. На телефоне с вашим миром нажмите «Создать игру». На втором — «Присоединиться».";
                showNextInstall(experimental ? "Экспериментальная сборка" : "Всё готово для игры вдвоём",
                    experimental ? "Работа мода на этой версии не гарантируется. " + connection : connection, null, null);
            } else if (install.isEnabled()) {
                String message = info == null ? "Файлы готовы. Подтвердите установку в окне Android."
                    : "Файлы готовы. Android обновит игру поверх установленной и сохранит миры. Shizuku не нужен.";
                showNextInstall("Установите мод на этот телефон", message,
                    "Установить подготовленный мод", () -> install.performClick());
            } else if (gameInAnotherProfile) {
                if (access != null) {
                    showNextInstall("Нужен доступ к другому профилю", "Игра осталась в другом профиле Android. Подключите Shizuku, чтобы вернуть её в текущий профиль.",
                        "Подключить Shizuku", this::connectShizuku);
                } else {
                    showNextInstall("Верните игру в этот профиль", "Игра осталась в другом профиле Android. Верните её сюда, чтобы продолжить установку.",
                        "Вернуть игру в текущий профиль", () -> recoverGame.performClick());
                }
            } else if (info == null) {
                showNextInstall("Установите Subnautica", plan != null ? blocker : "Сначала установите Subnautica " + PatchEngine.GAME_VERSION + " в этом профиле Android.", null, null);
            } else if (replacement && experimental) {
                showNextInstall("Экспериментальная замена игры", "APK подготовлены. При замене Android удалит миры и прогресс. "
                    + "Патчер этой версии не умеет сохранять эти миры.", "Без Shizuku · удалить миры", this::confirmReplaceWithoutBackup);
            } else if (replacement && access != null) {
                showNextInstall("Дайте доступ к сохранениям", "Для замены игры нужен доступ к мирам. " + access,
                    "Подключить Shizuku", this::connectShizuku);
            } else if (replacement && saved == null) {
                showNextInstall("Сохраните копию миров", "Сохранитесь в игре, выйдите в главное меню и сделайте копию миров.",
                    "Создать копию миров", backup.isEnabled() ? this::confirmBackup : null);
            } else if (replacement) {
                showNextInstall("Замените игру", removalBlocker == null ? "Файлы готовы. Установщик проверит актуальность копии миров перед заменой игры." : removalBlocker,
                    "Проверить копию и заменить игру", remove.isEnabled() ? this::confirmReplace : null);
            } else if (plan == null) {
                showNextInstall("Подготовьте игру на этом телефоне", "Откройте этот патчер на обоих телефонах. Если сейчас играете, сохраните мир и выйдите в главное меню. Затем нажмите «Подготовить игру».",
                    "Подготовить игру", this::confirmPrepare);
            } else {
                showNextInstall("Обновите подготовленные файлы", blocker, "Подготовить файлы заново", this::confirmPrepare);
            }
            if (!busy && removalToken != null && removalToken.equals(getSharedPreferences("status", 0).getString("removalToken", ""))) {
                removalToken = null;
                long age = SystemClock.elapsedRealtime() - statusPrefs.getLong("removalReadyAt", 0);
                String identity = statusPrefs.getString("removalIdentity", "");
                boolean discard = statusPrefs.getBoolean("removalDiscardsWorlds", false);
                statusPrefs.edit().remove("removalToken").apply();
                if (replacement && pending < 0 && age >= 0 && age < 10000
                        && (discard ? installPermission : SaveBridge.readiness() == null)) {
                    try {
                        if (!identity.equals(engine.removalIdentity()))
                            throw new IOException("Игра изменилась во время проверки");
                        SharedPreferences.Editor replacementPrefs = statusPrefs.edit();
                        if (discard) replacementPrefs.putString(SaveManager.SKIP_RESTORE_TARGET, PatchEngine.targetIdentity(plan));
                        else replacementPrefs.remove(SaveManager.SKIP_RESTORE_TARGET);
                        if (!replacementPrefs.commit()) throw new IOException("Не удалось сохранить режим установки");
                        startActivity(new Intent(Intent.ACTION_DELETE, Uri.parse("package:" + PatchEngine.GAME)));
                        Diagnostics.event(this, "request_game_removal", discard ? "confirmed replacement without a new backup"
                            : "verified current-profile identity and backup");
                    } catch (Exception error) { showError("check-removal", error); }
                } else PatchService.status(this, "Проверка удаления устарела или условия изменились. Игра не удалена. Повторите проверку.");
            }
            String lastMessage = getSharedPreferences("status", 0).getString("message", "");
            updateText(status, lastMessage.isEmpty() ? "Операций пока нет" : lastMessage);
            JSONObject lastError = Diagnostics.lastError(this);
            boolean showError = !busy && lastError.has("code") && lastMessage.startsWith(lastError.optString("code") + " · ");
            installError.setVisibility(showError ? View.VISIBLE : View.GONE);
            if (showError) updateText(installError, lastError.optString("title") + " · Что делать?");
            quickBackup.setEnabled(backup.isEnabled());
            quickDiagnose.setEnabled(!reportBusy);
            quickJournal.setEnabled(errorDetails.isEnabled());
        } catch (Exception e) {
            showInstallPhase(0);
            showInstallProgress(false, false, getSharedPreferences("status", 0));
            backup.setEnabled(false); restore.setEnabled(false); export.setEnabled(false);
            remove.setEnabled(false); install.setEnabled(false);
            launchHost.setEnabled(false); launchGuest.setEnabled(false);
            recoverGame.setEnabled(false);
            installRoles.setVisibility(View.GONE);
            installShizukuHelp.setVisibility(View.GONE);
            installWithoutBackup.setVisibility(View.GONE);
            installImport.setEnabled(false);
            installExport.setEnabled(false);
            cancel.setVisibility(View.GONE);
            installError.setVisibility(View.GONE);
            showNextInstall("Не удалось проверить игру", e.getMessage(), "Проверить снова", this::render);
            updateText(removalHint, e.getMessage());
            updateText(status, getSharedPreferences("status", 0).getString("message", "Нужна повторная подготовка."));
        }
    }

    void updateInfoRow(LinearLayout row, CharSequence value) {
        TextView view = (TextView) row.getChildAt(1);
        if (!android.text.TextUtils.equals(view.getText(), value)) view.setText(value);
    }

    boolean launchReady(PackageInfo info, boolean needsRestore, boolean busy) throws IOException {
        return !busy && !needsRestore && PatchEngine.hasAssets(info);
    }

    Boolean checkInstalledMod(PatchEngine engine, PackageInfo info, JSONObject payload) {
        String key = info.applicationInfo.sourceDir + "@" + info.lastUpdateTime;
        if (!key.equals(modCheckKey)) {
            modCheckKey = key;
            installedModMatches = null;
            String expectedHash = payload.optString("sha256");
            String nativeDir = info.applicationInfo.nativeLibraryDir;
            new Thread(() -> {
                boolean matches = false;
                try {
                    if (nativeDir != null) {
                        File installed = new File(nativeDir, "libmain.so");
                        matches = installed.canRead() && expectedHash.equals(ApkPatcher.sha256(installed));
                    }
                    if (!matches) matches = Diagnostics.installedMod(getApplicationContext(), engine)
                        .optBoolean("matchesInstallerPayload");
                } catch (Exception ignored) { }
                final boolean result = matches;
                handler.post(() -> {
                    if (key.equals(modCheckKey)) installedModMatches = result;
                });
            }, "snmp-check-mod").start();
        }
        return installedModMatches;
    }

    void showNextInstall(String stage, String message, String label, Runnable action) {
        updateText(installStage, stage);
        updateText(note, message);
        nextInstallAction = action;
        nextInstall.setVisibility(action == null ? View.GONE : View.VISIBLE);
        if (action != null) updateText(nextInstall, label);
    }

    void updateStatusChip(boolean busy, boolean needsRestore, PackageInfo info, boolean currentMod) throws IOException {
        String label; int color;
        if (info == null) { label = "Игра не найдена"; color = Ui.RED; }
        else if (!PatchEngine.GAME_VERSION.equals(info.versionName)) {
            label = currentMod ? "Экспериментальная сборка" : "Версия игры не поддерживается"; color = Ui.AMBER;
        }
        else if (busy) { label = "Выполняется операция"; color = Ui.AMBER; }
        else if (needsRestore) { label = "Требуется восстановление миров"; color = Ui.AMBER; }
        else if (!PatchEngine.hasAssets(info)) { label = "Не найдены ресурсы игры"; color = Ui.AMBER; }
        else if (currentMod) { label = "Мод установлен"; color = Ui.GREEN; }
        else { label = "Игра найдена · нужна установка мода"; color = Ui.CYAN; }
        homeChip.setText(label);
        homeChip.setTextColor(color);
        homeChip.setBackground(Ui.round(this, Color.argb(60, Color.red(color), Color.green(color), Color.blue(color)), 20, 0, 0));
    }

    void refreshProfiles() {
        if (profilePending || SystemClock.elapsedRealtime() - profileCheckedAt < 10000 && profileCheckedAt != 0) return;
        profilePending = true;
        new Thread(() -> {
            JSONObject state = null; String error = null;
            try (SaveBridge bridge = new SaveBridge(this)) { state = bridge.call(SaveAccessService.PROFILE_STATE, null, false); }
            catch (Exception failure) { error = failure.getMessage() == null ? failure.getClass().getSimpleName() : failure.getMessage(); }
            final JSONObject checked = state; final String problem = error;
            handler.post(() -> { profileState = checked; profileError = problem; profileCheckedAt = SystemClock.elapsedRealtime(); profilePending = false; });
        }, "snmp-profiles").start();
    }

    int dp(int value) { return Ui.dp(this, value); }
    TextView text(LinearLayout body, String value, int size, int color) { return Ui.text(this, value, size, color); }
    LinearLayout gap(int height) { return Ui.gap(this, height); }
}
