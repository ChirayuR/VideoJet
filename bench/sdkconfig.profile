# Overlay for profile builds only. Use it with -DSDKCONFIG_DEFAULTS="...sdkconfig.defaults;...sdkconfig.profile"
# and -DSDKCONFIG=<build-prof>/sdkconfig, so the timed bench/sdkconfig never sees it.
CONFIG_VJ_PROFILE=y
