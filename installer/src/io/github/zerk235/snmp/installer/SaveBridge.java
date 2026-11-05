package io.github.zerk235.snmp.installer;

import android.content.*;
import android.content.pm.PackageManager;
import android.os.*;
import org.json.JSONObject;
import rikka.shizuku.Shizuku;
import java.io.*;
import java.util.concurrent.*;

final class SaveBridge implements AutoCloseable {
    final Shizuku.UserServiceArgs args;
    final ServiceConnection connection;
    IBinder binder;
    static String readiness() {
        try {
            if(!Shizuku.pingBinder()) return "Запустите Shizuku и вернитесь сюда";
            if(Shizuku.getVersion()<13) return "Обновите Shizuku до версии 13 или новее";
            if(Shizuku.checkSelfPermission()!=PackageManager.PERMISSION_GRANTED) return "Разрешите установщику доступ через Shizuku";
            return null;
        } catch(RuntimeException e) { return "Shizuku недоступен: "+e.getMessage(); }
    }
    SaveBridge(Context context) throws Exception {
        String blocked=readiness(); if(blocked!=null) throw new IOException(blocked);
        CountDownLatch ready=new CountDownLatch(1);
        args=new Shizuku.UserServiceArgs(new ComponentName(context,SaveAccessService.class))
            .tag("snmp-save-backup-v1").version(3).daemon(false).processNameSuffix("save_access");
        connection=new ServiceConnection() {
            public void onServiceConnected(ComponentName name,IBinder service) { binder=service; ready.countDown(); }
            public void onServiceDisconnected(ComponentName name) { binder=null; }
        };
        Shizuku.bindUserService(args,connection);
        if(!ready.await(20,TimeUnit.SECONDS) || binder==null) { close(); throw new IOException("Не удалось подключить службу копирования Shizuku"); }
    }
    JSONObject call(int operation,File archive,boolean write) throws Exception {
        if(binder==null || !binder.pingBinder()) throw new IOException("Shizuku остановлен. Запустите его снова");
        try(ParcelFileDescriptor fd=ParcelFileDescriptor.open(archive,write?
            ParcelFileDescriptor.MODE_CREATE|ParcelFileDescriptor.MODE_TRUNCATE|ParcelFileDescriptor.MODE_WRITE_ONLY:ParcelFileDescriptor.MODE_READ_ONLY)) {
            Parcel data=Parcel.obtain(),reply=Parcel.obtain();
            try {
                data.writeInterfaceToken(SaveAccessService.DESCRIPTOR); fd.writeToParcel(data,0);
                if(!binder.transact(operation,data,reply,0)) throw new IOException("Служба не поддерживает запрос");
                reply.readException(); boolean success=reply.readInt()==1; String result=reply.readString();
                if(!success) throw new IOException(result);
                return new JSONObject(result);
            } finally { data.recycle(); reply.recycle(); }
        }
    }
    public void close() { try { Shizuku.unbindUserService(args,connection,true); } catch(RuntimeException ignored) {} }
}
