package io.github.zerk235.snmp.installer;

import android.content.*;
import android.content.pm.PackageInstaller;

public final class InstallReceiver extends BroadcastReceiver {
    public void onReceive(Context context, Intent result) {
        int id = result.getIntExtra(PackageInstaller.EXTRA_SESSION_ID, -1);
        int pending = context.getSharedPreferences("status", 0).getInt("session", -1);
        if (id != pending || pending < 0) return;
        int status = result.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirm = result.getParcelableExtra(Intent.EXTRA_INTENT);
            if (confirm != null) {
                try { context.startActivity(confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)); }
                catch (Exception e) { PatchService.status(context, "Android не открыл подтверждение. Вернитесь в установщик и повторите установку."); }
            }
        } else if (status == PackageInstaller.STATUS_SUCCESS) {
            context.getSharedPreferences("status", 0).edit().remove("session").apply();
            try {
                PatchService.status(context,new SaveManager(context).needsRestore()
                    ? "Мод установлен. Теперь нажмите «Восстановить миры», прежде чем открывать игру."
                    : "Мод установлен. Можно запускать игру и подключаться в одной Wi-Fi сети.");
            } catch(Exception error) { PatchService.status(context,"Мод установлен. Проверьте резервную копию миров перед запуском: "+error.getMessage()); }
        } else {
            context.getSharedPreferences("status", 0).edit().remove("session").apply();
            PatchService.status(context, "Установка не завершена (" + status + "): " + result.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE)
                + "\nИгра автоматически не удаляется. Подготовленные файлы сохранены.");
        }
    }
}
