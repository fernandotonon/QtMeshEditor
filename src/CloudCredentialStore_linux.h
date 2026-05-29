#ifndef CLOUD_CREDENTIAL_STORE_LINUX_H
#define CLOUD_CREDENTIAL_STORE_LINUX_H

#ifdef __cplusplus
extern "C" {
#endif

/** @return 1 on success, 0 on failure. */
int qtmesh_cloud_secret_store(const char* payload);

/** @return newly allocated password string, or NULL. Free with qtmesh_cloud_secret_free. */
char* qtmesh_cloud_secret_load(void);

void qtmesh_cloud_secret_free(char* payload);

void qtmesh_cloud_secret_delete(void);

#ifdef __cplusplus
}
#endif

#endif // CLOUD_CREDENTIAL_STORE_LINUX_H
