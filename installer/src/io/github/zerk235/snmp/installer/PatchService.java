package io.github.zerk235.snmp.installer;

import android.app.*;
import android.content.*;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageInfo;
import android.os.*;
import org.json.JSONObject;
import java.io.*;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

public final class PatchService extends Service {
    static final AtomicBoolean BUSY = new AtomicBoolean();
    static final String CHANNEL = "patching";
    long lastReport;
    public IBinder onBind(Intent intent) { return null; }

    static void status(Context context, String message) {
        status(context, message, 0, 0);
    }

    private static void status(Context context, String message, long done, long total) {
        context.getSharedPreferences("status", 0).edit().putString("message", message)
            .putLong("progressDone", done).putLong("progressTotal", total).apply();
        Diagnostics.progress(context,message);
    }

    public int onStartCommand(Intent intent, int flags, int startId) {
        if (pendingSession(this) >= 0 && (intent==null || !"diagnostics".equals(intent.getAction()))) {
            status(this, "Есть незавершённая установка Android. Подтвердите или отмените её в установщике.");
            if (!BUSY.get()) stopSelf();
            return START_NOT_STICKY;
        }
        if (!BUSY.compareAndSet(false, true)) return START_NOT_STICKY;
        getSystemService(NotificationManager.class).createNotificationChannel(new NotificationChannel(CHANNEL, "Подготовка мода", NotificationManager.IMPORTANCE_LOW));
        startForeground(1, notification("Подготовка…"));
        final String action=intent==null?"prepare":intent.getAction();
        Diagnostics.begin(this,action);
        status(this, "Запуск операции…");
        new Thread(() -> {
            PowerManager.WakeLock wake = getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "SNMP:prepare");
            try {
                wake.acquire(30 * 60 * 1000L);
                PatchEngine engine = new PatchEngine(this);
                ApkPatcher.Progress progress = (stage, done, total) -> {
                    long now = SystemClock.elapsedRealtime();
                    if (now - lastReport < 750 && done != 0) return;
                    lastReport = now;
                    String text = total > 0 ? stage + " · " + Math.min(100, done * 100 / total) + "%" : stage;
                    status(this, text, done, total);
                    getSystemService(NotificationManager.class).notify(1, notification(text));
                };
                if("diagnostics".equals(action)) {
                    status(this,"Сбор диагностики. Игра продолжает работать…");
                    File report=Diagnostics.create(this,intent.getStringExtra("category"),intent.getStringExtra("description"),true);
                    status(this,"Диагностический ZIP готов. Нажмите «Отправить ZIP» или «Сохранить диагностический ZIP». Получите такой же отчёт со второго телефона при проблеме синхронизации.");
                } else if ("install".equals(action)) install(engine, progress);
                else if ("recover-assets".equals(action)) recoverAssets(engine, intent.getData(), progress);
                else if("recover-game".equals(action)) {
                    if(engine.installed()!=null || engine.retained()==null) throw new IOException("Восстановление регистрации игры сейчас не требуется");
                    SaveManager worlds=new SaveManager(this);
                    if(worlds.latest()!=null) worlds.verify();
                    String cert=PatchEngine.certificate(engine.retained());
                    getSharedPreferences("status",0).edit().putBoolean("restoreRequired",true).commit();
                    try(SaveBridge bridge=new SaveBridge(this)) {
                        JSONObject result=bridge.call(SaveAccessService.RECOVER_GAME,null,false);
                        Diagnostics.event(this,"game_profile_recovered",result.toString());
                    }
                    if(!cert.equals(PatchEngine.certificate(engine.installed()))) throw new IOException("Не удалось подтвердить подпись восстановленной игры");
                    status(this,worlds.latest()!=null?"Игра возвращена в текущий профиль без удаления клона. Теперь восстановите миры из проверенной копии перед запуском."
                        :"Игра возвращена в текущий профиль без удаления клона. Если есть ZIP миров, импортируйте и восстановите его перед запуском.");
                }
                else if("backup".equals(action)) {
                    status(this,"Копирование миров и проверка SHA256…");
                    JSONObject record=new SaveManager(this).backup();
                    status(this,"Копия миров проверена: "+record.getInt("files")+" файлов. Она находится отдельно от игры, в установщике.");
                } else if("restore".equals(action)) {
                    status(this,"Восстановление миров. Игра будет остановлена…");
                    JSONObject report=new SaveManager(this).restore();
                    status(this,"Миры восстановлены: "+report.getInt("files")+" файлов, SHA256 совпал. Можно открывать игру. Прежняя папка и резервная копия сохранены.");
                } else if ("check-removal".equals(action) || "check-removal-without-backup".equals(action)) {
                    boolean discardWorlds = "check-removal-without-backup".equals(action);
                    getSharedPreferences("status", 0).edit().remove("removalToken").commit();
                    String identity = discardWorlds ? checkRemovalWithoutBackup(engine, progress)
                        : new SaveManager(this).checkRemoval(progress);
                    getSharedPreferences("status",0).edit().putString("removalToken",intent.getStringExtra("token"))
                        .putString("removalIdentity",identity)
                        .putBoolean("removalDiscardsWorlds", discardWorlds)
                        .putLong("removalReadyAt",SystemClock.elapsedRealtime()).apply();
                    status(this, discardWorlds ? "APK проверены. Android запросит подтверждение удаления игры и её миров."
                        : "APK и актуальная копия миров проверены. Подтвердите удаление старой игры в системном окне.");
                }
                else {
                    if ("prepare-experimental".equals(action)) {
                        engine.prepare(progress, intent.getStringExtra("acceptedVersion"), intent.getLongExtra("acceptedCode", -1));
                    } else {
                        engine.prepare(progress);
                    }
                    String blocker = engine.installBlocker(engine.plan());
                    status(this, blocker == null ? "Файлы готовы. Можно устанавливать мод." : "Файлы готовы.\n\n" + blocker);
                }
            } catch (Throwable e) {
                android.util.Log.e("SNMPInstaller", "Operation failed", e);
                status(this, Diagnostics.failure(this,action,e));
            } finally {
                Diagnostics.finish(this,action);
                if (wake.isHeld()) wake.release();
                BUSY.set(false); stopForeground(true); stopSelf();
            }
        }, "snmp-prepare").start();
        return START_NOT_STICKY;
    }

    String checkRemovalWithoutBackup(PatchEngine engine, ApkPatcher.Progress progress) throws Exception {
        JSONObject plan = engine.plan();
        PackageInfo game = engine.installed();
        String blocked = engine.removalBlocker(plan);
        if (blocked != null) throw new IOException(blocked);
        String identity = PatchEngine.removalIdentity(game);
        boolean profilesVerified = false;
        if (SaveBridge.readiness() == null) {
            try (SaveBridge bridge = new SaveBridge(this)) {
                bridge.call(SaveAccessService.CHECK_PROFILES, null, false);
                profilesVerified = true;
            } catch (SaveBridge.UnavailableException unavailable) {
                Diagnostics.event(this, "shizuku_profile_check_unavailable", unavailable.getMessage());
            }
        }
        if (!profilesVerified && getSystemService(UserManager.class).getUserProfiles().size() != 1) {
            throw new IOException("Для замены при нескольких профилях Android нужна проверка через Shizuku");
        }
        engine.verifyPlan(plan, progress);
        if (!identity.equals(engine.removalIdentity())) throw new IOException("Игра изменилась во время проверки");
        return identity;
    }

    Notification notification(String text) {
        return new Notification.Builder(this, CHANNEL).setSmallIcon(android.R.drawable.stat_sys_download)
            .setContentTitle("Subnautica Multiplayer").setContentText(text).setOngoing(true).build();
    }

    static int pendingSession(Context context) {
        int id = context.getSharedPreferences("status", 0).getInt("session", -1);
        if (id >= 0 && context.getPackageManager().getPackageInstaller().getSessionInfo(id) == null) {
            context.getSharedPreferences("status", 0).edit().remove("session").apply(); return -1;
        }
        return id;
    }

    void install(PatchEngine engine, ApkPatcher.Progress progress) throws Exception {
        JSONObject plan = engine.plan(); String blocker = engine.installBlocker(plan);
        if (blocker != null) throw new IOException(blocker);
        if (!getPackageManager().canRequestPackageInstalls()) throw new IOException("Разрешите установку из этого приложения");
        List<File> files = engine.verifyPlan(plan, progress);
        blocker = engine.installBlocker(plan); if (blocker != null) throw new IOException(blocker);
        // If a migration backup exists, verify it before installing into the now-absent game slot.
        boolean discardWorlds = PatchEngine.targetIdentity(plan).equals(
            getSharedPreferences("status", 0).getString(SaveManager.SKIP_RESTORE_TARGET, ""));
        if (engine.installed() == null && !discardWorlds && new SaveManager(this).latest() != null)
            new SaveManager(this).verify();
        commitInstall(files, false, progress);
    }

    void recoverAssets(PatchEngine engine, android.net.Uri source, ApkPatcher.Progress progress) throws Exception {
        if (!getPackageManager().canRequestPackageInstalls()) throw new IOException("Разрешите установку из этого приложения");
        File input = File.createTempFile("assets-input-", ".apk", getCacheDir());
        File signed = new File(getCacheDir(), input.getName() + ".signed.apk");
        try {
            progress.update("Чтение пакета ресурсов", 0, 0);
            File base = engine.prepareAssetsRecovery(source, input, signed, progress);
            // Submit the complete pair together so the system confirmation cannot
            // replace the game with a base-only APK during resource recovery.
            commitInstall(java.util.Arrays.asList(base, signed), true, progress);
        } finally {
            input.delete();
            signed.delete();
        }
    }

    void commitInstall(List<File> files, boolean assetsOnly, ApkPatcher.Progress progress) throws Exception {
        PackageInstaller installer = getPackageManager().getPackageInstaller();
        int id = SessionWriter.stage(this, PatchEngine.GAME, files, progress);
        boolean committed = false;
        try (PackageInstaller.Session session = installer.openSession(id)) {
            Intent callback = new Intent(this, InstallReceiver.class).setAction(getPackageName() + ".INSTALL_RESULT")
                .putExtra("assetsOnly", assetsOnly);
            int mutable = Build.VERSION.SDK_INT >= 31 ? 0x02000000 : 0;
            PendingIntent result = PendingIntent.getBroadcast(this, id, callback, PendingIntent.FLAG_UPDATE_CURRENT | mutable);
            getSharedPreferences("status", 0).edit().putInt("session", id).commit();
            status(this, "Подтвердите установку в системном окне Android.");
            session.commit(result.getIntentSender()); committed = true;
        } finally {
            if (!committed) {
                installer.abandonSession(id);
                getSharedPreferences("status", 0).edit().remove("session").apply();
            }
        }
    }
}
