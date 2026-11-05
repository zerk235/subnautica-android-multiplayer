package io.github.zerk235.snmp.installer;

import android.app.Instrumentation;
import android.content.*;
import android.content.pm.ProviderInfo;
import android.os.*;
import android.system.Os;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.zip.*;
import org.json.*;

/** Diagnostic-only tests: no game installation, force-stop, save reads or writes. */
public final class DiagnosticsTests extends Instrumentation {
    Bundle options;
    public void onCreate(Bundle args) { options=args; start(); }
    static void check(boolean ok,String message) { if(!ok) throw new AssertionError(message); }
    static void code(String message,String expected) { check(DiagnosticError.classify(new IOException(message)).code.equals(expected),"wrong code for "+message); }
    public void onStart() {
        Bundle result=new Bundle(); Context real=getTargetContext();
        try {
            PatchService.BUSY.set(true);
            startActivitySync(new Intent(real,MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            File dir=new File(real.getFilesDir(),"diagnostic-self-test");
            new PatchEngine(real).removeOwnTree(dir); check(dir.mkdir(),"test directory");
            Context context=new ContextWrapper(real) {
                public File getFilesDir() { return dir; }
                public Context getApplicationContext() { return this; }
                public SharedPreferences getSharedPreferences(String name,int mode) { return real.getSharedPreferences("diagtest-"+name,mode); }
            };
            code("open failed: ENOSPC","STORAGE_FULL");
            code("Subnautica также установлена в профиле Android 11","GAME_OTHER_PROFILE");
            code("Разрешите установщику доступ через Shizuku","SHIZUKU_PERMISSION");
            code("Shizuku остановлен. Запустите его снова","SHIZUKU_UNAVAILABLE");
            code("FileNotFoundException: EACCES (Permission denied)","ACCESS_DENIED");
            code("Подпись установленной игры отличается","SIGNATURE_CONFLICT");
            code("Поддерживается игра 1.22.83315; установлена другая","GAME_VERSION");
            code("Сначала установите собственную копию Subnautica","GAME_NOT_FOUND");
            code("Миры изменились после копирования","BACKUP_OUTDATED");
            code("Резервная копия повреждена. Игру не удаляйте","BACKUP_INVALID");
            code("Мод обновился. Нажмите подготовить мод","PREPARED_OUTDATED");
            code("Something unexpected","OPERATION_FAILED");
            check(DiagnosticError.installation(android.content.pm.PackageInstaller.STATUS_FAILURE_STORAGE,"unknown").code.equals("STORAGE_FULL"),"Android result mapping");
            Diagnostics.begin(context,"prepare"); Diagnostics.progress(context,"Подпись 2/2");
            Diagnostics.failure(context,"prepare",new IOException("write failed",new IOException("ENOSPC")));
            JSONObject error=Diagnostics.lastError(context);
            check(error.getString("code").equals("STORAGE_FULL") && error.getString("stage").equals("Подпись 2/2") && error.getString("detail").contains("Caused by"),"persistent error chain/stage");
            Diagnostics.crash(context,"main",new IllegalStateException("synthetic Java crash"));
            check(Diagnostics.lastError(context).getString("code").equals("INSTALLER_CRASH") && Diagnostics.lastError(context).getString("stage").equals("Подпись 2/2"),"Java crash not persisted");
            Diagnostics.failure(context,"prepare",new IOException("ENOSPC"));

            File game=new File(dir,"fake-game"); game.mkdir(); File logs=new File(game,DiagnosticLogs.PATHS[0]); logs.getParentFile().mkdirs();
            StringBuilder trace=new StringBuilder("=== log opened 2026-11-05 10:41:38 ===\n");
            for(int i=0;i<7;i++) trace.append("10:48:").append(i*5).append(" pump alive: 100 loops, peers=1, local_valid=1, rx hello=").append(100+i).append(" state=6229 other=0\n");
            check(DiagnosticLogs.analyze(trace.toString()).getJSONArray("findings").length()==1,"pose stall not detected");
            check(DiagnosticLogs.analyze(trace.toString().replace("state=6229","state=0")).getJSONArray("findings").length()==0,"initial world transfer misclassified");
            check(DiagnosticLogs.analyze(trace.toString().replace("peers=1","peers=0")).getJSONArray("findings").length()==0,"no-peer misclassified");
            check(DiagnosticLogs.analyze(trace.toString().replaceAll("hello=10[0-9]","hello=100")).getJSONArray("findings").length()==0,"total disconnection called one-way pose stall");
            PatchEngine.write(logs,trace.toString().getBytes(StandardCharsets.UTF_8));
            File saves=new File(game,"SavedGames/slot/scene.bin"); saves.getParentFile().mkdirs(); PatchEngine.write(saves,"NEVER_INCLUDE_SAVES".getBytes(StandardCharsets.UTF_8));
            File remote=new File(dir,"capture.zip");
            DiagnosticLogs.capture(game,new FileOutputStream(remote),false,0);
            try(ZipFile zip=new ZipFile(remote)) {
                check(zip.getEntry("game/mod_log/logs/modloader.log")!=null && zip.getEntry("game/log_findings.json")!=null,"game log entries missing");
                Enumeration<? extends ZipEntry> entries=zip.entries(); while(entries.hasMoreElements()) check(!entries.nextElement().getName().contains("SavedGames"),"saves leaked");
            }
            File external=new File(dir,"secret"); PatchEngine.write(external,"SECRET".getBytes(StandardCharsets.UTF_8));
            check(logs.delete(),"remove synthetic log"); Os.symlink(external.getAbsolutePath(),logs.getAbsolutePath());
            DiagnosticLogs.capture(game,new FileOutputStream(remote),false,0);
            try(ZipFile zip=new ZipFile(remote)) { check(zip.getEntry("game/mod_log/logs/modloader.log")==null,"symlink log escaped allowed scope"); }
            logs.delete();
            File large=new File(dir,"large.log");
            try(FileOutputStream out=new FileOutputStream(large)) { byte[] bytes=new byte[4096]; Arrays.fill(bytes,(byte)'A'); out.write(bytes); out.write("TAIL".getBytes(StandardCharsets.UTF_8)); }
            check(new String(DiagnosticLogs.tail(large,4),StandardCharsets.UTF_8).equals("TAIL"),"log tail cap");

            File report=Diagnostics.create(context,"test","Read-only partial report",false);
            check(Diagnostics.latest(context).equals(report),"latest report not persisted");
            try(ZipFile zip=new ZipFile(report)) {
                JSONObject json=new JSONObject(new String(ApkPatcher.readSmall(zip.getInputStream(zip.getEntry("report.json")),1024*1024),StandardCharsets.UTF_8));
                check(json.getJSONObject("gameCapture").has("unavailable") && json.getJSONObject("lastError").getString("code").equals("STORAGE_FULL"),"partial capture or last error lost");
                check(zip.getEntry("README.txt")!=null && zip.getEntry("installer.jsonl")!=null,"support report incomplete");
                Enumeration<? extends ZipEntry> entries=zip.entries(); while(entries.hasMoreElements()) {
                    String name=entries.nextElement().getName(); check(!name.endsWith(".apk") && !name.contains("SavedGames") && !name.contains("keystore"),"unexpected report data");
                }
            }
            ReportProvider provider=new ReportProvider(); ProviderInfo info=new ProviderInfo(); info.authority=context.getPackageName()+".reports"; info.exported=false;
            provider.attachInfo(context,info);
            try(ParcelFileDescriptor fd=provider.openFile(ReportProvider.uri(context,report),"r")) { check(fd.getStatSize()==report.length(),"share read"); }
            boolean denied=false; try { provider.openFile(ReportProvider.uri(context,report),"w"); } catch(FileNotFoundException expected) { denied=true; } check(denied,"share allowed write");
            denied=false; try { ReportProvider.file(context,"../secret"); } catch(FileNotFoundException expected) { denied=true; } check(denied,"provider path traversal");
            check(!Diagnostics.allowedGameEntry("game/../../SavedGames/data") && !Diagnostics.allowedGameEntry("game/SavedGames/scene.bin"),"archive entry allowlist");
            for(int i=0;i<4;i++) Diagnostics.create(context,"test","retention",false);
            check(new File(Diagnostics.folder(context),"reports").listFiles((parent,name)->name.endsWith(".zip")).length==3,"report retention not bounded");
            if(options!=null && options.getString("mode","").equals("game")) {
                File captured=Diagnostics.create(real,"Проверка диагностики","Сбор реальных игровых логов без остановки игры; тест установщика",true);
                result.putString("report",captured.getAbsolutePath());
                try(ZipFile zip=new ZipFile(captured)) {
                    JSONObject json=new JSONObject(new String(ApkPatcher.readSmall(zip.getInputStream(zip.getEntry("report.json")),1024*1024),StandardCharsets.UTF_8));
                    check(!json.getJSONObject("gameCapture").has("unavailable"),"real Shizuku capture unavailable");
                    check(json.getJSONObject("installedMod").getBoolean("present") && json.getJSONObject("installedMod").getBoolean("matchesInstallerPayload"),"installed bootstrap library not identified or differs from payload");
                    check(zip.getEntry("game/mod_log/logs/modloader.log")!=null,"real game log missing");
                    result.putString("capture",json.getJSONObject("gameCapture").toString());
                }
            }
            result.putString("result","PASS: error codes, stage/cause persistence, historical pose-stall evidence, partial/full report, bounded read, save exclusion, symlink/path/write rejection, provider and retention");
            finish(-1,result);
        } catch(Throwable error) {
            android.util.Log.e("SNMPInstallerTest","Diagnostic test failed",error); result.putString("error",android.util.Log.getStackTraceString(error)); finish(0,result);
        } finally { PatchService.BUSY.set(false); }
    }
}
