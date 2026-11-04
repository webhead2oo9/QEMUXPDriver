#include <stdio.h>
#include <string.h>

#include "qxpdisp/qxp_shader_owner.h"

static int require(int condition, const char *message)
{
    if (condition) return 1;
    fprintf(stderr, "shader owner test failed: %s\n", message);
    return 0;
}

int main(void)
{
    QXP_SHADER_OWNER owner;
    unsigned long first_device = 0;
    unsigned long second_device = 0;

    memset(&owner, 0xff, sizeof(owner));
    qxp_shader_owner_initialize(&owner, 7, 11, &first_device);
    if (!require(qxp_shader_owner_is_set(&owner),
                 "initialized owner must occupy a slot")) return 1;
    if (!require(qxp_shader_owner_matches(
                     &owner, 7, 11, &first_device),
                 "exact owner tuple")) return 1;
    if (!require(!qxp_shader_owner_matches(
                     &owner, 8, 11, &first_device),
                 "different context must not alias")) return 1;
    if (!require(!qxp_shader_owner_matches(
                     &owner, 7, 12, &first_device),
                 "reused context handle in another process must not alias")) {
        return 1;
    }
    if (!require(!qxp_shader_owner_matches(
                     &owner, 7, 11, &second_device),
                 "reused context handle on another device must not alias")) {
        return 1;
    }
    qxp_shader_owner_initialize(&owner, 0, 0, NULL);
    if (!require(!qxp_shader_owner_is_set(&owner),
                 "zero context releases the slot")) return 1;
    if (!require(!qxp_shader_owner_matches(&owner, 0, 0, NULL),
                 "empty owner must never match")) return 1;

    puts("shader owner tests passed");
    return 0;
}
