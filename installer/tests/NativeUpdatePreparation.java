package io.github.zerk235.snmp.installer;

import android.app.Instrumentation;
import android.content.Context;
import android.content.pm.PackageInfo;
import android.os.Bundle;
import java.io.*;
import java.nio.charset.StandardCharsets;
import org.json.JSONObject;

/** Test-build only. Prepares one base APK using the existing non-exportable key.
 * Never installs/stops/removes the game, changes worlds, or re-signs asset splits. */
public final class NativeUpdatePreparation extends Instrumentation {
    public void onCreate(Bundle arguments) { start(); }
    public void onStart() {
        Bundle result = new Bundle();
        try {
            Context context = getTargetContext();
            PatchEngine engine = new PatchEngine(context);
            PackageInfo game = engine.installed();
            if (game == null || !PatchEngine.GAME_VERSION.equals(game.versionName)
                    || !PatchEngine.hasAssets(game)) throw new IOException("Compatible installed game required");
            LocalSigner signer = new LocalSigner();
            if (!signer.fingerprint().equals(PatchEngine.certificate(game)))
                throw new IOException("Existing game key differs; refusing a signature change");
            JSONObject payload = engine.payload();
            byte[] mod = ApkPatcher.readSmall(context.getAssets().open("libsnmod.so"), 16 * 1024 * 1024);
            String modSha = ApkPatcher.hex(java.security.MessageDigest.getInstance("SHA-256").digest(mod));
            if (!modSha.equals(payload.getString("sha256"))) throw new IOException("Native payload hash mismatch");
            File source = new File(game.applicationInfo.sourceDir);
            String sourceSha = ApkPatcher.sha256(source);
            File dir = new File(context.getFilesDir(), "native-update-check");
            engine.removeOwnTree(dir);
            if (!dir.mkdir()) throw new IOException("Cannot create isolated preparation directory");
            File unsigned = new File(dir, "unsigned.apk"), signed = new File(dir, "base.apk");
            ApkPatcher.rewrite(source, unsigned, mod, (stage, done, total) -> {});
            signer.sign(unsigned, signed);
            signer.verify(signed);
            ApkIdentity identity = ApkIdentity.read(signed);
            if (!PatchEngine.GAME.equals(identity.packageName) || identity.split != null
                    || identity.versionCode != game.getLongVersionCode()) throw new IOException("Candidate identity differs");
            if (!sourceSha.equals(ApkPatcher.sha256(source))) throw new IOException("Installed source changed");
            JSONObject report = new JSONObject().put("build_id", payload.getString("buildId"))
                .put("library_sha256", modSha).put("certificate", signer.fingerprint())
                .put("source_sha256", sourceSha).put("candidate_sha256", ApkPatcher.sha256(signed))
                .put("installed", false).put("resources_modified", false).put("saves_modified", false);
            try (FileOutputStream stream = new FileOutputStream(new File(dir, "report.json"))) {
                stream.write(report.toString(2).getBytes(StandardCharsets.UTF_8)); stream.getFD().sync();
            }
            if (!unsigned.delete()) throw new IOException("Cannot remove temporary unsigned APK");
            result.putString("result", report.toString()); finish(-1, result);
        } catch (Throwable error) {
            result.putString("failure", android.util.Log.getStackTraceString(error)); finish(0, result);
        }
    }
}
