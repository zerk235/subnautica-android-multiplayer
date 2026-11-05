package io.github.zerk235.snmp.installer;

import android.content.*;
import android.database.*;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import java.io.*;

/** Read-only, URI-granted access to completed diagnostic ZIPs, never other private files. */
public final class ReportProvider extends ContentProvider {
    public boolean onCreate() { return true; }
    static Uri uri(Context context,File file) { return new Uri.Builder().scheme("content").authority(context.getPackageName()+".reports").appendPath(file.getName()).build(); }
    File resolve(Uri uri) throws FileNotFoundException {
        if(uri.getPathSegments().size()!=1 || uri.getQuery()!=null || uri.getFragment()!=null) throw new FileNotFoundException("Invalid report URI");
        return file(getContext(),uri.getLastPathSegment());
    }
    static File file(Context context,String name) throws FileNotFoundException {
        if(name==null || !name.matches("snmp-diagnostic-[0-9a-f-]{36}\\.zip")) throw new FileNotFoundException("Invalid report name");
        File root=new File(context.getFilesDir(),"diagnostics/reports"),file=new File(root,name);
        try {
            if(!file.isFile() || !file.getCanonicalFile().getParentFile().equals(root.getCanonicalFile())) throw new FileNotFoundException("Report not found");
        } catch(IOException error) { throw new FileNotFoundException(error.getMessage()); }
        return file;
    }
    public ParcelFileDescriptor openFile(Uri uri,String mode) throws FileNotFoundException {
        if(!"r".equals(mode)) throw new FileNotFoundException("Read-only report");
        return ParcelFileDescriptor.open(resolve(uri),ParcelFileDescriptor.MODE_READ_ONLY);
    }
    public String getType(Uri uri) { return "application/zip"; }
    public Cursor query(Uri uri,String[] projection,String selection,String[] selectionArgs,String sortOrder) {
        try {
            File file=resolve(uri); String[] columns=projection==null?new String[]{OpenableColumns.DISPLAY_NAME,OpenableColumns.SIZE}:projection;
            MatrixCursor cursor=new MatrixCursor(columns); Object[] values=new Object[columns.length];
            for(int i=0;i<columns.length;i++) values[i]=OpenableColumns.DISPLAY_NAME.equals(columns[i])?file.getName():OpenableColumns.SIZE.equals(columns[i])?file.length():null;
            cursor.addRow(values); return cursor;
        } catch(FileNotFoundException error) { return null; }
    }
    public Uri insert(Uri uri,ContentValues values) { throw new UnsupportedOperationException("Read-only"); }
    public int update(Uri uri,ContentValues values,String selection,String[] args) { throw new UnsupportedOperationException("Read-only"); }
    public int delete(Uri uri,String selection,String[] args) { throw new UnsupportedOperationException("Read-only"); }
}
