#include "common_priv.h"

gchar *get_filepath_by_name(const gchar *name)
{
    struct stat st;
    gchar *full_path = g_strconcat("/home/", g_getenv("USER"), "/.config/gwc/", name, NULL);
    if (stat(full_path, &st) == -1) {
        g_free(full_path);
        gchar *current_dir = g_get_current_dir();
        full_path = g_strconcat(current_dir, "/", name, NULL);
        g_free(current_dir);
        if (stat(full_path, &st) == -1) {
            g_free(full_path);
            full_path = g_strconcat("/etc/gwc/", name, NULL);
            if (stat(full_path, &st) == -1) {
                g_free(full_path);
                return NULL;
            }
        }
    }

    return full_path;
}