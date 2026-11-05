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
import rikka.shizuku.Shizuku;
import java.io.*;
import java.util.UUID;

public final class MainActivity extends Activity {
    TextView game, status, note, savesNote, diagnosticsNote;
    Button prepare, install, launch, cancel, backup, restore, remove, export, importCopy;
    Button launchHost,launchGuest;
    Button diagnose,shareReport,saveReport,errorDetails;
    String removalToken;
    final Shizuku.OnRequestPermissionResultListener permissionResult=(request,result)->render();
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
        text(body, "Установка мода · 0.3.1-preview", 14, Color.LTGRAY);
        game = text(body, "", 16, Color.WHITE);
        text(body,"Совместная игра",20,Color.rgb(79,216,206));
        text(body,"Выбери роль этого телефона. Для игры нужна одна Wi-Fi сеть и одинаковая сборка мода на обоих устройствах.",16,Color.LTGRAY);
        launchHost=button(body,"Запустить хостом");
        launchHost.setOnClickListener(v->new AlertDialog.Builder(this).setTitle("Этот телефон — хост")
            .setMessage("Сохрани текущую игру и выйди в главное меню. После запуска загрузи здесь мир, которым хочешь поделиться. Можно выбрать и копию, полученную от другого хоста.\n\nНа втором телефоне выбери «Подключиться гостем» и оставь игру в главном меню. Роль сохраняется для следующих запусков.")
            .setNegativeButton("Отмена",null).setPositiveButton("Запустить хостом",(d,w)->launchGame("host")).show());
        launchGuest=button(body,"Подключиться гостем");
        launchGuest.setOnClickListener(v->new AlertDialog.Builder(this).setTitle("Этот телефон — гость")
            .setMessage("Сохрани текущую игру и выйди в главное меню. У хоста уже должен быть загружен мир. На этом телефоне оставь главное меню — мод сам получит мир и подключится.\n\nЕсли локальный мир с таким именем отличается, его папка будет сохранена отдельно перед получением копии хоста. Роль сохраняется для следующих запусков.")
            .setNegativeButton("Отмена",null).setPositiveButton("Подключиться гостем",(d,w)->launchGame("guest")).show());
        text(body,"Диагностика и помощь",20,Color.rgb(79,216,206));
        text(body,"При ошибке создайте ZIP и отправьте разработчику. Если пропал игрок или сломалась синхронизация, соберите отчёт на ОБОИХ телефонах, пока баг виден, и укажите хоста. Игра продолжит работать. Для игровых логов нужен запущенный Shizuku с разрешением; без него будет частичный отчёт. В архив не входят миры, APK и ключи. Журналы игры могут содержать локальные IP, имена слотов и команды.",15,Color.LTGRAY);
        diagnose=button(body,"Создать диагностический ZIP");
        diagnose.setOnClickListener(v->diagnosticForm());
        errorDetails=button(body,"Последняя ошибка — подробности");
        errorDetails.setOnClickListener(v->showLastError());
        shareReport=button(body,"Отправить ZIP"); shareReport.setOnClickListener(v->shareDiagnostic());
        saveReport=button(body,"Сохранить диагностический ZIP"); saveReport.setOnClickListener(v->saveDiagnostic());
        diagnosticsNote=text(body,"",14,Color.LTGRAY);
        Shizuku.addRequestPermissionResultListener(permissionResult);
        text(body,"1  Сохранить миры",20,Color.rgb(79,216,206));
        text(body,"Перед заменой подписи сохраним все миры отдельно от игры и проверим каждый файл. Для доступа к папке игры нужен Shizuku. На Android 11+ он запускается через беспроводную отладку без компьютера и root.",16,Color.LTGRAY);
        Button connect=button(body,"Подключить Shizuku");
        connect.setOnClickListener(v->{
            try {
                if(!Shizuku.pingBinder()) {
                    Intent launch=getPackageManager().getLaunchIntentForPackage("moe.shizuku.privileged.api");
                    if(launch!=null) startActivity(launch);
                    else startActivity(new Intent(Intent.ACTION_VIEW,Uri.parse("https://shizuku.rikka.app/download/")));
                } else if(Shizuku.checkSelfPermission()!=android.content.pm.PackageManager.PERMISSION_GRANTED) Shizuku.requestPermission(701);
                else alert("Доступ Shizuku предоставлен. Сохранитесь, выйдите в главное меню игры и создайте копию миров.");
            } catch(Exception error) { showError("shizuku-connect",error); }
        });
        Button guide=button(body,"Как запустить Shizuku без компьютера");
        guide.setOnClickListener(v->startActivity(new Intent(Intent.ACTION_VIEW,Uri.parse("https://shizuku.rikka.app/guide/setup/"))));
        Button help=button(body,"Помощь с Shizuku");
        help.setOnClickListener(v->new AlertDialog.Builder(this).setTitle("Помощь с Shizuku")
            .setMessage("ЗАЧЕМ НУЖЕН SHIZUKU\nAndroid защищает папку миров другой игры. С вашим разрешением Shizuku позволяет установщику скопировать и восстановить SavedGames. Для подготовки APK он не нужен.\n\n"
                +"ЗАПУСК БЕЗ КОМПЬЮТЕРА — ANDROID 11+\n1. Установите официальный Shizuku.\n2. Включите возможности разработчика и отладку по Wi-Fi.\n3. В Shizuku выполните сопряжение с кодом из окна «Подключить устройство с помощью кода» и запустите службу.\n4. Вернитесь сюда → «Подключить Shizuku» → разрешите доступ для SN Multiplayer Installer.\nПосле перезагрузки телефона Shizuku может потребоваться запустить заново. На Android 10 беспроводной запуск без компьютера и root не поддерживается.\n\n"
                +"ОШИБКА «ДОСТУП К ADB ОГРАНИЧЕН»\nНа некоторых realme / OPPO / OnePlus доступ ограничивает прошивка. Для realme GT8 с V17.0.0 найден пункт Disable system optimization («Выключить оптимизацию системы»). При активном ограничении он скрывается на русском языке.\n\n"
                +"Что сделать на этой прошивке:\n1. Временно смените системный язык на English.\n2. Откройте Settings → System & updates → Developer options.\n3. Ближе к концу списка найдите Disable system optimization и включите его. Прочитайте системное предупреждение: возможен повышенный расход батареи в фоне.\n4. Перезапустите Shizuku.\n5. Здесь снова нажмите «Подключить Shizuku» и разрешите доступ.\n6. Русский язык можно вернуть.\n\n"
                +"На других прошивках пункт может называться «Отключить мониторинг разрешений»; если он называется «Мониторинг разрешений», его требуется выключить. Не меняйте похожие настройки наугад. «Запретить доступ к отладке с помощью adb» и ограничение дочерних процессов — другие параметры. Установщик сам настройки системы не переключает.\n\n"
                +"ЕСЛИ ДОСТУП НЕ ПОЯВИЛСЯ\nПроверьте, что Shizuku сообщает о работающей службе. Закройте и снова откройте установщик, затем нажмите подключение. Данные установщика не очищайте: там находятся ключ подписи и копии миров.\n\n"
                +"Нужный результат: «Shizuku: доступ предоставлен». Затем сохраните игру, выйдите в её главное меню и создайте копию миров. До сообщения об успешной проверке копии игру не удаляйте.")
            .setPositiveButton("Понятно",null).setNeutralButton("Официальная инструкция",(d,w)->startActivity(new Intent(Intent.ACTION_VIEW,Uri.parse("https://shizuku.rikka.app/guide/setup/")))).show());
        savesNote=text(body,"",15,Color.LTGRAY);
        backup=button(body,"Создать копию миров");
        backup.setOnClickListener(v->new AlertDialog.Builder(this).setTitle("Сохранить все миры?")
            .setMessage("Сначала сохраните игру, дождитесь окончания сохранения и выйдите в главное меню. Будет скопирована папка SavedGames со всеми мирами, включая ячейки мира. Приватные настройки игры не копируются. Игру не удаляем.")
            .setNegativeButton("Отмена",null).setPositiveButton("Создать копию",(d,w)->start("backup")).show());
        export=button(body,"Сохранить ZIP-копию в выбранную папку");
        export.setOnClickListener(v->{
            try { if(new SaveManager(this).latest()==null) { alert("Сначала создайте копию миров"); return; }
                startActivityForResult(new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("application/zip")
                    .addCategory(Intent.CATEGORY_OPENABLE).putExtra(Intent.EXTRA_TITLE,"Subnautica-worlds-"+System.currentTimeMillis()+".zip"),702);
            } catch(Exception error) { alert(error.getMessage()); }
        });
        importCopy=button(body,"Открыть ZIP-копию миров");
        importCopy.setOnClickListener(v->startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
            .setType("application/zip").addCategory(Intent.CATEGORY_OPENABLE),703));
        text(body, "2  Подготовить игру", 20, Color.rgb(79, 216, 206));
        text(body, "Нужна установленная собственная копия игры 1.22.83315. Установщик возьмёт её APK с телефона и добавит мод. Root и компьютер не нужны.", 16, Color.LTGRAY);
        prepare = button(body, "Подготовить мод");
        prepare.setOnClickListener(v -> new AlertDialog.Builder(this).setTitle("Подготовить файлы?")
            .setMessage("Сначала сохраните игру и выйдите в главное меню. Создадим отдельную копию APK с модом. При смене подписи и предоставленном доступе Shizuku также создадим проверенную копию миров. Если Shizuku недоступен, APK подготовятся, но перед удалением игры копию миров нужно создать отдельно.\n\nНе удаляйте установщик и не очищайте его данные: здесь хранятся копии и ключ обновлений.")
            .setNegativeButton("Отмена", null).setPositiveButton("Подготовить", (d, w) -> start("prepare")).show());
        text(body, "3  Заменить игру", 20, Color.rgb(79, 216, 206));
        note = text(body, "", 16, Color.LTGRAY);
        remove=button(body,"Удалить старую игру");
        remove.setOnClickListener(v->new AlertDialog.Builder(this).setTitle("Проверить копию перед заменой?")
            .setMessage("Сохранитесь и выйдите в главное меню заранее: для финальной проверки игра будет остановлена. Проверим APK, SHA256 копии и совпадение текущих миров с копией. Затем Android отдельно попросит подтвердить удаление Subnautica. После удаления установите подготовленный мод и восстановите миры. До конца замены игру не открывайте. Сам установщик не удаляйте.")
            .setNegativeButton("Отмена",null).setPositiveButton("Проверить",(d,w)->{
                removalToken=UUID.randomUUID().toString();
                startForegroundService(new Intent(this,PatchService.class).setAction("check-removal").putExtra("token",removalToken));
            }).show());
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
            } catch (Exception e) { showError("install-check",e); }
        });
        text(body,"4  Вернуть миры",20,Color.rgb(79,216,206));
        restore=button(body,"Восстановить миры");
        restore.setOnClickListener(v->new AlertDialog.Builder(this).setTitle("Восстановить проверенную копию?")
            .setMessage("Игра будет остановлена. Сначала запишем миры во временную папку, проверим каждый файл и только затем подключим их к игре. Если в игре уже есть миры, их папка останется рядом как резервная. Несохранённый прогресс запущенной игры потеряется — сохранитесь и выйдите в меню заранее.")
            .setNegativeButton("Отмена",null).setPositiveButton("Восстановить",(d,w)->start("restore")).show());
        launch = button(body, "Открыть игру автоматически");
        launch.setOnClickListener(v -> launchGame("auto"));
        cancel = button(body, "Отменить ожидающую установку Android");
        cancel.setOnClickListener(v -> {
            int id = PatchService.pendingSession(this);
            if (id >= 0) {
                try {
                    getPackageManager().getPackageInstaller().abandonSession(id);
                    getSharedPreferences("status", 0).edit().remove("session").apply();
                    PatchService.status(this, "Ожидающая установка отменена. Подготовленные файлы сохранены.");
                } catch (Exception e) { showError("cancel-install",e); }
            }
            render();
        });
        text(body, "Состояние", 20, Color.rgb(79, 216, 206));
        status = text(body, "", 15, Color.WHITE);
        Button report = button(body, "Скопировать отчёт");
        report.setOnClickListener(v -> {
            String value = game.getText() + "\n" + status.getText()+"\n\n"+DiagnosticError.readable(Diagnostics.lastError(this))
                +"\n\nДля полного отчёта нажмите «Создать диагностический ZIP».";
            ((android.content.ClipboardManager)getSystemService(CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText("SNMP", value));
            Toast.makeText(this, "Отчёт скопирован", Toast.LENGTH_SHORT).show();
        });
        text(body, "Первое подключение: хост сохраняет мир, гость остаётся в главном меню. Нужны одна сборка мода и одна Wi-Fi сеть.\n\nС разрешения Shizuku установщик копирует и восстанавливает SavedGames, а по кнопке диагностики читает журналы Subnautica и logcat её текущего процесса в этом профиле. Копии остаются на телефоне. Установщик не содержит игру и не отправляет данные в интернет.", 14, Color.LTGRAY);
        render();
    }

    void start(String action) {
        try {
            startForegroundService(new Intent(this, PatchService.class).setAction(action));
            updateText(status,"Запуск…");
        } catch(Exception error) { showError(action,error); }
    }
    void launchGame(String role) {
        Intent intent=getPackageManager().getLaunchIntentForPackage(PatchEngine.GAME);
        if(intent==null){showError("launch-"+role,new IOException("Игра пока не установлена"));return;}
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK|Intent.FLAG_ACTIVITY_SINGLE_TOP);
        intent.putExtra("snmp_launch_role",role);
        intent.putExtra("snmp_launch_request",UUID.randomUUID().toString());
        try { startActivity(intent); Diagnostics.event(this,"launch_game","requested role="+role); }
        catch(Exception error) { showError("launch-"+role,error); }
    }
    void showError(String action,Throwable error) {
        String message=Diagnostics.failure(this,action,error); PatchService.status(this,message);
        new AlertDialog.Builder(this).setTitle("Не удалось завершить действие").setMessage(message)
            .setPositiveButton("Понятно",null).setNeutralButton("Подробности",(d,w)->showLastError()).show();
    }
    void showLastError() {
        JSONObject error=Diagnostics.lastError(this);
        String message=error.has("code")?DiagnosticError.readable(error):"Ошибок установщика пока не записано. Это не исключает сбоя в игре — создайте диагностический ZIP.";
        TextView text=new TextView(this); text.setText(message); text.setTextColor(Color.WHITE); text.setPadding(dp(18),dp(12),dp(18),dp(12)); text.setTextIsSelectable(true);
        ScrollView scroll=new ScrollView(this); scroll.addView(text);
        new AlertDialog.Builder(this).setTitle("Последняя сохранённая ошибка").setView(scroll).setPositiveButton("Закрыть",null).show();
    }
    void diagnosticForm() {
        LinearLayout form=new LinearLayout(this); form.setOrientation(LinearLayout.VERTICAL); form.setPadding(dp(18),dp(8),dp(18),0);
        Spinner category=new Spinner(this); category.setAdapter(new ArrayAdapter<String>(this,android.R.layout.simple_spinner_dropdown_item,
            new String[]{"Установка и обновление","Подключение и мир","Игрок исчез или застыл","Строительство базы","Предметы, анимации и свет","Другое"})); form.addView(category);
        EditText description=new EditText(this); description.setTextColor(Color.WHITE); description.setHintTextColor(Color.LTGRAY);
        description.setHint("Что произошло и после какого действия? Кто хост? Примерное время ошибки? Что ожидали увидеть?");
        description.setMinLines(4); description.setMaxLines(7); description.setInputType(android.text.InputType.TYPE_CLASS_TEXT|android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        description.setFilters(new android.text.InputFilter[]{new android.text.InputFilter.LengthFilter(4000)}); form.addView(description);
        new AlertDialog.Builder(this).setTitle("Отчёт разработчику").setView(form)
            .setMessage("Опишите проблему. Журналы соберутся без остановки игры. Если Shizuku недоступен, это будет явно указано в частичном отчёте. Отправка только по вашему выбору.")
            .setNegativeButton("Отмена",null).setPositiveButton("Собрать ZIP",(d,w)->{
                try {
                    startForegroundService(new Intent(this,PatchService.class).setAction("diagnostics")
                        .putExtra("category",category.getSelectedItem().toString()).putExtra("description",description.getText().toString()));
                } catch(Exception error) { showError("diagnostics-start",error); }
            }).show();
    }
    void shareDiagnostic() {
        File report=Diagnostics.latest(this); if(report==null) { alert("Сначала создайте диагностический ZIP"); return; }
        Uri uri=ReportProvider.uri(this,report);
        Intent send=new Intent(Intent.ACTION_SEND).setType("application/zip").putExtra(Intent.EXTRA_STREAM,uri)
            .putExtra(Intent.EXTRA_SUBJECT,"SNMP · диагностический отчёт").putExtra(Intent.EXTRA_TEXT,"Отчёт SNMP. При проблеме синхронизации приложите ZIP со второго телефона и укажите хоста.")
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        send.setClipData(ClipData.newRawUri("SNMP diagnostic",uri));
        try { startActivity(Intent.createChooser(send,"Отправить диагностический ZIP")); }
        catch(Exception error) { showError("diagnostics-share",error); }
    }
    void saveDiagnostic() {
        File report=Diagnostics.latest(this); if(report==null) { alert("Сначала создайте диагностический ZIP"); return; }
        getSharedPreferences("diagnostics",0).edit().putString("exportReport",report.getName()).apply();
        try { startActivityForResult(new Intent(Intent.ACTION_CREATE_DOCUMENT).setType("application/zip").addCategory(Intent.CATEGORY_OPENABLE)
            .putExtra(Intent.EXTRA_TITLE,report.getName()),704); }
        catch(Exception error) { showError("diagnostics-save",error); }
    }
    void alert(String message) { new AlertDialog.Builder(this).setMessage(message).setPositiveButton("Понятно", null).show(); }
    void updateText(TextView view,CharSequence value) {
        if(!android.text.TextUtils.equals(view.getText(),value)) view.setText(value);
    }
    public void onResume() { super.onResume(); handler.post(refresh); }
    public void onPause() { handler.removeCallbacks(refresh); super.onPause(); }
    public void onDestroy() { Shizuku.removeRequestPermissionResultListener(permissionResult); super.onDestroy(); }
    protected void onActivityResult(int request,int result,Intent data) {
        super.onActivityResult(request,result,data);
        if((request!=702 && request!=703 && request!=704) || result!=RESULT_OK || data==null || data.getData()==null) return;
        if(!PatchService.BUSY.compareAndSet(false,true)) { alert("Дождитесь текущей операции"); return; }
        Uri uri=data.getData();
        final String action=request==704?"diagnostics-save":request==703?"backup-import":"backup-export";
        Diagnostics.begin(this,action);
        new Thread(()->{
            try {
                if(request==704) {
                    String name=getSharedPreferences("diagnostics",0).getString("exportReport","");
                    File report=ReportProvider.file(this,name);
                    try(InputStream in=new FileInputStream(report); OutputStream out=getContentResolver().openOutputStream(uri,"w")) {
                        if(out==null) throw new IOException("Папка не разрешила запись отчёта");
                        byte[] buffer=new byte[65536]; int n; while((n=in.read(buffer))!=-1) out.write(buffer,0,n);
                    }
                    String expected=ApkPatcher.sha256(report);
                    try(InputStream in=getContentResolver().openInputStream(uri)) {
                        java.security.MessageDigest digest=java.security.MessageDigest.getInstance("SHA-256"); byte[] buffer=new byte[65536]; int n;
                        while((n=in.read(buffer))!=-1) digest.update(buffer,0,n);
                        if(!expected.equals(ApkPatcher.hex(digest.digest()))) throw new IOException("Сохранённый отчёт не прошёл проверку SHA256");
                    }
                    PatchService.status(this,"Диагностический ZIP сохранён и проверен. Отправьте его разработчику вместе с описанием проблемы."); return;
                }
                if(request==703) {
                    new SaveManager(this).importArchive(getContentResolver().openInputStream(uri));
                    PatchService.status(this,"ZIP-копия открыта и проверена. Нажмите «Восстановить миры», чтобы вернуть её в установленную игру.");
                    return;
                }
                SaveManager manager=new SaveManager(this); JSONObject record=manager.verify();
                try(InputStream in=new FileInputStream(manager.archive(record)); OutputStream out=getContentResolver().openOutputStream(uri,"w")) {
                    if(out==null) throw new IOException("Папка не разрешила запись");
                    byte[] buffer=new byte[128*1024]; int n; while((n=in.read(buffer))!=-1) out.write(buffer,0,n);
                }
                try(InputStream in=getContentResolver().openInputStream(uri)) { SaveArchive.inspect(in,null); }
                PatchService.status(this,"ZIP-копия сохранена в выбранную папку и проверена. Копия внутри установщика тоже сохранена.");
            } catch(Exception error) { PatchService.status(this,Diagnostics.failure(this,action,error)); }
            finally { Diagnostics.finish(this,action); PatchService.BUSY.set(false); }
        },"snmp-export").start();
    }

    void render() {
        boolean reportBusy=PatchService.BUSY.get(); File diagnostic=Diagnostics.latest(this);
        diagnose.setEnabled(!reportBusy); shareReport.setEnabled(!reportBusy && diagnostic!=null); saveReport.setEnabled(!reportBusy && diagnostic!=null);
        errorDetails.setEnabled(Diagnostics.lastError(this).has("code"));
        updateText(diagnosticsNote,reportBusy?"Выполняется операция. Состояние показано в уведомлении и внизу экрана.":diagnostic==null?"Диагностический ZIP ещё не создан.":
            "Последний ZIP: "+new java.util.Date(diagnostic.lastModified())+" · "+(diagnostic.length()/1024)+" КиБ\n"+getSharedPreferences("diagnostics",0).getString("reportCompleteness","Состав и время игровых данных указаны внутри ZIP."));
        try {
            PatchEngine engine = new PatchEngine(this); PackageInfo info = engine.installed();
            JSONObject mod = engine.payload();
            String gameLabel="\nИгра: " + (info == null ? "не найдена" : info.versionName) + "\nМод: " + mod.getString("buildId").substring(0,12) + " · протокол " + mod.getInt("protocol") + "\n";
            if(info==null && engine.retained()!=null) gameLabel+="Игра осталась в другом профиле / клоне.\n";
            updateText(game,gameLabel);
            boolean busy = PatchService.BUSY.get();
            JSONObject plan = null; String planError = null;
            try { plan = engine.plan(); }
            catch (Exception invalidPlan) { planError = invalidPlan.getMessage(); }
            int pending = PatchService.pendingSession(this);
            SaveManager worlds=new SaveManager(this); JSONObject saved=worlds.latest();
            if(info==null && saved!=null && !getSharedPreferences("status",0).getBoolean("restoreRequired",false))
                getSharedPreferences("status",0).edit().putBoolean("restoreRequired",true).apply();
            String access=SaveBridge.readiness(); boolean needsRestore=worlds.needsRestore();
            updateText(savesNote,(access==null?"Shizuku: доступ предоставлен.":access)+"\n"+(saved==null?"Проверенной копии миров пока нет.":"Копия: "+saved.getInt("files")+" файлов, "+(saved.getLong("bytes")/1024)+" КиБ. "+(needsRestore?"После замены игры нужно восстановление.":"Копия хранится отдельно от игры.")));
            backup.setEnabled(!busy && pending<0 && info!=null && access==null && !needsRestore);
            restore.setEnabled(!busy && pending<0 && info!=null && saved!=null && access==null);
            export.setEnabled(!busy && saved!=null);
            importCopy.setEnabled(!busy && pending<0);
            prepare.setEnabled(!busy && info != null && pending < 0);
            String blocker = engine.installBlocker(plan);
            remove.setEnabled(!busy && pending<0 && info!=null && plan!=null && plan.getBoolean("full") && blocker!=null && saved!=null && access==null);
            install.setEnabled(!busy && plan != null && blocker == null && pending < 0);
            cancel.setVisibility(pending < 0 ? View.GONE : View.VISIBLE);
            cancel.setEnabled(!busy);
            String noteValue=plan == null ? (planError == null ? "После подготовки здесь станет доступна установка." : planError) : (blocker == null
                ? "Файлы проверены. Android запросит разрешение и подтверждение установки."
                : blocker);
            launch.setEnabled(!busy && info != null && !needsRestore);
            launchHost.setEnabled(launch.isEnabled());launchGuest.setEnabled(launch.isEnabled());
            if(needsRestore) noteValue="Мод установлен. Восстановите миры из проверенной копии перед запуском игры.";
            updateText(note,noteValue);
            if(removalToken!=null && removalToken.equals(getSharedPreferences("status",0).getString("removalToken",""))
                && System.currentTimeMillis()-getSharedPreferences("status",0).getLong("removalReadyAt",0)<60000) {
                removalToken=null;
                getSharedPreferences("status",0).edit().remove("removalToken").apply();
                startActivity(new Intent(Intent.ACTION_DELETE,Uri.parse("package:"+PatchEngine.GAME)));
            }
            updateText(status,getSharedPreferences("status", 0).getString("message", "Готов к подготовке. Сначала сохраните игру."));
        } catch (Exception e) {
            prepare.setEnabled(!PatchService.BUSY.get()); install.setEnabled(false);
            remove.setEnabled(false); restore.setEnabled(false); backup.setEnabled(false); launch.setEnabled(false);
            launchHost.setEnabled(false);launchGuest.setEnabled(false);
            updateText(note,e.getMessage());
            updateText(status,getSharedPreferences("status", 0).getString("message", "Нужна повторная подготовка."));
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
