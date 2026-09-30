#include "todo_model.h"
#include "cJSON.h"
#include <string.h>
#include <limits.h>

static bool matches(cJSON *root, const char *key, const char *expected) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(v) && !strcmp(v->valuestring, expected);
}

bool todo_parse(const char *data, size_t length, todo_snapshot_t *out) {
    if (!data || !out || !length || length > TODO_MAX_MESSAGE) return false;
    // Caller supplies a terminating NUL; embedded NUL must not hide extra JSON.
    if (strlen(data) != length) return false;
    cJSON *root = cJSON_ParseWithLengthOpts(data, length + 1, NULL, true);
    if (!root) return false;
    bool valid = false;
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    cJSON *revision = cJSON_GetObjectItemCaseSensitive(root, "revision");
    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (!cJSON_IsNumber(version) || version->valuedouble != 1 ||
        !matches(root, "device_id", "tablet-001") || !matches(root, "list_id", "inbox") ||
        !cJSON_IsNumber(revision) || revision->valuedouble < 0 || revision->valuedouble > INT_MAX ||
        revision->valuedouble != revision->valueint || !cJSON_IsArray(items)) goto done;
    int count = cJSON_GetArraySize(items);
    if (count > TODO_MAX_ITEMS) goto done;
    out->count = 0;
    for (int i = 0; i < count; ++i) {
        cJSON *item = cJSON_GetArrayItem(items, i);
        cJSON *title = cJSON_GetObjectItemCaseSensitive(item, "title");
        cJSON *completed = cJSON_GetObjectItemCaseSensitive(item, "completed");
        if (!cJSON_IsString(title) || !cJSON_IsBool(completed)) goto done;
        size_t n = strlen(title->valuestring);
        if (!n || n > TODO_MAX_TITLE) goto done;
        for (size_t j = 0; j < n; ++j)
            if ((unsigned char)title->valuestring[j] < 32 || title->valuestring[j] == 127) goto done;
        memcpy(out->items[i].title, title->valuestring, n + 1);
        out->items[i].completed = cJSON_IsTrue(completed);
        out->count++;
    }
    out->revision = revision->valueint;
    valid = true;
done:
    cJSON_Delete(root);
    return valid;
}
