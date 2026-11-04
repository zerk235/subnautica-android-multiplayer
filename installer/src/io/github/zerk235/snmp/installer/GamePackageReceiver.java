package io.github.zerk235.snmp.installer;
import android.content.*;

/** Tracks removal even if the same certificate/package UID is later reused. */
public final class GamePackageReceiver extends BroadcastReceiver {
    public void onReceive(Context context,Intent intent) {
        if(intent.getData()==null || !PatchEngine.GAME.equals(intent.getData().getSchemeSpecificPart())) return;
        if(Intent.ACTION_PACKAGE_REMOVED.equals(intent.getAction()) && !intent.getBooleanExtra(Intent.EXTRA_REPLACING,false)) {
            context.getSharedPreferences("status",0).edit().putBoolean("restoreRequired",true).apply();
            PatchService.status(context,"Игра удалена в текущем профиле. Подготовленные APK и проверенная копия миров остаются в установщике. После установки восстановите миры.");
        }
    }
}
