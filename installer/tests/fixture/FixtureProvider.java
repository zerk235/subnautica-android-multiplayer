package io.github.zerk235.snmp.fixture;

import android.content.*;
import android.database.*;
import android.net.Uri;
import java.io.*;
import java.util.UUID;

/** Only packaged in test fixtures; a private data marker survives an APK update. */
public final class FixtureProvider extends ContentProvider {
    public boolean onCreate() {
        File file=new File(getContext().getFilesDir(),"update-marker");
        if(!file.exists()) try(FileOutputStream out=new FileOutputStream(file)) {
            out.write(UUID.randomUUID().toString().getBytes("UTF-8")); out.getFD().sync();
        } catch(IOException e){throw new IllegalStateException(e);}
        return true;
    }
    public Cursor query(Uri uri,String[] projection,String selection,String[] args,String order) {
        try(BufferedReader in=new BufferedReader(new InputStreamReader(new FileInputStream(new File(getContext().getFilesDir(),"update-marker")),"UTF-8"))) {
            MatrixCursor cursor=new MatrixCursor(new String[]{"marker"}); cursor.addRow(new Object[]{in.readLine()}); return cursor;
        } catch(IOException e){throw new IllegalStateException(e);}
    }
    public String getType(Uri uri){return "vnd.android.cursor.item/snmp-fixture";}
    public Uri insert(Uri uri,ContentValues values){throw new UnsupportedOperationException();}
    public int delete(Uri uri,String where,String[] args){throw new UnsupportedOperationException();}
    public int update(Uri uri,ContentValues values,String where,String[] args){throw new UnsupportedOperationException();}
}
