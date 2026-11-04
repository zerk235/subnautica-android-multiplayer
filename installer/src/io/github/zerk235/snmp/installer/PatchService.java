package io.github.zerk235.snmp.installer;

import android.app.*;
import android.content.*;
import android.content.pm.PackageInstaller;
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
        context.getSharedPreferences("status", 0).edit().putString("message", message).apply();
    }

    public int onStartCommand(Intent intent, int flags, int startId) {
        if (pendingSession(this) >= 0) {
            status(this, "Есть незавершённая установка Android. Подтвердите или отмените её в установщике.");
            if (!BUSY.get()) stopSelf();
            return START_NOT_STICKY;
        }
        if (!BUSY.compareAndSet(false, true)) return START_NOT_STICKY;
        getSystemService(NotificationManager.class).createNotificationChannel(new NotificationChannel(CHANNEL, "Подготовка мода", NotificationManager.IMPORTANCE_LOW));
        startForeground(1, notification("Подготовка…"));
        final String action=intent==null?"prepare":intent.getAction();
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
                    status(this, text);
                    getSystemService(NotificationManager.class).notify(1, notification(text));
                };
                if ("install".equals(action)) install(engine, progress);
                else if("backup".equals(action)) {
                    status(this,"Копирование миров и проверка SHA256…");
                    JSONObject record=new SaveManager(this).backup();
                    status(this,"Копия миров проверена: "+record.getInt("files")+" файлов. Она находится отдельно от игры, в установщике.");
                } else if("restore".equals(action)) {
                    status(this,"Восстановление миров. Игра будет остановлена…");
                    JSONObject report=new SaveManager(this).restore();
                    status(this,"Миры восстановлены: "+report.getInt("files")+" файлов, SHA256 совпал. Можно открывать игру. Прежняя папка и резервная копия сохранены.");
                } else if("check-removal".equals(action)) {
                    new SaveManager(this).checkRemoval(progress);
                    getSharedPreferences("status",0).edit().putString("removalToken",intent.getStringExtra("token"))
                        .putLong("removalReadyAt",System.currentTimeMillis()).apply();
                    status(this,"APK и актуальная копия миров проверены. Подтвердите удаление старой игры в системном окне.");
                }
                else {
                    engine.prepare(progress);
                    if(engine.plan().getBoolean("full") && SaveBridge.readiness()==null && !new SaveManager(this).needsRestore()) {
                        status(this,"APK готовы. Копирование миров и проверка SHA256…");
                        new SaveManager(this).backup();
                    }
                    String blocker = engine.installBlocker(engine.plan());
                    status(this, blocker == null ? "Файлы готовы. Можно устанавливать мод." : "Файлы готовы.\n\n" + blocker);
                }
            } catch (Throwable e) {
                android.util.Log.e("SNMPInstaller", "Operation failed", e);
                status(this, "Не удалось завершить: " + e.getClass().getSimpleName() + ": " + e.getMessage());
            } finally {
                if (wake.isHeld()) wake.release();
                BUSY.set(false); stopForeground(true); stopSelf();
            }
        }, "snmp-prepare").start();
        return START_NOT_STICKY;
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
        if(engine.installed()==null && new SaveManager(this).latest()!=null) new SaveManager(this).verify();
        PackageInstaller installer = getPackageManager().getPackageInstaller();
        int id = SessionWriter.stage(this, PatchEngine.GAME, plan.getBoolean("full"), files, progress);
        boolean committed = false;
        try (PackageInstaller.Session session = installer.openSession(id)) {
            Intent callback = new Intent(this, InstallReceiver.class).setAction(getPackageName() + ".INSTALL_RESULT");
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
