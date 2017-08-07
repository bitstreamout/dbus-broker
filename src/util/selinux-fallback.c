/*
 * Bus SELinux Fallback Helpers
 *
 * This fallback is used when libselinux is not available, and is meant to be
 * functionally equivalent to util/selinux.c in case SELinux is disabled, but
 * without requiring the library.
 *
 * See util/selinux.c for details.
 */

#include <c-macro.h>
#include <stdlib.h>
#include "util/selinux.h"

bool bus_selinux_is_enabled(void) {
        return false;
}

const char *bus_selinux_policy_root(void) {
        return NULL;
}

int bus_selinux_id_init(BusSELinuxID **id, const char *seclabel) {
        *id = NULL;
        return 0;
}

int bus_selinux_registry_new(BusSELinuxRegistry **registryp, BusSELinuxID *fallback_id) {
        *registryp = NULL;
        return 0;
}

BusSELinuxRegistry *bus_selinux_registry_ref(BusSELinuxRegistry *registry) {
        return NULL;
}

BusSELinuxRegistry *bus_selinux_registry_unref(BusSELinuxRegistry *registry) {
        return NULL;
}

int bus_selinux_registry_add_name(BusSELinuxRegistry *registry, const char *name, const char *context) {
        return 0;
}

int bus_selinux_check_own(BusSELinuxRegistry *registry,
                          BusSELinuxID *id_owner,
                          const char *name) {
        return 0;
}

int bus_selinux_check_send(BusSELinuxRegistry *registry,
                           BusSELinuxID *id_sender,
                           BusSELinuxID *id_receiver) {
        return 0;
}

int bus_selinux_init_global(void) {
        return 0;
}

void bus_selinux_deinit_global(void) {
        return;
}
