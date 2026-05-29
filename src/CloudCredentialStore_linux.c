#include "CloudCredentialStore_linux.h"

#include <libsecret/secret.h>

static const SecretSchema kCloudSessionSchema = {
    "org.qtmesheditor.cloud",
    SECRET_SCHEMA_NONE,
    {
        {"session", SECRET_SCHEMA_ATTRIBUTE_STRING},
        {"NULL", 0},
    },
};

int qtmesh_cloud_secret_store(const char* payload)
{
    GError* error = NULL;
    const gboolean ok = secret_password_store_sync(
        &kCloudSessionSchema, SECRET_COLLECTION_DEFAULT, "QtMesh Cloud session", payload, NULL, &error,
        "session", "default", NULL);
    if (error)
        g_error_free(error);
    return ok ? 1 : 0;
}

char* qtmesh_cloud_secret_load(void)
{
    GError* error = NULL;
    char* password = secret_password_lookup_sync(
        &kCloudSessionSchema, NULL, &error, "session", "default", NULL);
    if (error) {
        g_error_free(error);
        return NULL;
    }
    return password;
}

void qtmesh_cloud_secret_free(char* payload)
{
    secret_password_free(payload);
}

void qtmesh_cloud_secret_delete(void)
{
    GError* error = NULL;
    secret_password_clear_sync(&kCloudSessionSchema, NULL, &error, "session", "default", NULL);
    if (error)
        g_error_free(error);
}
