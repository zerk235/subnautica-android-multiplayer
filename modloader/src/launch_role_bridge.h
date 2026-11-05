#pragma once
#include <jni.h>
void launch_role_bridge_init(JavaVM* vm,JNIEnv* env);
void launch_role_bridge_tick(bool at_menu);
