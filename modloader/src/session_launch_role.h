#pragma once
enum SessionLaunchRole { kLaunchAuto=0,kLaunchHost=1,kLaunchGuest=2 };
inline bool session_launch_role_valid(SessionLaunchRole role) {
    return role==kLaunchAuto || role==kLaunchHost || role==kLaunchGuest;
}
