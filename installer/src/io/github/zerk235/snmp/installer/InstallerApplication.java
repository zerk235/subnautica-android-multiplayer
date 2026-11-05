package io.github.zerk235.snmp.installer;

import android.app.Application;

/** Persist Java crashes before delegating to Android's normal crash handler. */
public final class InstallerApplication extends Application {
    public void onCreate() {
        super.onCreate();
        final Thread.UncaughtExceptionHandler previous=Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread,error)->{
            try { Diagnostics.crash(this,thread.getName(),error); } catch(Throwable ignored) {}
            finally {
                if(previous!=null) previous.uncaughtException(thread,error);
                else { android.os.Process.killProcess(android.os.Process.myPid()); System.exit(10); }
            }
        });
    }
}
