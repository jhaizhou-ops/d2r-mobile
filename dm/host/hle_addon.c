#include "hle.h"
#include "hle_addon.h"
#include <sys/stat.h>

enum { MAX_ADDON = 64 };
static struct { int index; char directory[1024]; } addons[MAX_ADDON];
static int addon_count;
static pthread_mutex_t addon_lock = PTHREAD_MUTEX_INITIALIZER;

int dm_addon_register(int index, const char *directory) {
    struct stat st;
    if (index <= 0 || !directory || directory[0] != '/' ||
        strlen(directory) >= sizeof addons[0].directory ||
        stat(directory, &st) || !S_ISDIR(st.st_mode)) return 0;
    pthread_mutex_lock(&addon_lock);
    int position = 0;
    while (position < addon_count && addons[position].index < index) position++;
    int valid = addon_count < MAX_ADDON &&
        (position == addon_count || addons[position].index != index);
    if (valid) {
        memmove(&addons[position + 1], &addons[position],
                (addon_count - position) * sizeof addons[0]);
        addons[position].index = index;
        strlcpy(addons[position].directory, directory, sizeof addons[0].directory);
        addon_count++;
    }
    pthread_mutex_unlock(&addon_lock);
    return valid;
}

int dm_addon_count(void) {
    pthread_mutex_lock(&addon_lock);
    int count = addon_count;
    pthread_mutex_unlock(&addon_lock);
    return count;
}

int dm_addon_list(int *out, int offset, int capacity) {
    if (!out || offset < 0 || capacity <= 0) return 0;
    pthread_mutex_lock(&addon_lock);
    int count = 0;
    for (int i = offset; i < addon_count && count < capacity; i++)
        out[count++] = addons[i].index;
    pthread_mutex_unlock(&addon_lock);
    return count;
}

int dm_addon_path(int index, char *out, size_t capacity) {
    if (!out || !capacity) return 0;
    pthread_mutex_lock(&addon_lock);
    int found = 0;
    for (int i = 0; i < addon_count; i++) if (addons[i].index == index) {
        if (strlen(addons[i].directory) < capacity) {
            strlcpy(out, addons[i].directory, capacity);
            found = 1;
        }
        break;
    }
    pthread_mutex_unlock(&addon_lock);
    return found;
}

