package io.github.zerk235.snmp.installer;

import android.app.Instrumentation;
import android.content.*;
import android.os.Bundle;
import android.system.Os;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.zip.*;
import org.json.JSONObject;

/** Tests keep game data untouched unless explicitly invoked with restore-game. */
public final class SaveTests extends Instrumentation {
    Bundle options;
    public void onCreate(Bundle args) { options=args; start(); }
    static void check(boolean value,String message) { if(!value) throw new AssertionError(message); }
    static void put(File file,String data) throws Exception { file.getParentFile().mkdirs(); PatchEngine.write(file,data.getBytes(StandardCharsets.UTF_8)); }
    static void reject(File archive,File target) throws Exception {
        boolean refused=false;
        try { SaveArchive.restore(new FileInputStream(archive),target); } catch(Exception expected) { refused=true; }
        check(refused,"unsafe archive accepted");
    }
    public void onStart() {
        Bundle result=new Bundle(); Context context=getTargetContext();
        try {
            PatchService.BUSY.set(true);
            startActivitySync(new Intent(context,MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            File dir=new File(context.getFilesDir(),"save-self-test");
            new PatchEngine(context).removeOwnTree(dir); check(dir.mkdir(),"test dir");
            File source=new File(dir,"source"); source.mkdir();
            put(new File(source,"slotA/gameinfo.json"),"world one");
            put(new File(source,"slotA/CellsCache/cell.zip"),"world cells");
            put(new File(source,".previous/scene.bin"),"hidden backup");
            put(new File(source,"options/options.json"),"options");
            JSONObject before=SaveArchive.inventory(source); File zip=new File(dir,"backup.zip");
            SaveArchive.create(source,new FileOutputStream(zip));
            check(SaveArchive.same(before,SaveArchive.inspect(new FileInputStream(zip),null)),"backup inventory");
            File parent=new File(dir,"target"); parent.mkdir(); File target=new File(parent,"SavedGames");
            put(new File(target,"old-world/data"),"old progress");
            JSONObject report=SaveArchive.restore(new FileInputStream(zip),target);
            check(report.getInt("files")==4 && SaveArchive.same(before,SaveArchive.inventory(target)),"restored data");
            check(new File(parent,report.getString("previous")+"/old-world/data").isFile(),"previous worlds not retained");
            String prior=ApkPatcher.sha256(new File(target,"slotA/gameinfo.json"));
            File corrupt=new File(dir,"corrupt.zip");
            try(ZipFile in=new ZipFile(zip); ZipOutputStream out=new ZipOutputStream(new FileOutputStream(corrupt))) {
                java.util.Enumeration<? extends ZipEntry> entries=in.entries();
                while(entries.hasMoreElements()) {
                    ZipEntry entry=entries.nextElement(); out.putNextEntry(new ZipEntry(entry.getName()));
                    out.write(entry.getName().endsWith("gameinfo.json")?"altered".getBytes(StandardCharsets.UTF_8):ApkPatcher.readSmall(in.getInputStream(entry),1024*1024));
                    out.closeEntry();
                }
            }
            reject(corrupt,target); check(prior.equals(ApkPatcher.sha256(new File(target,"slotA/gameinfo.json"))),"corruption changed existing worlds");
            SaveManager manager=new SaveManager(context,new File(dir,"managed-backups"));
            JSONObject imported=manager.importArchive(new FileInputStream(zip)); manager.verify();
            boolean badImport=false;
            try { manager.importArchive(new FileInputStream(corrupt)); } catch(Exception expected) { badImport=true; }
            check(badImport && manager.latest().getString("archive").equals(imported.getString("archive")),"bad import replaced verified backup");
            File traversal=new File(dir,"traversal.zip");
            try(ZipOutputStream out=new ZipOutputStream(new FileOutputStream(traversal))) {
                out.putNextEntry(new ZipEntry("SavedGames/../../escaped")); out.write(1); out.closeEntry();
            }
            reject(traversal,target); check(!new File(dir,"escaped").exists(),"path traversal");
            Os.symlink(new File(source,"slotA/gameinfo.json").getAbsolutePath(),new File(source,"link").getAbsolutePath());
            boolean refused=false; try { SaveArchive.inventory(source); } catch(Exception expected) { refused=true; }
            check(refused,"symlink accepted"); new File(source,"link").delete();
            SaveAccessService.checkProfiles(" User 0: installed=true stopped=false\n",0);
            boolean cloned=false;
            try { SaveAccessService.checkProfiles(" User 0: installed=true\n User 11: installed=true\n",0); } catch(IOException expected) { cloned=true; }
            check(cloned,"clone signature conflict not blocked");
            String mode=options==null?"core":options.getString("mode","core");
            if("shizuku".equals(mode)) {
                JSONObject backup=new SaveManager(context).backup(); new SaveManager(context).verify();
                try(SaveBridge bridge=new SaveBridge(context)) {
                    JSONObject checked=bridge.call(SaveAccessService.CHECK,new SaveManager(context).archive(backup),false);
                    check(checked.getBoolean("verified"),"remote match");
                    result.putString("backup",backup.toString());
                }
            } else if("clone-guard".equals(mode)) {
                boolean blocked=false;
                try(SaveBridge bridge=new SaveBridge(context)) {
                    JSONObject copy=new SaveManager(context).verify();
                    bridge.call(SaveAccessService.CHECK_PROFILES,new SaveManager(context).archive(copy),false);
                } catch(IOException expected) {
                    blocked=expected.getMessage().contains("11"); result.putString("cloneGuard",expected.getMessage());
                }
                check(blocked,"actual phone profile 11 not detected");
            } else if("restore-game".equals(mode)) {
                JSONObject restored=new SaveManager(context).restore(); result.putString("restored",restored.toString());
            }
            new PatchEngine(context).removeOwnTree(dir);
            PatchService.BUSY.set(false);
            result.putString("result","PASS: stable archive, nested/hidden worlds, SHA256, retained previous worlds, corrupted archive rollback, verified import and failed-import preservation, traversal refusal, symlink refusal; mode="+mode);
            finish(-1,result);
        } catch(Throwable error) {
            PatchService.BUSY.set(false); android.util.Log.e("SNMPSaveTest","FAIL",error);
            result.putString("failure",android.util.Log.getStackTraceString(error)); finish(0,result);
        }
    }
}
