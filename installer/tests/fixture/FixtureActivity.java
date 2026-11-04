package io.github.zerk235.snmp.fixture;

/** Explicit first launch: some OEMs block providers of a never-launched app. */
public final class FixtureActivity extends android.app.Activity {
    public void onCreate(android.os.Bundle state) { super.onCreate(state); finish(); }
}
