package io.github.zerk235.snmp.installer;

import android.app.Instrumentation;
import android.app.PendingIntent;
import android.content.*;
import android.content.pm.*;
import android.content.Context;
import android.os.Bundle;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.zip.*;
import java.util.concurrent.*;

/** Registered only in --test builds. Never installs or removes the game. */
public final class InstallerTests extends Instrumentation {
    Bundle options;
    public void onCreate(Bundle arguments) { options = arguments; start(); }
    static void check(boolean value, String message) { if (!value) throw new AssertionError(message); }
    static byte[] elf(String text) {
        byte[] bytes = new byte[64 + text.length()]; bytes[0]=127; bytes[1]='E'; bytes[2]='L'; bytes[3]='F'; bytes[4]=2; bytes[5]=1;
        bytes[16]=3; bytes[18]=(byte)183; System.arraycopy(text.getBytes(StandardCharsets.US_ASCII),0,bytes,64,text.length()); return bytes;
    }
    public void onStart() {
        Bundle result = new Bundle();
        try {
            Context context = getTargetContext();
            PatchService.BUSY.set(true);
            PatchService.status(context,"Идёт автоматическая проверка установщика. Игра не изменяется.");
            startActivitySync(new Intent(context,MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            File dir = new File(context.getFilesDir(), "self-test");
            new PatchEngine(context).removeOwnTree(dir); check(dir.mkdir(),"mkdir");
            File input = new File(dir,"input.apk"), output = new File(dir,"patched.apk");
            byte[] original=elf("original"), mod=elf("mod libmain_orig.so");
            try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(input))) {
                zip.putNextEntry(new ZipEntry(ApkPatcher.MAIN)); zip.write(original); zip.closeEntry();
                zip.putNextEntry(new ZipEntry("assets/fake")); zip.write(new byte[1024*1024]); zip.closeEntry();
                zip.putNextEntry(new ZipEntry("META-INF/CERT.RSA")); zip.write(new byte[12]); zip.closeEntry();
            }
            String before=ApkPatcher.sha256(input);
            ApkPatcher.Progress progress=(stage,done,total)->{};
            ApkPatcher.rewrite(input,output,mod,progress);
            check(before.equals(ApkPatcher.sha256(input)),"input changed");
            try (ZipFile zip=new ZipFile(output)) {
                check(Arrays.equals(mod,ApkPatcher.readSmall(zip.getInputStream(zip.getEntry(ApkPatcher.MAIN)),4096)),"mod");
                check(Arrays.equals(original,ApkPatcher.readSmall(zip.getInputStream(zip.getEntry(ApkPatcher.ORIGINAL)),4096)),"original");
                check(zip.getEntry("META-INF/CERT.RSA")==null,"old signature");
            }
            File again=new File(dir,"again.apk"); ApkPatcher.rewrite(output,again,mod,progress);
            try(ZipFile zip=new ZipFile(again)) { check(Arrays.equals(original,ApkPatcher.readSmall(zip.getInputStream(zip.getEntry(ApkPatcher.ORIGINAL)),4096)),"nested bootstrap"); }
            boolean refused=false; try { ApkPatcher.rewrite(input,output,mod,progress); } catch(IOException expected) { refused=true; } check(refused,"overwrite");
            refused=false; try { ApkPatcher.requireElf(new byte[64]); } catch(IOException expected) { refused=true; } check(refused,"ELF guard");
            LocalSigner signer=new LocalSigner(); String cert=signer.fingerprint(); check(cert.equals(new LocalSigner().fingerprint()),"key persistence");
            // Sign a genuine tiny Android APK (this installer) to exercise apksig on ART.
            File own=new File(context.getApplicationInfo().sourceDir), unsigned=new File(dir,"own-unsigned.apk"), signed=new File(dir,"own-signed.apk");
            ApkIdentity ownIdentity=ApkIdentity.read(own);
            check(context.getPackageName().equals(ownIdentity.packageName) && ownIdentity.versionCode==1 && ownIdentity.split==null,"binary manifest");
            refused=false; try { ApkIdentity.parse(new byte[64]); } catch(IOException expected) { refused=true; } check(refused,"manifest bounds");
            ApkPatcher.rewrite(own,unsigned,null,progress); signer.sign(unsigned,signed); signer.verify(signed);
            if (options != null && "sessions".equals(options.getString("mode",""))) testSessions(context,dir,signer,progress);
            String mode = options == null ? "" : options.getString("mode","");
            if ("prepare-game".equals(mode)) {
                new PatchEngine(context).prepare((stage,done,total)->{
                    if(done==0) android.util.Log.i("SNMPInstallerTest",stage);
                });
                PatchEngine engine=new PatchEngine(context); engine.verifyPlan(engine.plan(),progress);
                check(engine.installBlocker(engine.plan())!=null,"test phone must remain signed by existing private key");
                PatchService.status(context,"Проверка на телефоне прошла. Подготовленные APK подписаны и проверены. Игра и сохранения не менялись.");
            }
            new PatchEngine(context).removeOwnTree(dir);
            result.putString("result","PASS: streaming patch, original preservation/repatch, overwrite/ELF guards, manifest parsing, persistent Android Keystore, on-device APK signing/verification"
                + ("prepare-game".equals(mode)?", real game full preparation and signature conflict guard":"")
                + ("sessions".equals(mode)?", fresh Android split session, base-only update, split SHA256 and private app-data preservation":""));
            PatchService.status(context,"Автоматические проверки установщика прошли. Игра и её сохранения не менялись.");
            PatchService.BUSY.set(false);
            finish(-1,result);
        } catch(Throwable e) {
            android.util.Log.e("SNMPInstallerTest","FAIL",e);
            PatchService.status(getTargetContext(),"Проверка не прошла: "+e.getMessage());
            PatchService.BUSY.set(false);
            result.putString("failure",android.util.Log.getStackTraceString(e)); finish(0,result);
        }
    }

