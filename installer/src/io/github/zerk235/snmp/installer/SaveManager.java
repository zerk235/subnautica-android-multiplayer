package io.github.zerk235.snmp.installer;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.util.AtomicFile;
import org.json.JSONObject;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.UUID;

final class SaveManager {
    static final Object INDEX_LOCK=new Object();
    final Context context;
    final File folder;
    final AtomicFile index;
    SaveManager(Context context) {
        this(context,new File(context.getFilesDir(),"save-backups"));
    }
    SaveManager(Context context,File folder) {
        this.context=context.getApplicationContext();
        this.folder=folder;
        index=new AtomicFile(new File(folder,"latest.json"));
    }
    JSONObject latest() throws Exception {
        synchronized(INDEX_LOCK) {
        if(!index.getBaseFile().isFile() && !new File(index.getBaseFile().getPath()+".bak").isFile()) return null;
        return new JSONObject(new String(ApkPatcher.readSmall(index.openRead(),65536),StandardCharsets.UTF_8));
        }
    }
    File archive(JSONObject record) throws Exception {
        String name=record.getString("archive");
        if(!name.matches("[0-9a-f-]{36}\\.zip")) throw new IOException("Неверное имя резервной копии");
        File file=new File(folder,name);
        if(!file.getCanonicalPath().startsWith(folder.getCanonicalPath()+File.separator) || !file.isFile()) throw new IOException("Резервная копия не найдена");
        return file;
    }
    void save(JSONObject record) throws Exception {
        synchronized(INDEX_LOCK) {
        FileOutputStream stream=index.startWrite();
        try { stream.write(record.toString(2).getBytes(StandardCharsets.UTF_8)); index.finishWrite(stream); }
        catch(Exception error) { index.failWrite(stream); throw error; }
        }
    }
    JSONObject verify() throws Exception {
        JSONObject record=latest(); if(record==null) throw new IOException("Сначала создайте копию миров");
        File file=archive(record);
        if(file.length()!=record.getLong("archiveBytes") || !ApkPatcher.sha256(file).equals(record.getString("sha256")))
            throw new IOException("Резервная копия повреждена. Игру не удаляйте");
        JSONObject manifest=SaveArchive.inspect(new FileInputStream(file),null);
        if(manifest.getJSONObject("files").length()!=record.getInt("files") || manifest.getLong("bytes")!=record.getLong("bytes"))
            throw new IOException("Состав копии миров не совпал с отчётом");
        return record;
    }
    JSONObject backup() throws Exception {
        PatchEngine engine=new PatchEngine(context); PackageInfo game=engine.installed();
        if(game==null || !PatchEngine.GAME_VERSION.equals(game.versionName)) throw new IOException("Нужна установленная поддерживаемая игра");
        String cert=PatchEngine.certificate(game);
        JSONObject previous=latest();
        if(needsRestore(previous,game)) throw new IOException("Сначала восстановите предыдущую копию: новая копия не должна заменить её до восстановления");
        if(!folder.isDirectory() && !folder.mkdirs()) throw new IOException("Не удалось создать папку копий");
        String id=UUID.randomUUID().toString(); File pending=new File(folder,id+".partial"),complete=new File(folder,id+".zip");
        JSONObject remote;
        try(SaveBridge bridge=new SaveBridge(context)) { remote=bridge.call(SaveAccessService.SNAPSHOT,pending,true); }
        JSONObject checked=SaveArchive.inspect(new FileInputStream(pending),null);
        if(remote.getInt("files")!=checked.getJSONObject("files").length() || remote.getLong("bytes")!=checked.getLong("bytes"))
            throw new IOException("Проверка полученной копии миров не прошла");
        PackageInfo current=engine.installed();
        if(current==null || !cert.equals(PatchEngine.certificate(current)) || game.applicationInfo.uid!=current.applicationInfo.uid)
            throw new IOException("Игра изменилась во время копирования");
        try(FileOutputStream stream=new FileOutputStream(pending,true)) { stream.getFD().sync(); }
        String sha=ApkPatcher.sha256(pending); long length=pending.length();
        if(!pending.renameTo(complete)) throw new IOException("Не удалось завершить копию миров");
        JSONObject record=new JSONObject().put("schema",1).put("archive",complete.getName()).put("sha256",sha).put("archiveBytes",length)
            .put("files",checked.getJSONObject("files").length()).put("bytes",checked.getLong("bytes"))
            .put("sourceCert",cert).put("sourceUid",game.applicationInfo.uid).put("createdAt",System.currentTimeMillis())
            .put("serviceUid",remote.getInt("serviceUid")).put("verified",true);
        save(record); return record;
    }
    JSONObject importArchive(InputStream input) throws Exception {
        if(!folder.isDirectory() && !folder.mkdirs()) throw new IOException("Не удалось создать папку копий");
        String id=UUID.randomUUID().toString(); File pending=new File(folder,id+".partial"),complete=new File(folder,id+".zip");
        try(InputStream in=input; FileOutputStream out=new FileOutputStream(pending)) {
            byte[] buffer=new byte[128*1024]; long size=0; int n;
            while((n=in.read(buffer))!=-1) {
                size+=n; if(size>SaveArchive.MAX_TOTAL+32L*1024*1024) throw new IOException("Слишком большой архив миров");
                out.write(buffer,0,n);
            }
            out.getFD().sync();
        }
        JSONObject checked=SaveArchive.inspect(new FileInputStream(pending),null);
        String sha=ApkPatcher.sha256(pending); long length=pending.length();
        if(!pending.renameTo(complete)) throw new IOException("Не удалось сохранить импортированную копию");
        PackageInfo game=new PatchEngine(context).installed();
        JSONObject record=new JSONObject().put("schema",1).put("archive",complete.getName()).put("sha256",sha).put("archiveBytes",length)
            .put("files",checked.getJSONObject("files").length()).put("bytes",checked.getLong("bytes"))
            .put("sourceCert",game==null?"":PatchEngine.certificate(game)).put("sourceUid",game==null?-1:game.applicationInfo.uid)
            .put("createdAt",System.currentTimeMillis()).put("imported",true).put("verified",true);
        save(record); return record;
    }
    void checkRemoval(ApkPatcher.Progress progress) throws Exception {
        PatchEngine engine=new PatchEngine(context); JSONObject plan=engine.plan(); PackageInfo game=engine.installed();
        if(game==null || plan==null || !plan.getBoolean("full") || !PatchEngine.GAME_VERSION.equals(game.versionName)
            || game.getLongVersionCode()!=plan.getLong("versionCode") || PatchEngine.certificate(game).equals(plan.getString("cert")))
            throw new IOException("Удаление для этой установки не требуется");
        JSONObject record=verify();
        if(!record.getString("sourceCert").equals(PatchEngine.certificate(game)) || record.getInt("sourceUid")!=game.applicationInfo.uid)
            throw new IOException("Копия относится к другой установке игры. Создайте новую");
        try(SaveBridge bridge=new SaveBridge(context)) { bridge.call(SaveAccessService.CHECK_PROFILES,archive(record),false); }
        engine.verifyPlan(plan,progress);
        try(SaveBridge bridge=new SaveBridge(context)) { bridge.call(SaveAccessService.CHECK_REMOVAL,archive(record),false); }
        PackageInfo after=engine.installed();
        if(after==null || !PatchEngine.GAME_VERSION.equals(after.versionName) || after.applicationInfo.uid!=game.applicationInfo.uid
            || !PatchEngine.certificate(after).equals(record.getString("sourceCert"))) throw new IOException("Игра изменилась во время проверки");
    }
    boolean needsRestore() throws Exception { return needsRestore(latest(),new PatchEngine(context).installed()); }
    boolean needsRestore(JSONObject record,PackageInfo game) throws Exception {
        return record!=null && game!=null && (context.getSharedPreferences("status",0).getBoolean("restoreRequired",false)
            || (!record.getString("sourceCert").equals(PatchEngine.certificate(game))
            && !record.optString("restoredCert","").equals(PatchEngine.certificate(game))));
    }
    JSONObject restore() throws Exception {
        JSONObject record=verify(); PackageInfo game=new PatchEngine(context).installed();
        if(game==null || !PatchEngine.GAME_VERSION.equals(game.versionName)) throw new IOException("Сначала установите подготовленную игру");
        JSONObject report;
        try(SaveBridge bridge=new SaveBridge(context)) { report=bridge.call(SaveAccessService.RESTORE,archive(record),false); }
        if(!report.getBoolean("verified") || report.getInt("files")!=record.getInt("files") || report.getLong("bytes")!=record.getLong("bytes"))
            throw new IOException("Восстановление не подтверждено. Резервная копия сохранена");
        record.put("restoredCert",PatchEngine.certificate(game)).put("restoredAt",System.currentTimeMillis());
        save(record);
        context.getSharedPreferences("status",0).edit().remove("restoreRequired").apply();
        return report;
    }
}
