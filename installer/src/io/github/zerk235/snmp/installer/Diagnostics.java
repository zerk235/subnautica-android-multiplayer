package io.github.zerk235.snmp.installer;

import android.content.*;
import android.content.pm.*;
import android.net.*;
import android.os.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.*;
import java.util.zip.*;
import org.json.*;

/** Bounded local support reports. Never uploads; never reads saves, signing keys or other apps' logs. */
final class Diagnostics {
    static final Object LOCK=new Object();
    static final int JOURNAL_LIMIT=256*1024;
    static File folder(Context c) { return new File(c.getFilesDir(),"diagnostics"); }
    static String limit(String s,int n) { if(s==null) return ""; return s.length()>n?s.substring(0,n)+"\n[truncated]":s; }
    static void event(Context c,String kind,String message) {
        synchronized(LOCK) {
            try {
                File root=folder(c); if(!root.isDirectory() && !root.mkdirs()) return;
                File journal=new File(root,"installer.jsonl");
                if(journal.length()>JOURNAL_LIMIT) {
                    File previous=new File(root,"installer.previous.jsonl");
                    if(previous.exists() && !previous.delete()) return;
                    if(!journal.renameTo(previous)) return;
                }
                JSONObject item=new JSONObject().put("at",System.currentTimeMillis()).put("kind",kind).put("message",limit(message,24000));
                try(FileOutputStream out=new FileOutputStream(journal,true)) { out.write((item.toString()+"\n").getBytes(StandardCharsets.UTF_8)); }
            } catch(Exception ignored) { android.util.Log.w("SNMPInstaller","Could not persist diagnostic journal"); }
        }
    }
    static void begin(Context c,String action) {
        SharedPreferences prefs=c.getSharedPreferences("diagnostics",0);
        try { prefs.edit().putString("previousOperation",operation(c).toString()).apply(); } catch(Exception ignored) {}
        prefs.edit().putString("action",action).putString("stage","Запуск").putLong("startedAt",System.currentTimeMillis()).putLong("finishedAt",0).apply();
        event(c,"operation_start",action);
    }
    static void finish(Context c,String action) {
        c.getSharedPreferences("diagnostics",0).edit().putLong("finishedAt",System.currentTimeMillis()).apply();
        event(c,"operation_end",action);
    }
    static JSONObject operation(Context c) throws Exception {
        SharedPreferences prefs=c.getSharedPreferences("diagnostics",0);
        return new JSONObject().put("action",prefs.getString("action","")).put("stage",prefs.getString("stage",""))
            .put("startedAt",prefs.getLong("startedAt",0)).put("finishedAt",prefs.getLong("finishedAt",0))
            .put("note","finishedAt=0 means no completion was recorded; it may still be running or was interrupted. Android installation result is logged separately.");
    }
    static void progress(Context c,String text) {
        c.getSharedPreferences("diagnostics",0).edit().putString("stage",limit(text,2000)).apply();
        event(c,"status",text);
    }
    static JSONObject lastError(Context c) {
        try { return new JSONObject(c.getSharedPreferences("diagnostics",0).getString("lastError","{}")); }
        catch(Exception error) { return new JSONObject(); }
    }
    static String failure(Context c,String action,Throwable error) {
        return failure(c,action,DiagnosticError.classify(error),error);
    }
    static String failure(Context c,String action,DiagnosticError issue,Throwable error) {
        try {
            SharedPreferences prefs=c.getSharedPreferences("diagnostics",0);
            String stage=action.equals(prefs.getString("action","")) || action.startsWith("android-")?prefs.getString("stage","Не указан"):"Проверка действия";
            JSONObject detail=issue.json(action,stage,error);
            c.getSharedPreferences("diagnostics",0).edit().putString("lastError",detail.toString()).commit();
            event(c,"error",detail.toString());
        } catch(Exception ignored) { android.util.Log.e("SNMPInstaller","Could not persist error report",error); }
        return issue.code+" · "+issue.title+"\n"+issue.meaning+"\n\n"+issue.steps+"\n\nПодробности — «Последняя ошибка». Для разработчика создайте диагностический ZIP.";
    }
    static void crash(Context c,String thread,Throwable error) {
        String action=c.getSharedPreferences("diagnostics",0).getString("action","installer");
        failure(c,action,DiagnosticError.of("INSTALLER_CRASH","Установщик аварийно завершился",
            "Необработанная ошибка в потоке "+thread+". Android завершил процесс; уже подготовленные данные автоматически не удаляются.",
            "Откройте установщик заново, создайте диагностический ZIP и отправьте разработчику. Не очищайте данные установщика: там находятся ключ и копии миров."),error);
    }
    static File latest(Context c) {
        String name=c.getSharedPreferences("diagnostics",0).getString("latest","");
        if(!name.matches("snmp-diagnostic-[0-9a-f-]{36}\\.zip")) return null;
        File file=new File(folder(c),"reports/"+name); return file.isFile()?file:null;
    }
    interface Section { Object read() throws Exception; }
    static void section(JSONObject report,String key,Section source) throws Exception {
        try { report.put(key,source.read()); }
        catch(Exception error) { report.put(key,new JSONObject().put("unavailable",error.getClass().getSimpleName()+": "+error.getMessage())); }
    }
    static JSONObject packageInfo(PackageInfo info) throws Exception {
        JSONObject result=new JSONObject().put("installed",info!=null);
        if(info==null) return result;
        result.put("versionName",info.versionName).put("versionCode",info.getLongVersionCode()).put("firstInstallTime",info.firstInstallTime)
            .put("lastUpdateTime",info.lastUpdateTime).put("uid",info.applicationInfo.uid).put("certificateSha256",PatchEngine.certificate(info));
        JSONArray parts=new JSONArray(); parts.put(new JSONObject().put("name","base.apk").put("bytes",new File(info.applicationInfo.sourceDir).length()));
        if(info.applicationInfo.splitSourceDirs!=null) for(String path:info.applicationInfo.splitSourceDirs)
            parts.put(new JSONObject().put("name",new File(path).getName()).put("bytes",new File(path).length()));
        return result.put("apks",parts);
    }
    static JSONObject installedMod(Context c,PatchEngine engine) throws Exception {
        PackageInfo game=engine.installed(); if(game==null) return new JSONObject().put("present",false);
        try(ZipFile zip=new ZipFile(game.applicationInfo.sourceDir)) {
            // The bootstrap replaces libmain.so and preserves Unity's original next to it.
            ZipEntry entry=zip.getEntry(ApkPatcher.ORIGINAL)!=null?zip.getEntry(ApkPatcher.MAIN):zip.getEntry("lib/arm64-v8a/libsnmod.so");
            if(entry==null) return new JSONObject().put("present",false);
            byte[] bytes=ApkPatcher.readSmall(zip.getInputStream(entry),16*1024*1024);
            String sha=ApkPatcher.hex(MessageDigest.getInstance("SHA-256").digest(bytes));
            return new JSONObject().put("present",true).put("apkEntry",entry.getName()).put("sha256",sha).put("matchesInstallerPayload",sha.equals(engine.payload().getString("sha256")));
        }
    }
    static JSONObject snapshot(Context c,String id,String category,String description) throws Exception {
        PatchEngine engine=new PatchEngine(c);
        JSONObject report=new JSONObject().put("schema",1).put("id",id).put("capturedAt",System.currentTimeMillis())
            .put("timezone",TimeZone.getDefault().getID()).put("category",limit(category,100)).put("playerDescription",limit(description,4000))
            .put("lastError",lastError(c));
        section(report,"currentOperation",()->operation(c));
        section(report,"previousOperation",()->new JSONObject(c.getSharedPreferences("diagnostics",0).getString("previousOperation","{}")));
        section(report,"device",()->new JSONObject().put("manufacturer",Build.MANUFACTURER).put("model",Build.MODEL)
            .put("android",Build.VERSION.RELEASE).put("sdk",Build.VERSION.SDK_INT).put("securityPatch",Build.VERSION.SECURITY_PATCH)
            .put("buildDisplay",Build.DISPLAY).put("abis",new JSONArray(Arrays.asList(Build.SUPPORTED_ABIS)))
            .put("freeBytes",new StatFs(c.getFilesDir().getPath()).getAvailableBytes())
            .put("batteryOptimizationExempt",c.getSystemService(PowerManager.class).isIgnoringBatteryOptimizations(c.getPackageName())));
        section(report,"installer",()->packageInfo(c.getPackageManager().getPackageInfo(c.getPackageName(),PackageManager.GET_SIGNING_CERTIFICATES)));
        section(report,"payload",engine::payload);
        section(report,"game",()->packageInfo(engine.installed()));
        section(report,"retainedGame",()->packageInfo(engine.retained()));
        section(report,"installedMod",()->installedMod(c,engine));
        section(report,"shizuku",()->new JSONObject().put("ready",SaveBridge.readiness()==null).put("message",String.valueOf(SaveBridge.readiness())));
        section(report,"installation",()->{
            JSONObject plan=engine.plan();
            JSONObject result=new JSONObject().put("pendingSession",PatchService.pendingSession(c)).put("canInstallPackages",c.getPackageManager().canRequestPackageInstalls())
                .put("blocker",String.valueOf(engine.installBlocker(plan))).put("currentStatus",c.getSharedPreferences("status",0).getString("message",""));
            if(plan!=null) result.put("prepared",new JSONObject().put("full",plan.getBoolean("full")).put("buildId",plan.getString("buildId"))
                .put("version",plan.getString("version")).put("apks",plan.getJSONArray("apks")));
            return result;
        });
        section(report,"backup",()->{
            SaveManager manager=new SaveManager(c); JSONObject saved=manager.latest();
            JSONObject result=new JSONObject().put("present",saved!=null).put("restoreRequired",manager.needsRestore());
            if(saved!=null) result.put("files",saved.getInt("files")).put("bytes",saved.getLong("bytes")).put("createdAt",saved.getLong("createdAt"))
                .put("verifiedAtCreation",saved.optBoolean("verified")).put("contentsIncluded",false);
            return result;
        });
        section(report,"network",()->{
            ConnectivityManager manager=c.getSystemService(ConnectivityManager.class); Network network=manager.getActiveNetwork();
            NetworkCapabilities caps=manager.getNetworkCapabilities(network);
            return new JSONObject().put("active",network!=null).put("wifi",caps!=null && caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI))
                .put("cellular",caps!=null && caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)).put("vpn",caps!=null && caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN))
                .put("note","No SSID, MAC, serial or account identifiers collected");
        });
        return report;
    }
    static File create(Context c,String category,String description,boolean includeGame) throws Exception {
        String id=UUID.randomUUID().toString(); File root=folder(c),reports=new File(root,"reports");
        if(!reports.isDirectory() && !reports.mkdirs()) throw new IOException("Не удалось создать папку диагностики");
        JSONObject report=snapshot(c,id,category,description);
        File remote=new File(root,"game-"+id+".partial"),partial=new File(reports,"snmp-diagnostic-"+id+".partial"),finished=new File(reports,"snmp-diagnostic-"+id+".zip");
        boolean hasGame=false;
        try {
            if(includeGame) {
                try(SaveBridge bridge=new SaveBridge(c)) {
                    JSONObject captured=bridge.call(SaveAccessService.DIAGNOSTICS,remote,true);
                    report.put("gameCapture",captured); hasGame=true;
                } catch(Exception error) {
                    report.put("gameCapture",new JSONObject().put("unavailable",error.getClass().getSimpleName()+": "+error.getMessage())
                        .put("help","Запустите Shizuku, разрешите доступ и повторите сбор, пока баг виден. Частичный отчёт всё равно полезен."));
                }
            } else report.put("gameCapture",new JSONObject().put("unavailable","Игровые логи не запрошены (тест частичного отчёта)"));
            try(ZipOutputStream out=new ZipOutputStream(new FileOutputStream(partial))) {
                if(hasGame) {
                    try(ZipFile zip=new ZipFile(remote)) {
                        Enumeration<? extends ZipEntry> entries=zip.entries(); int count=0,total=0;
                        while(entries.hasMoreElements()) {
                            ZipEntry entry=entries.nextElement(); String name=entry.getName();
                            if(++count>10 || !allowedGameEntry(name)) throw new IOException("Неверный состав диагностического архива игры");
                            byte[] bytes=ApkPatcher.readSmall(zip.getInputStream(entry),DiagnosticLogs.LOG_LIMIT);
                            total+=bytes.length; if(total>24*1024*1024) throw new IOException("Диагностический архив игры слишком большой");
                            DiagnosticLogs.entry(out,name,bytes);
                            if(name.equals("game/log_findings.json")) report.put("gameLogAnalysis",new JSONObject(new String(bytes,StandardCharsets.UTF_8)));
                        }
                    }
                }
                DiagnosticLogs.entry(out,"report.json",report.toString(2).getBytes(StandardCharsets.UTF_8));
                DiagnosticLogs.entry(out,"README.txt",summary(report).getBytes(StandardCharsets.UTF_8));
                synchronized(LOCK) {
                    for(String name:new String[]{"installer.previous.jsonl","installer.jsonl"}) {
                        File file=new File(root,name);
                        if(file.isFile()) DiagnosticLogs.entry(out,name,DiagnosticLogs.tail(file,JOURNAL_LIMIT+32000));
                    }
                }
            }
            // Confirm every entry is readable before exposing the completed file to other apps.
            try(ZipFile verify=new ZipFile(partial)) {
                if(verify.getEntry("report.json")==null || verify.getEntry("README.txt")==null) throw new IOException("Диагностический ZIP не прошёл проверку");
                Enumeration<? extends ZipEntry> entries=verify.entries();
                while(entries.hasMoreElements()) { ZipEntry entry=entries.nextElement(); ApkPatcher.readSmall(verify.getInputStream(entry),DiagnosticLogs.LOG_LIMIT); }
            }
            if(!partial.renameTo(finished)) throw new IOException("Не удалось завершить диагностический ZIP");
            JSONObject gameCapture=report.optJSONObject("gameCapture");
            boolean logs=gameCapture!=null && gameCapture.optJSONArray("files")!=null && gameCapture.optJSONArray("files").length()>0;
            c.getSharedPreferences("diagnostics",0).edit().putString("latest",finished.getName()).putString("reportSummary",summary(report))
                .putString("reportCompleteness",logs?"Игровые файлы журналов получены. Их время и доступность logcat указаны в ZIP.":"Частичный отчёт: игровые файлы журналов не получены. Причина указана в ZIP; проверьте Shizuku.").commit();
            File[] old=reports.listFiles((dir,name)->name.matches("snmp-diagnostic-[0-9a-f-]{36}\\.zip"));
            if(old!=null) {
                Arrays.sort(old,Comparator.comparingLong(File::lastModified).reversed());
                int kept=1;
                for(File previous:old) if(!previous.equals(finished) && kept++>=3) previous.delete();
            }
            event(c,"diagnostic_ready",finished.getName()); return finished;
        } finally { remote.delete(); partial.delete(); }
    }
    static boolean allowedGameEntry(String name) {
        return name.equals("game/capture.json") || name.equals("game/log_findings.json") || name.matches("game/logcat_[0-9]+\\.txt")
            || name.equals("game/mod_log/logs/modloader.log") || name.equals("game/Player.log") || name.equals("game/Unity/Player.log");
    }
    static String summary(JSONObject report) {
        JSONObject error=report.optJSONObject("lastError"),game=report.optJSONObject("gameCapture");
        JSONObject installer=report.optJSONObject("installer"),payload=report.optJSONObject("payload");
        String logs=game==null || game.has("unavailable")?"НЕДОСТУПНЫ — "+(game==null?"нет данных":game.optString("unavailable"))
            : game.optJSONArray("files")==null || game.optJSONArray("files").length()==0?"Файлы журналов не получены; смотрите game/capture.json и logcat, если он доступен."
            :"Получены; состав, время изменения и обрезка указаны в game/capture.json. Это могут быть журналы прошлых запусков.";
        StringBuilder findings=new StringBuilder();
        JSONObject analysis=report.optJSONObject("gameLogAnalysis"),installed=report.optJSONObject("installedMod");
        if(installed!=null && installed.optBoolean("present") && !installed.optBoolean("matchesInstallerPayload"))
            findings.append("MOD_PAYLOAD_DIFFERS: мод в установленной игре отличается от мода в установщике. Проверьте, обновлялась ли сама игра после обновления установщика.\n");
        JSONArray items=analysis==null?null:analysis.optJSONArray("findings");
        if(items!=null) for(int i=0;i<items.length();i++) {
            JSONObject finding=items.optJSONObject(i); if(finding==null) continue;
            findings.append(finding.optString("code")).append(": ").append(finding.optString("meaning")).append("\n")
                .append(finding.optString("session")).append("\n").append(finding.optString("firstEvidence")).append("\n").append(finding.optString("lastEvidence")).append("\n\n");
        }
        return "SNMP · диагностический отчёт "+report.optString("id")+"\n"
            +"Время: "+new Date(report.optLong("capturedAt"))+" · "+report.optString("timezone")+"\n"
            +"Категория: "+report.optString("category")+"\nОписание игрока: "+report.optString("playerDescription")+"\n\n"
            +"Установщик: "+(installer==null?"нет данных":installer.optString("versionName"))+"\nМод в установщике: "+(payload==null?"нет данных":payload.optString("buildId"))+"\n"
            +"Последняя сохранённая ошибка (может относиться к прошлой операции):\n"+(error==null || !error.has("code")?"Не записана":DiagnosticError.readable(error))+"\n\n"
            +"Игровые логи: "+logs+"\n\n"
            +"Автоматические признаки (не окончательный диагноз):\n"+(findings.length()==0?"Не найдены или данных недостаточно.\n":findings.toString())+"\n"
            +"Разработчику: начните с report.json, lastError, installedMod и gameLogAnalysis; затем installer.jsonl и игровые журналы. "
            +"При проблеме с синхронизацией нужны отчёты ОБОИХ игроков, снятые примерно в одно время, и указание хоста. Автоматические находки — признаки, а не доказанная причина.\n\n"
            +"Сбор не останавливает игру и не меняет её файлы. В ZIP нет APK, сохранений, ключей подписи и журналов других приложений. "
            +"Журналы самой игры могут содержать локальные IP-адреса, имена слотов и введённые команды. Автоматической отправки нет: получателя выбирает игрок.\n";
    }
}
