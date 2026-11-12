package io.github.zerk235.snmp.installer;

import android.app.*;
import android.content.*;
import android.content.pm.PackageInfo;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.*;
import android.provider.Settings;
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
    TextView[] stepTitles, stepSubtitles, stepChevrons;
    TextView[] stepBadges = new TextView[5];
    LinearLayout stepRows[] = new LinearLayout[5];
    LinearLayout progressHost;
    TextView note, installHint;
    Button install, cancel;
    // Worlds screen views.
    TextView savesNote;
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

    JSONObject profileState;
    String profileError;
    boolean profilePending;
    long profileCheckedAt;
    String removalToken;
    final Shizuku.OnRequestPermissionResultListener permissionResult=(request,result)->render();
    final Handler handler = new Handler(Looper.getMainLooper());
    final Runnable refresh = new Runnable() { public void run() { render(); handler.postDelayed(this, 1000); } };

    public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(Ui.BG);
        getWindow().setNavigationBarColor(Ui.BG);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Ui.BG);
        FrameLayout content = new FrameLayout(this);
        content.setBackgroundColor(Ui.BG);
        for (int i = 0; i < 4; i++) {
            ScrollView scroll = new ScrollView(this);
            scroll.setFillViewport(true);
            scroll.setBackgroundColor(Ui.BG);
            bodies[i] = screen();
            scroll.addView(bodies[i]);
            screens[i] = scroll;
            content.addView(scroll, new FrameLayout.LayoutParams(-1, -1));
        }
        root.addView(content, new LinearLayout.LayoutParams(-1, 0, 1f));
        root.addView(bottomNav());
        setContentView(root);
        buildHome(bodies[0]);
        buildInstall(bodies[1]);
        buildWorlds(bodies[2]);
        buildSettings(bodies[3]);
        selectTab(0);
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
        nav.setBackground(Ui.round(this, Color.argb(255, 12, 28, 40), 0, Ui.CARD_STROKE, 0));
        GradientDrawable top = Ui.round(this, Ui.CARD_STROKE, 0, 0, 0);
        top.setShape(GradientDrawable.RECTANGLE);
        top.setCornerRadii(new float[]{0, 0, 0, 0, dp(18), 0, 0, dp(18)});
        nav.setBackground(top);
        nav.setPadding(0, dp(8), 0, dp(6));
        String[] icons = {"⌂", "↧", "▦", "⚙"};
        String[] labels = {"Главная", "Установка", "Миры", "Настройки"};
        tabIcons = new TextView[4]; tabLabels = new TextView[4];
        for (int i = 0; i < 4; i++) {
            LinearLayout tab = Ui.tab(this, icons[i], labels[i]);
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
        for (int i = 0; i < 4; i++) {
            screens[i].setVisibility(i == index ? View.VISIBLE : View.GONE);
            int color = i == index ? Ui.CYAN : Ui.MUTED;
            tabIcons[i].setTextColor(color);
            tabLabels[i].setTextColor(color);
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
        launchHost = Ui.button(this, " ◉  Создать игру (Хост)", 0);
        launchHost.setOnClickListener(v -> new AlertDialog.Builder(this).setTitle("Этот телефон — хост")
            .setMessage("Сохрани текущую игру и выйди в главное меню. После запуска загрузи здесь мир, которым хочешь поделиться. Можно выбрать и копию, полученную от другого хоста.\n\nНа втором телефоне выбери «Присоединиться (клиент)» и оставь игру в главном меню. Роль сохраняется для следующих запусков.")
            .setNegativeButton("Отмена", null).setPositiveButton("Запустить хостом", (d, w) -> launchGame("host")).show());
        coop.addView(launchHost);
        coop.addView(gap(8));
        launchGuest = Ui.button(this, " ◉  Присоединиться (Клиент)", 1);
        launchGuest.setOnClickListener(v -> new AlertDialog.Builder(this).setTitle("Этот телефон — гость")
            .setMessage("Сохрани текущую игру и выйди в главное меню. У хоста уже должен быть загружен мир. На этом телефоне оставь главное меню — мод сам получит мир и подключится.\n\nЕсли локальный мир с таким именем отличается, его папка будет сохранена отдельно перед получением копии хоста. Роль сохраняется для следующих запусков.")
            .setNegativeButton("Отмена", null).setPositiveButton("Подключиться гостем", (d, w) -> launchGame("guest")).show());
        coop.addView(launchGuest);
        body.addView(coop);
        body.addView(gap(12));

        // Connection state card.
        LinearLayout conn = Ui.card(this);
        conn.addView(Ui.text(this, "Состояние подключения", 15, Ui.MUTED));
        conn.addView(gap(4));
        status = Ui.text(this, "Не подключено", 14, Ui.TEXT);
        conn.addView(status);
        body.addView(conn);
        body.addView(gap(12));

        // Quick actions.
        LinearLayout quick = Ui.card(this);
        quick.addView(Ui.text(this, "Быстрые действия", 15, Ui.MUTED));
        quick.addView(gap(10));
        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        quickBackup = Ui.button(this, "⟳\nСинхр. миры", 1);
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

    void buildInstall(LinearLayout body) {
        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        TextView icon = Ui.text(this, "↧", 22, Ui.CYAN);
        icon.setPadding(0, 0, dp(10), 0);
        head.addView(icon);
        TextView title = Ui.header(this, "Установка мода");
        head.addView(title);
        body.addView(head);
        body.addView(gap(4));
        body.addView(Ui.caption(this, "Пошаговая подготовка игры\nи установка модификации."));
        body.addView(gap(14));

        String[] titles = {"Подключить Shizuku", "Создать резервную копию", "Подготовить игру", "Заменить игру", "Проверка"};
        String[] subs = {"Служба для доступа к файлам системы", "Сохранение оригинального APK и миров", "Патч APK и добавление мода", "Установка модифицированного APK", "Убедитесь, что мод работает"};
        stepTitles = new TextView[5]; stepSubtitles = new TextView[5]; stepChevrons = new TextView[5];
        for (int i = 0; i < 5; i++) {
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setBackground(Ui.round(this, Ui.CARD, 16, Ui.CARD_STROKE, 1));
            int pad = dp(12); row.setPadding(pad, pad, pad, pad);
            LinearLayout textCol = new LinearLayout(this);
            textCol.setOrientation(LinearLayout.VERTICAL);
            stepTitles[i] = Ui.text(this, titles[i], 15, Ui.TEXT);
            stepTitles[i].setTypeface(Typeface.DEFAULT_BOLD);
            stepSubtitles[i] = Ui.caption(this, subs[i]);
            textCol.addView(stepTitles[i]);
            textCol.addView(stepSubtitles[i]);
            final int step = i;
            row.setOnClickListener(v -> tapStep(step));
            row.addView(stepBadges[i] = Ui.stepBadge(this, i + 1, 0), new LinearLayout.LayoutParams(dp(34), dp(34)));
            TextView spacer = new TextView(this); spacer.setPadding(dp(12), 0, 0, 0);
            row.addView(textCol, new LinearLayout.LayoutParams(0, -2, 1f));
            stepChevrons[i] = Ui.text(this, "›", 18, Ui.MUTED);
            row.addView(stepChevrons[i]);
            stepRows[i] = row;
            body.addView(row);
            body.addView(gap(8));
        }

        LinearLayout progressCard = Ui.card(this);
        progressCard.addView(Ui.text(this, "Прогресс установки", 15, Ui.MUTED));
        progressCard.addView(gap(10));
        progressHost = new LinearLayout(this);
        progressCard.addView(progressHost);
        body.addView(gap(4));
        body.addView(progressCard);
        body.addView(gap(12));

        LinearLayout hint = Ui.card(this);
        hint.addView(Ui.header(this, "Подсказка"));
        hint.addView(gap(6));
        installHint = Ui.text(this, "Убедись, что у тебя установлены:\n• Shizuku (v13+)\n• Оригинальная игра v1.22.83315\n• Достаточно свободного места (≈ 2 ГБ)", 13, Ui.MUTED);
        hint.addView(installHint);
        body.addView(hint);
        body.addView(gap(12));

        note = Ui.text(this, "", 14, Ui.MUTED);
        body.addView(note);
        body.addView(gap(8));
        install = Ui.button(this, "Установить подготовленный мод", 0);
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
                    .setNegativeButton("Отмена", null).setPositiveButton("Установить", (d, w) -> start("install")).show();
            } catch (Exception e) { showError("install-check", e); }
        });
        body.addView(install);
        body.addView(gap(8));
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
        body.addView(cancel);
    }

    void tapStep(int step) {
        switch (step) {
            case 0: connectShizuku(); break;
            case 1: confirmBackup(); break;
            case 2: confirmPrepare(); break;
            case 3: confirmReplace(); break;
            case 4: launchGame("auto"); break;
        }
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

        backup = Ui.button(this, "  ⟳  Синхронизировать миры\n      Передать миры с хоста к клиенту", 1);
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

        importCopy = Ui.button(this, "  ⤒  Восстановить из ZIP\n      Импортировать сохранение", 1);
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
            else alert("Доступ Shizuku предоставлен. Сохранитесь, выйдите в главное меню игры и создайте копию миров.");
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
            .setMessage("Сначала сохраните игру и выйдите в главное меню. Создадим отдельную копию APK с модом. При смене подписи и предоставленном доступе Shizuku также создадим проверенную копию миров. Если Shizuku недоступен, APK подготовятся, но перед удалением игры копию миров нужно создать отдельно.\n\nНе удаляйте установщик и не очищайте его данные: здесь хранятся копии и ключ обновлений.")
            .setNegativeButton("Отмена", null).setPositiveButton("Подготовить", (d, w) -> start("prepare")).show();
    }

    void confirmReplace() {
        new AlertDialog.Builder(this).setTitle("Проверить копию перед заменой?")
            .setMessage("Сохранитесь и выйдите в главное меню заранее: для финальной проверки игра будет остановлена. Проверим APK, SHA256 копии и совпадение текущих миров с копией. Затем Android отдельно попросит подтвердить удаление Subnautica. После удаления установите подготовленный мод и восстановите миры. До конца замены игру не открывайте. Сам установщик не удаляйте.")
            .setNegativeButton("Отмена", null).setPositiveButton("Проверить", (d, w) -> {
                removalToken = UUID.randomUUID().toString();
                startForegroundService(new Intent(this, PatchService.class).setAction("check-removal").putExtra("token", removalToken));
            }).show();
    }

    void start(String action) {
        try {
            startForegroundService(new Intent(this, PatchService.class).setAction(action));
            updateText(status, "Запуск…");
        } catch (Exception error) { showError(action, error); }
    }

    void launchGame(String role) {
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

    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
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
            boolean busy = PatchService.BUSY.get();
            JSONObject plan = null; String planError = null;
            try { plan = engine.plan(); }
            catch (Exception invalidPlan) { planError = invalidPlan.getMessage(); }
            int pending = PatchService.pendingSession(this);
            SaveManager worlds = new SaveManager(this); JSONObject saved = worlds.latest();
            if (info == null && saved != null && !getSharedPreferences("status", 0).getBoolean("restoreRequired", false))
                getSharedPreferences("status", 0).edit().putBoolean("restoreRequired", true).apply();
            String access = SaveBridge.readiness(); boolean needsRestore = worlds.needsRestore();
            recoverGame.setVisibility(info == null && engine.retained() != null ? View.VISIBLE : View.GONE);
            recoverGame.setEnabled(!busy && pending < 0 && access == null);
            updateText(savesNote, (access == null ? "Shizuku: доступ предоставлен." : access) + "\n" + (saved == null ? "Проверенной копии миров пока нет." : "Копия: " + saved.getInt("files") + " файлов, " + (saved.getLong("bytes") / 1024) + " КиБ. " + (needsRestore ? "После замены игры нужно восстановление." : "Копия хранится отдельно от игры.")));
            backup.setEnabled(!busy && pending < 0 && info != null && access == null && !needsRestore);
            restore.setEnabled(!busy && pending < 0 && info != null && saved != null && access == null);
            export.setEnabled(!busy && saved != null);
            importCopy.setEnabled(!busy && pending < 0);
            setEnabledStep(0, !busy && access != null);
            setEnabledStep(1, !busy && pending < 0 && info != null && access == null && !needsRestore);
            setEnabledStep(2, !busy && info != null && pending < 0);
            setEnabledStep(4, !busy && info != null && !needsRestore);
            install.setEnabled(!busy && plan != null && engine.installBlocker(plan) == null && pending < 0);
            String blocker = engine.installBlocker(plan);
            boolean replacement = plan != null && engine.removalBlocker(plan) == null;
            if (replacement && access == null && !busy) refreshProfiles();
            boolean onlyCurrent = profileState != null && profileState.optBoolean("currentInstalled")
                && profileState.getJSONArray("installedUsers").length() == 1 && profileError == null
                && SystemClock.elapsedRealtime() - profileCheckedAt < 15000;
            remove.setEnabled(!busy && pending < 0 && replacement && onlyCurrent && saved != null && access == null);
            cancel.setVisibility(pending < 0 ? View.GONE : View.VISIBLE);
            cancel.setEnabled(!busy);
            String noteValue = plan == null ? (planError == null ? "После подготовки здесь станет доступна установка." : planError) : (blocker == null
                ? "Файлы проверены. Android запросит разрешение и подтверждение установки."
                : blocker);
            if (replacement && access == null && !onlyCurrent) noteValue = profileError != null ? profileError
                : profileState == null ? "Проверка профилей Android перед заменой подписи…"
                : "Игра установлена также в другом профиле Android или клоне. Удаление заблокировано: новая подпись не заменит этот пакет. Клон и его миры сохраняются.";
            setEnabledStep(3, remove.isEnabled() || install.isEnabled());
            launchHost.setEnabled(launchReady(info, needsRestore, busy));
            launchGuest.setEnabled(launchReady(info, needsRestore, busy));
            if (needsRestore) noteValue = "Мод установлен. Восстановите миры из проверенной копии перед запуском игры.";
            updateText(note, noteValue);
            updateStatusChip(busy, needsRestore, info == null);
            updateSteps(access, saved, plan, needsRestore, launchReady(info, needsRestore, busy));
            progressHost.removeAllViews();
            progressHost.addView(Ui.progressStrip(this, stepStates));
            if (!busy && removalToken != null && removalToken.equals(getSharedPreferences("status", 0).getString("removalToken", ""))) {
                removalToken = null;
                long age = SystemClock.elapsedRealtime() - getSharedPreferences("status", 0).getLong("removalReadyAt", 0);
                String identity = getSharedPreferences("status", 0).getString("removalIdentity", "");
                getSharedPreferences("status", 0).edit().remove("removalToken").apply();
                if (!busy && replacement && onlyCurrent && age >= 0 && age < 10000 && identity.equals(engine.removalIdentity())) {
                    Diagnostics.event(this, "request_game_removal", "verified current-profile identity and fresh one-use grant");
                    startActivity(new Intent(Intent.ACTION_DELETE, Uri.parse("package:" + PatchEngine.GAME)));
                } else PatchService.status(this, "Проверка удаления устарела или установка изменилась. Игра не удалена; повторите проверку.");
            }
            updateText(status, getSharedPreferences("status", 0).getString("message", "Не подключено"));
            quickBackup.setEnabled(backup.isEnabled());
            quickDiagnose.setEnabled(!reportBusy);
            quickJournal.setEnabled(errorDetails.isEnabled());
            recoverGame.setVisibility(info == null && engine.retained() != null ? View.VISIBLE : View.GONE);
        } catch (Exception e) {
            backup.setEnabled(false); restore.setEnabled(false); export.setEnabled(false);
            remove.setEnabled(false); install.setEnabled(false);
            launchHost.setEnabled(false); launchGuest.setEnabled(false);
            recoverGame.setEnabled(false);
            updateText(note, e.getMessage());
            updateText(status, getSharedPreferences("status", 0).getString("message", "Нужна повторная подготовка."));
        }
    }

    void updateInfoRow(LinearLayout row, CharSequence value) {
        TextView view = (TextView) row.getChildAt(1);
        if (!android.text.TextUtils.equals(view.getText(), value)) view.setText(value);
    }

    boolean launchReady(PackageInfo info, boolean needsRestore, boolean busy) { return !busy && info != null && !needsRestore; }

    void setEnabledStep(int index, boolean enabled) {
        stepRows[index].setEnabled(enabled);
        stepChevrons[index].setEnabled(enabled);
        stepRows[index].setAlpha(enabled ? 1f : 0.55f);
    }

    int[] stepStates = new int[5];

    void updateSteps(String access, JSONObject saved, JSONObject plan, boolean needsRestore, boolean ready) {
        stepStates[0] = access == null ? 2 : 1;
        stepStates[1] = saved != null ? 2 : stepStates[0] == 2 ? 1 : 0;
        stepStates[2] = plan != null ? 2 : stepStates[1] == 2 ? 1 : 0;
        stepStates[3] = needsRestore ? 2 : stepStates[2] == 2 ? 1 : 0;
        stepStates[4] = ready && !needsRestore ? 2 : needsRestore ? 1 : 0;
        for (int i = 0; i < 5; i++) {
            TextView badge = stepBadges[i];
            GradientDrawable bg = new GradientDrawable();
            bg.setShape(GradientDrawable.OVAL);
            if (stepStates[i] == 2) { bg.setColor(Ui.GREEN); badge.setText("✓"); badge.setTextColor(Color.rgb(4, 26, 20)); }
            else if (stepStates[i] == 1) { bg.setColor(Ui.CYAN); badge.setText(String.valueOf(i + 1)); badge.setTextColor(Color.rgb(4, 26, 20)); }
            else { bg.setColor(Ui.CARD); bg.setStroke(dp(1), Ui.CARD_STROKE); badge.setText(String.valueOf(i + 1)); badge.setTextColor(Ui.MUTED); }
            badge.setBackground(bg);
        }
    }

    void updateStatusChip(boolean busy, boolean needsRestore, boolean missing) {
        String label; int color;
        if (missing) { label = "Игра не найдена"; color = Ui.RED; }
        else if (busy) { label = "Выполняется операция"; color = Ui.AMBER; }
        else if (needsRestore) { label = "Требуется восстановление миров"; color = Ui.AMBER; }
        else { label = "Установлен и готов к игре"; color = Ui.GREEN; }
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
            catch (Exception failure) { error = failure.getMessage(); }
            final JSONObject checked = state; final String problem = error;
            handler.post(() -> { profileState = checked; profileError = problem; profileCheckedAt = SystemClock.elapsedRealtime(); profilePending = false; });
        }, "snmp-profiles").start();
    }

    int dp(int value) { return Ui.dp(this, value); }
    TextView text(LinearLayout body, String value, int size, int color) { return Ui.text(this, value, size, color); }
    LinearLayout gap(int height) { return Ui.gap(this, height); }
}
