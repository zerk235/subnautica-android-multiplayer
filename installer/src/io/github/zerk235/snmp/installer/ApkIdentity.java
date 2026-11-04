package io.github.zerk235.snmp.installer;

import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.util.zip.*;

/** Minimal bounded reader for root attributes of Android's binary manifest. */
final class ApkIdentity {
    String packageName, versionName, split;
    long versionCode;

    static ApkIdentity read(File apk) throws IOException {
        try (ZipFile zip = new ZipFile(apk)) {
            ZipEntry manifest = zip.getEntry("AndroidManifest.xml");
            if (manifest == null) throw new IOException("В APK нет манифеста");
            return parse(ApkPatcher.readSmall(zip.getInputStream(manifest), 4*1024*1024));
        }
    }

    static ApkIdentity parse(byte[] data) throws IOException {
        try {
            ByteBuffer b=ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
            if(data.length<8 || u16(b,0)!=3 || u16(b,2)!=8 || b.getInt(4)!=data.length) throw bad();
            String[] strings=null;
            for(int at=8; at<data.length;) {
                if(at>data.length-8) throw bad();
                int type=u16(b,at), header=u16(b,at+2), size=b.getInt(at+4);
                if(header<8 || size<header || size>data.length-at) throw bad();
                if(type==1) {
                    if(strings!=null || header<28) throw bad();
                    int count=b.getInt(at+8), styles=b.getInt(at+12), flags=b.getInt(at+16), start=b.getInt(at+20);
                    if(count<0 || count>65536 || styles<0 || (long)header+4L*(count+styles)>size || start<header || start>size) throw bad();
                    strings=new String[count];
                    for(int i=0;i<count;i++) {
                        int offset=b.getInt(at+header+i*4);
                        if(offset<0 || offset>=size-start) throw bad();
                        int p=at+start+offset, end=at+size;
                        if((flags&256)!=0) {
                            int[] utf16=len8(data,p,end); int[] utf8=len8(data,utf16[1],end);
                            int length=utf8[0]; p=utf8[1];
                            if(length>end-p-1 || data[p+length]!=0) throw bad();
                            strings[i]=new String(data,p,length,StandardCharsets.UTF_8);
                        } else {
                            if(p>end-2) throw bad(); int length=u16(b,p); p+=2;
                            if((length&32768)!=0) { if(p>end-2) throw bad(); length=((length&32767)<<16)|u16(b,p); p+=2; }
                            if(length<0 || length>(end-p-2)/2 || u16(b,p+length*2)!=0) throw bad();
                            strings[i]=new String(data,p,length*2,StandardCharsets.UTF_16LE);
                        }
                    }
                } else if(type==0x102) {
                    if(strings==null || header!=16 || size<36 || !"manifest".equals(str(strings,b.getInt(at+20)))) throw bad();
                    int first=16+u16(b,at+24), stride=u16(b,at+26), count=u16(b,at+28);
                    if(first<36 || stride<20 || first+(long)stride*count>size) throw bad();
                    ApkIdentity result=new ApkIdentity(); boolean code=false;
                    java.util.HashSet<String> seen=new java.util.HashSet<>();
                    long major=0;
                    for(int i=0;i<count;i++) {
                        int attr=at+first+i*stride; int namespace=b.getInt(attr);
                        String ns=namespace==-1?"":str(strings,namespace), name=str(strings,b.getInt(attr+4));
                        if(!seen.add(ns+":"+name)) throw bad();
                        int kind=data[attr+15]&255, value=b.getInt(attr+16);
                        String text=kind==3?str(strings,value):null;
                        if(ns.isEmpty() && name.equals("package")) result.packageName=text;
                        if(ns.isEmpty() && name.equals("split")) result.split=text;
                        if(ns.equals("http://schemas.android.com/apk/res/android")) {
                            if(name.equals("versionName")) result.versionName=text;
                            if(name.equals("versionCode") || name.equals("versionCodeMajor")) {
                                if(kind!=16 && kind!=17) throw bad();
                                if(name.equals("versionCode")) { result.versionCode=Integer.toUnsignedLong(value); code=true; }
                                else major=Integer.toUnsignedLong(value);
                            }
                        }
                    }
                    if(result.packageName==null || result.packageName.isEmpty() || !code || major>Integer.MAX_VALUE) throw bad();
                    result.versionCode|=major<<32;
                    return result;
                }
                at+=size;
            }
            throw bad();
        } catch(IndexOutOfBoundsException | IllegalArgumentException e) { throw new IOException("Повреждён бинарный манифест APK",e); }
    }
    static int[] len8(byte[] bytes,int p,int end) throws IOException {
        if(p>=end) throw bad(); int n=bytes[p++]&255;
        if((n&128)!=0) { if(p>=end) throw bad(); n=((n&127)<<8)|(bytes[p++]&255); }
        return new int[]{n,p};
    }
    static int u16(ByteBuffer b,int at) { return b.getShort(at)&65535; }
    static String str(String[] strings,int i) throws IOException { if(i<0 || i>=strings.length) throw bad(); return strings[i]; }
    static IOException bad() { return new IOException("Неподдерживаемый бинарный манифест APK"); }
}
