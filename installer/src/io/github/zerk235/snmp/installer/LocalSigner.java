package io.github.zerk235.snmp.installer;

import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import com.android.apksig.ApkSigner;
import com.android.apksig.ApkVerifier;
import java.io.File;
import java.math.BigInteger;
import java.security.*;
import java.security.cert.X509Certificate;
import java.security.spec.RSAKeyGenParameterSpec;
import java.util.*;
import javax.security.auth.x500.X500Principal;

/** Each installation of the manager has a private, non-exportable Android Keystore key. */
final class LocalSigner {
    static final String ALIAS = "snmp-local-apk-signing-v1";
    final PrivateKey key;
    final X509Certificate certificate;

    LocalSigner() throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore"); store.load(null);
        if (!store.containsAlias(ALIAS)) {
            KeyPairGenerator generator = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_RSA, "AndroidKeyStore");
            generator.initialize(new KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_SIGN | KeyProperties.PURPOSE_VERIFY)
                .setAlgorithmParameterSpec(new RSAKeyGenParameterSpec(2048, RSAKeyGenParameterSpec.F4))
                .setDigests(KeyProperties.DIGEST_SHA256, KeyProperties.DIGEST_SHA512)
                .setSignaturePaddings(KeyProperties.SIGNATURE_PADDING_RSA_PKCS1)
                .setCertificateSubject(new X500Principal("CN=SN Multiplayer Local Install"))
                .setCertificateSerialNumber(new BigInteger(120, new SecureRandom()).add(BigInteger.ONE))
                .setCertificateNotBefore(new Date(1577836800000L))
                .setCertificateNotAfter(new Date(4102444800000L)).build());
            generator.generateKeyPair();
        }
        key = (PrivateKey)store.getKey(ALIAS, null);
        certificate = (X509Certificate)store.getCertificate(ALIAS);
        if (key == null || certificate == null) throw new GeneralSecurityException("Ключ подписи недоступен");
    }

    String fingerprint() throws Exception {
        return ApkPatcher.hex(MessageDigest.getInstance("SHA-256").digest(certificate.getEncoded()));
    }

    void sign(File unsigned, File output) throws Exception {
        if (output.exists() || unsigned.getCanonicalFile().equals(output.getCanonicalFile()))
            throw new java.io.IOException("Подписанный APK должен быть новым файлом");
        boolean okay = false;
        try {
            ApkSigner.SignerConfig config = new ApkSigner.SignerConfig.Builder("snmp", key,
                Collections.singletonList(certificate)).build();
            new ApkSigner.Builder(Collections.singletonList(config)).setInputApk(unsigned).setOutputApk(output)
                .setMinSdkVersion(29).setV1SigningEnabled(false).setV2SigningEnabled(true)
                .setV3SigningEnabled(true).setV4SigningEnabled(false)
                .setOtherSignersSignaturesPreserved(false).setAlignmentPreserved(true)
                .setLibraryPageAlignmentBytes(16384).build().sign();
            verify(output); okay = true;
        } finally {
            if (!okay && output.exists()) output.delete();
        }
    }

    void verify(File file) throws Exception {
        ApkVerifier.Result result = new ApkVerifier.Builder(file).setMinCheckedPlatformVersion(29).build().verify();
        if (!result.isVerified() || !(result.isVerifiedUsingV2Scheme() || result.isVerifiedUsingV3Scheme())
                || result.getSignerCertificates().size() != 1
                || !Arrays.equals(certificate.getEncoded(), result.getSignerCertificates().get(0).getEncoded()))
            throw new GeneralSecurityException("Подпись APK не прошла проверку: verified=" + result.isVerified()
                + ", v2=" + result.isVerifiedUsingV2Scheme() + ", v3=" + result.isVerifiedUsingV3Scheme()
                + ", signers=" + result.getSignerCertificates().size() + ", errors=" + result.getAllErrors());
    }
}
