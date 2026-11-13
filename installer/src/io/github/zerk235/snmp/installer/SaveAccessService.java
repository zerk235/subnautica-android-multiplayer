package io.github.zerk235.snmp.installer;

import android.content.Context;
import android.os.*;
import android.system.Os;
import android.system.OsConstants;
import android.system.StructStat;
import org.json.JSONObject;
import java.io.*;

/** Shizuku UserService. Only this installer's UID and fixed game paths are allowed. */
public final class SaveAccessService extends Binder {
    static final String DESCRIPTOR="io.github.zerk235.snmp.installer.SaveAccess.v1";
    static final int SNAPSHOT=1, CHECK=2, RESTORE=3, CHECK_REMOVAL=4, CHECK_PROFILES=5, DIAGNOSTICS=6,
        PROFILE_STATE=7, RECOVER_GAME=8, CHECK_ACCESS=9;
    final int owner;
    final File saves;
    public SaveAccessService(Context context) {
        owner=context.getApplicationInfo().uid;
        saves=new File("/storage/emulated/"+(owner/100000)+"/Android/data/"+PatchEngine.GAME+"/files/SavedGames");
        attachInterface(null,DESCRIPTOR);
    }
    @Override protected synchronized boolean onTransact(int code,Parcel data,Parcel reply,int flags) throws RemoteException {
        if(code==INTERFACE_TRANSACTION) { reply.writeString(DESCRIPTOR); return true; }
        int caller=Binder.getCallingUid();
        if(code==16777115 && (caller==0 || caller==2000 || caller==owner)) { System.exit(0); return true; }
        if(caller!=owner) throw new SecurityException("Wrong caller UID");
        data.enforceInterface(DESCRIPTOR);
        try {
            JSONObject result;
            if(code==CHECK_ACCESS) {
                result=new JSONObject().put("callerUid",caller);
            } else if(code==PROFILE_STATE) {
                result=profiles().json(owner/100000);
            } else if(code==RECOVER_GAME) {
                int user=owner/100000;
                GameProfiles before=profiles(); before.requireRecovery(user);
                // Re-register the existing, identically signed APK; never uninstall any profile.
                String output=command("/system/bin/cmd","package","install-existing","--user",String.valueOf(user),"--wait",PatchEngine.GAME);
                GameProfiles after=profiles();
                if(!after.installed.contains(user) || !after.installed.containsAll(before.installed))
                    throw new IOException("Android не подтвердил восстановление игры в текущем профиле: "+output);
                result=after.json(user).put("verified",true);
            } else if(code==DIAGNOSTICS) {
                // Read-only: no force-stop, save access or arbitrary caller-provided path/command.
                try(ParcelFileDescriptor fd=ParcelFileDescriptor.CREATOR.createFromParcel(data)) {
                    result=DiagnosticLogs.capture(saves.getParentFile(),new ParcelFileDescriptor.AutoCloseOutputStream(ParcelFileDescriptor.dup(fd.getFileDescriptor())),true,owner/100000);
                }
            } else if(code==SNAPSHOT) {
                try(ParcelFileDescriptor fd=ParcelFileDescriptor.CREATOR.createFromParcel(data)) {
                    JSONObject manifest=SaveArchive.create(saves,new ParcelFileDescriptor.AutoCloseOutputStream(ParcelFileDescriptor.dup(fd.getFileDescriptor())));
                    result=new JSONObject().put("files",manifest.getJSONObject("files").length()).put("bytes",manifest.getLong("bytes"));
                }
            } else if(code==CHECK || code==CHECK_REMOVAL) {
                if(code==CHECK_REMOVAL) {
                    checkProfiles(command("/system/bin/dumpsys","package",PatchEngine.GAME),owner/100000);
                    stopGame();
                }
                JSONObject current=SaveArchive.inventory(saves);
                // A manifest can be larger than Binder's transaction limit: compare using a supplied archive.
                try(ParcelFileDescriptor fd=ParcelFileDescriptor.CREATOR.createFromParcel(data)) {
                    JSONObject expected=SaveArchive.inspect(new ParcelFileDescriptor.AutoCloseInputStream(ParcelFileDescriptor.dup(fd.getFileDescriptor())),null);
                    if(!SaveArchive.same(current,expected)) throw new IOException("Миры изменились после копирования. Создайте новую копию");
                    result=new JSONObject().put("verified",true).put("files",current.getJSONObject("files").length());
                }
            } else if(code==CHECK_PROFILES) {
                checkProfiles(command("/system/bin/dumpsys","package",PatchEngine.GAME),owner/100000);
                result=new JSONObject().put("verified",true);
            } else if(code==RESTORE) {
                // Fixed argv, never a caller-provided shell command.
                stopGame();
                File parent=saves.getParentFile();
                if(!parent.isDirectory()) throw new IOException("Сначала один раз откройте установленную игру до меню и закройте её");
                StructStat gameFiles=Os.lstat(parent.getAbsolutePath());
                if(!OsConstants.S_ISDIR(gameFiles.st_mode)) throw new IOException("Неверная папка установленной игры");
                boolean root=android.os.Process.myUid()==0;
                if(root && (gameFiles.st_uid/100000!=owner/100000 || gameFiles.st_uid%100000<10000))
                    throw new IOException("Не удалось определить владельца папки игры. Откройте игру до меню и закройте её");
                try(ParcelFileDescriptor fd=ParcelFileDescriptor.CREATOR.createFromParcel(data)) {
                    result=SaveArchive.restore(new ParcelFileDescriptor.AutoCloseInputStream(ParcelFileDescriptor.dup(fd.getFileDescriptor())),saves,
                        staging->SaveArchive.prepareGameAccess(staging,gameFiles.st_uid,gameFiles.st_gid,root));
                }
            } else return super.onTransact(code,data,reply,flags);
            reply.writeNoException(); reply.writeInt(1); reply.writeString(result.put("serviceUid",android.os.Process.myUid()).toString());
        } catch(Exception error) {
            reply.writeNoException(); reply.writeInt(0); reply.writeString(error.getClass().getSimpleName()+": "+error.getMessage());
        }
        return true;
    }
    void stopGame() throws Exception {
        String output=command("/system/bin/am","force-stop","--user",String.valueOf(owner/100000),PatchEngine.GAME);
        if(output.contains("Exception")) throw new IOException("Не удалось остановить игру");
    }
    static void checkProfiles(String dump,int user) throws IOException {
        new GameProfiles(dump).requireOnly(user);
    }
    GameProfiles profiles() throws Exception { return new GameProfiles(command("/system/bin/dumpsys","package",PatchEngine.GAME)); }
    static String command(String... args) throws Exception {
        java.lang.Process process=new ProcessBuilder(args).redirectErrorStream(true).start();
        java.util.concurrent.FutureTask<byte[]> read=new java.util.concurrent.FutureTask<>(()->ApkPatcher.readSmall(process.getInputStream(),2*1024*1024));
        Thread drain=new Thread(read,"snmp-command-output"); drain.setDaemon(true); drain.start();
        try {
            if(!process.waitFor(15,java.util.concurrent.TimeUnit.SECONDS)) throw new IOException("Системный запрос не завершился вовремя");
            String output=new String(read.get(2,java.util.concurrent.TimeUnit.SECONDS),java.nio.charset.StandardCharsets.UTF_8);
            if(process.exitValue()!=0) throw new IOException("Android отклонил системный запрос: "+process.exitValue());
            return output;
        } finally { process.destroy(); }
    }
}
