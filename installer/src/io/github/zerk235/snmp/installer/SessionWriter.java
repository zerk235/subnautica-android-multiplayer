package io.github.zerk235.snmp.installer;

import android.content.Context;
import android.content.pm.PackageInstaller;
import java.io.*;
import java.util.List;

/** Shared by the production install path and disposable fixture tests. */
final class SessionWriter {
    static int stage(Context context, String packageName, boolean full, List<File> files,
                     ApkPatcher.Progress progress) throws Exception {
        PackageInstaller installer=context.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params=new PackageInstaller.SessionParams(full
            ? PackageInstaller.SessionParams.MODE_FULL_INSTALL : PackageInstaller.SessionParams.MODE_INHERIT_EXISTING);
        params.setAppPackageName(packageName);
        long total=0; for(File file:files) total=Math.addExact(total,file.length()); params.setSize(total);
        if(new android.os.StatFs(context.getFilesDir().getPath()).getAvailableBytes()<total+64L*1024*1024)
            throw new IOException("Для установки Android нужно ещё примерно "+((total+64L*1024*1024)/(1024*1024))+" МиБ свободной памяти");
        int id=installer.createSession(params); boolean success=false;
        try(PackageInstaller.Session session=installer.openSession(id)) {
            long done=0; byte[] buffer=new byte[1024*1024];
            for(File file:files) {
                try(InputStream in=new FileInputStream(file); OutputStream out=session.openWrite(file.getName(),0,file.length())) {
                    int n; while((n=in.read(buffer))!=-1) { out.write(buffer,0,n); done+=n; progress.update("Передача Android",done,total); }
                    session.fsync(out);
                }
            }
            success=true; return id;
        } finally { if(!success) installer.abandonSession(id); }
    }
}
