package io.github.zerk235.snmp.installer;

import android.content.*;
import android.content.pm.PackageManager;
import android.os.*;
import org.json.JSONObject;
import rikka.shizuku.Shizuku;
import java.io.*;
import java.util.concurrent.*;

final class SaveBridge implements AutoCloseable {
    static final Semaphore SERVICE_LOCK=new Semaphore(1,true);
    final Shizuku.UserServiceArgs args;
    final ServiceConnection connection;
    IBinder binder;
    boolean locked;
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
        if(!SERVICE_LOCK.tryAcquire(25,TimeUnit.SECONDS)) throw new IOException("Служба Shizuku занята другой проверкой. Повторите операцию");
        locked=true;
        CountDownLatch ready=new CountDownLatch(1);
        args=new Shizuku.UserServiceArgs(new ComponentName(context,SaveAccessService.class))
            .tag("snmp-save-backup-"+android.os.Process.myUid()).version(5).daemon(false).processNameSuffix("save_access");
        connection=new ServiceConnection() {
            public void onServiceConnected(ComponentName name,IBinder service) { binder=service; ready.countDown(); }
            public void onServiceDisconnected(ComponentName name) { binder=null; }
        };
        try {
            Shizuku.bindUserService(args,connection);
            if(!ready.await(20,TimeUnit.SECONDS) || binder==null) throw new IOException("Не удалось подключить службу копирования Shizuku");
            if(transact(SaveAccessService.CHECK_ACCESS,null).getInt("callerUid")!=android.os.Process.myUid())
                throw new IOException("Shizuku подключил службу другого профиля Android");
        } catch(Exception error) { close(); throw error; }
    }
    JSONObject call(int operation,File archive,boolean write) throws Exception {
        if(archive==null) {
            if(operation!=SaveAccessService.PROFILE_STATE && operation!=SaveAccessService.RECOVER_GAME && operation!=SaveAccessService.CHECK_PROFILES)
                throw new IOException("Для этой операции требуется архив");
            return transact(operation,null);
        }
        if(binder==null || !binder.pingBinder()) throw new IOException("Shizuku остановлен. Запустите его снова");
        try(ParcelFileDescriptor fd=ParcelFileDescriptor.open(archive,write?
            ParcelFileDescriptor.MODE_CREATE|ParcelFileDescriptor.MODE_TRUNCATE|ParcelFileDescriptor.MODE_WRITE_ONLY:ParcelFileDescriptor.MODE_READ_ONLY)) {
            return transact(operation,fd);
        }
    }
    private JSONObject transact(int operation,ParcelFileDescriptor fd) throws Exception {
        if(binder==null || !binder.pingBinder()) throw new IOException("Shizuku остановлен. Запустите его снова");
        Parcel data=Parcel.obtain(),reply=Parcel.obtain();
            try {
                data.writeInterfaceToken(SaveAccessService.DESCRIPTOR); if(fd!=null) fd.writeToParcel(data,0);
                if(!binder.transact(operation,data,reply,0)) throw new IOException("Служба не поддерживает запрос");
                reply.readException(); boolean success=reply.readInt()==1; String result=reply.readString();
                if(!success) throw new IOException(result);
                return new JSONObject(result);
            } finally { data.recycle(); reply.recycle(); }
    }
    public void close() {
        if(!locked) return;
        // Detach this client without asynchronously killing the service while the next
        // check is binding. daemon(false) still ties its lifetime to the client process.
        try { Shizuku.unbindUserService(args,connection,false); }
        catch(RuntimeException ignored) {}
        finally { locked=false; SERVICE_LOCK.release(); }
    }
}