    void testSessions(Context context,File dir,LocalSigner signer,ApkPatcher.Progress progress) throws Exception {
        final String fixture="io.github.zerk235.snmp.fixture";
        File base=new File(dir,"base.apk"),split=new File(dir,"split.apk");
        for(String name:new String[]{"base","split"}) {
            File raw=new File(dir,name+"-input.apk");
            try(InputStream in=context.getAssets().open("fixture-"+name+".apk"); OutputStream out=new FileOutputStream(raw)) {
                byte[] b=new byte[65536]; int n; while((n=in.read(b))!=-1) out.write(b,0,n);
            }
            ApkIdentity info=ApkIdentity.read(raw);
            check(fixture.equals(info.packageName) && info.versionCode==1,"fixture identity");
            check(name.equals("split") ? "demo_assets".equals(info.split) : info.split==null,"split identity");
            signer.sign(raw,name.equals("base")?base:split);
        }
        getUiAutomation().adoptShellPermissionIdentity("android.permission.INSTALL_PACKAGES","android.permission.DELETE_PACKAGES");
        try {
            // Only this fixed disposable package is installed; never the game.
            check(commit(context,fixture,true,Arrays.asList(base,split),progress)==PackageInstaller.STATUS_SUCCESS,"fresh split session");
            PackageInfo first=context.getPackageManager().getPackageInfo(fixture,0);
            check(first.splitNames!=null && Arrays.asList(first.splitNames).contains("demo_assets"),"initial split missing");
            context.startActivity(new Intent().setClassName(fixture,fixture+".FixtureActivity").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            String dataBefore=fixtureMarker(context);
            File sentinel=new File(context.getPackageManager().getApplicationInfo(fixture,0).splitSourceDirs[0]);
            String before=ApkPatcher.sha256(sentinel);
            check(commit(context,fixture,false,Collections.singletonList(base),progress)==PackageInstaller.STATUS_SUCCESS,"partial base session");
            PackageInfo next=context.getPackageManager().getPackageInfo(fixture,0);
            check(next.splitNames!=null && Arrays.asList(next.splitNames).contains("demo_assets"),"partial update lost split");
            context.startActivity(new Intent().setClassName(fixture,fixture+".FixtureActivity").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            File retained=new File(context.getPackageManager().getApplicationInfo(fixture,0).splitSourceDirs[0]);
            check(before.equals(ApkPatcher.sha256(retained)),"resource split changed");
            check(dataBefore.equals(fixtureMarker(context)),"private app data changed");
            android.util.Log.i("SNMPInstallerTest","PASS real Android sessions: fresh split install, base-only update, retained resource split SHA256 and app data");
        } finally { getUiAutomation().dropShellPermissionIdentity(); }
    }

    String fixtureMarker(Context context) {
        long deadline=android.os.SystemClock.elapsedRealtime()+3000;
        do {
            try(android.database.Cursor cursor=context.getContentResolver().query(android.net.Uri.parse("content://io.github.zerk235.snmp.fixture.state/marker"),null,null,null,null)) {
                if(cursor!=null && cursor.moveToFirst()) {
                    String marker=cursor.getString(0); check(marker!=null && marker.length()>20,"empty marker"); return marker;
                }
            }
            android.os.SystemClock.sleep(100);
        } while(android.os.SystemClock.elapsedRealtime()<deadline);
        throw new AssertionError("fixture provider not available after installation");
    }

    int commit(Context context,String pkg,boolean full,List<File> files,ApkPatcher.Progress progress) throws Exception {
        String action=context.getPackageName()+".FIXTURE_RESULT."+System.nanoTime();
        CountDownLatch latch=new CountDownLatch(1); int[] status={-999};
        BroadcastReceiver receiver=new BroadcastReceiver(){ public void onReceive(Context c,Intent intent) {
            status[0]=intent.getIntExtra(PackageInstaller.EXTRA_STATUS,-999); latch.countDown();
        }};
        context.registerReceiver(receiver,new IntentFilter(action));
        PackageInstaller installer=context.getPackageManager().getPackageInstaller();
        int id=-1;
        try {
            id=SessionWriter.stage(context,pkg,full,files,progress);
            Intent callback=new Intent(action).setPackage(context.getPackageName());
            int mutable=android.os.Build.VERSION.SDK_INT>=31?0x02000000:0;
            PendingIntent result=PendingIntent.getBroadcast(context,id,callback,PendingIntent.FLAG_UPDATE_CURRENT|mutable);
            try(PackageInstaller.Session session=installer.openSession(id)){session.commit(result.getIntentSender());}
            check(latch.await(60,TimeUnit.SECONDS),"session callback timeout");
            return status[0];
        } finally {
            context.unregisterReceiver(receiver);
            if(id>=0 && installer.getSessionInfo(id)!=null) installer.abandonSession(id);
        }
    }
}
