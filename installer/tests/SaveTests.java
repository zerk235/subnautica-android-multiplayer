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
        try { SaveArchive.restore(new FileInputStream(archive),target,staging->{}); } catch(Exception expected) { refused=true; }
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
            boolean denied=false;
            try {
                SaveArchive.restore(new FileInputStream(zip),target,staging->{ throw new IOException("test access failure"); });
            } catch(IOException expected) { denied=expected.getMessage().equals("test access failure"); }
            check(denied && new File(target,"old-world/data").isFile(),"access failure replaced existing worlds");
            JSONObject report=SaveArchive.restore(new FileInputStream(zip),target,
                staging->SaveArchive.prepareGameAccess(staging,-1,-1,false));
            check(report.getInt("files")==4 && SaveArchive.same(before,SaveArchive.inventory(target)),"restored data");
            check((Os.stat(target.getAbsolutePath()).st_mode & 0777)==0777,"shell restore directory mode");
            check((Os.stat(new File(target,"slotA/CellsCache/cell.zip").getAbsolutePath()).st_mode & 0777)==0666,"shell restore file mode");
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
            SaveAccessService.checkProfiles(" User 0: installed=true stopped=false\n User 11: installed=false\n User 0:\n",0);
            for(String dump:new String[]{"permission denied"," User 0: installed=false\n",
                    " User 0: installed=true\n User 0: installed=false\n"," User 11: installed=true\n"}) {
                boolean blocked=false;
                try { SaveAccessService.checkProfiles(dump,0); } catch(IOException expected) { blocked=true; }
                check(blocked,"unknown/absent/ambiguous current profile permitted deletion: "+dump);
            }
            GameProfiles recovery=new GameProfiles(" User 0: installed=false\n User 11: installed=true\n");
            recovery.requireRecovery(0);
            check(!recovery.json(0).getBoolean("currentInstalled") && recovery.installed.contains(11),"retained clone recovery state");
            boolean already=false;
            try { new GameProfiles(" User 0: installed=true\n").requireRecovery(0); } catch(IOException expected) { already=true; }
            check(already,"recovery must refuse an installed current profile");
            android.content.pm.PackageInfo fake=new android.content.pm.PackageInfo();
            fake.applicationInfo=new android.content.pm.ApplicationInfo(); fake.applicationInfo.uid=10430;
            check(!PatchEngine.installedHere(fake,0),"retained package treated as installed");
            fake.applicationInfo.flags=android.content.pm.ApplicationInfo.FLAG_INSTALLED;
            check(PatchEngine.installedHere(fake,0) && !PatchEngine.installedHere(fake,11),"cross-profile UID accepted");
            String mode=options==null?"core":options.getString("mode","core");
            if("profile-readonly".equals(mode)) {
                PatchEngine engine=new PatchEngine(context);
                android.content.pm.PackageInfo installed=engine.installed();
                check(installed!=null,"current game unavailable");
                JSONObject testPlan=new JSONObject().put("cert",PatchEngine.certificate(installed)).put("versionCode",installed.getLongVersionCode());
                engine.verifyInstalled(testPlan);
                testPlan.put("cert","wrong");
                boolean mismatch=false;
                try { engine.verifyInstalled(testPlan); } catch(IOException expected) { mismatch=true; }
                check(mismatch,"installation success accepted a wrong signer");
                boolean hasClone;
                try(SaveBridge bridge=new SaveBridge(context)) {
                    JSONObject state=bridge.call(SaveAccessService.PROFILE_STATE,null,false);
                    check(state.getBoolean("currentInstalled"),"remote current installation missing");
                    result.putString("profiles",state.toString());
                    hasClone=state.getJSONArray("installedUsers").length()>1;
                    boolean blocked=false;
                    try { bridge.call(SaveAccessService.RECOVER_GAME,null,false); }
                    catch(IOException expected) { blocked=expected.getMessage().contains("уже установлена"); }
                    check(blocked,"recovery did not refuse current installed game before any command");
                }
                if(hasClone && !PatchEngine.certificate(installed).equals(new LocalSigner().fingerprint())) {
                    File preparedPlan=new File(engine.prepared,"plan.json");
                    String beforePlan=preparedPlan.isFile()?ApkPatcher.sha256(preparedPlan):"absent";
                    boolean blocked=false;
                    try { engine.prepare((stage,done,total)->{}); }
                    catch(IOException expected) { blocked=expected.getMessage().contains("профиле Android"); result.putString("preparationRefusal",expected.getMessage()); }
                    check(blocked,"clone conflict was not refused before APK preparation");
                    check(beforePlan.equals(preparedPlan.isFile()?ApkPatcher.sha256(preparedPlan):"absent"),"clone check replaced prepared files");
                    result.putString("preparationGuard","PASS: clone + different signing key refused before modifying prepared package");
                }
                for(int i=0;i<5;i++) try(SaveBridge bridge=new SaveBridge(context)) {
                    check(bridge.call(SaveAccessService.PROFILE_STATE,null,false).getBoolean("currentInstalled"),"consecutive Shizuku bind failed");
                }
                result.putString("repeatBridge","PASS: five immediate close/rebind cycles");
                result.putString("gameCertificate",PatchEngine.certificate(installed));
                result.putString("localCertificate",new LocalSigner().fingerprint());
            } else if("shizuku".equals(mode)) {
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
