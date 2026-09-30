#include "todo_model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static todo_snapshot_t snapshot;
static bool parse(const char *s) { return todo_parse(s, strlen(s), &snapshot); }
#define PREFIX "{\"schema_version\":1,\"device_id\":\"tablet-001\",\"list_id\":\"inbox\",\"revision\":7,\"items\":"
int main(void) {
    assert(parse(PREFIX "[{\"title\":\"Buy milk\",\"completed\":true}]}"));
    assert(snapshot.revision == 7 && snapshot.count == 1);
    assert(snapshot.items[0].completed && !strcmp(snapshot.items[0].title, "Buy milk"));
    assert(parse(PREFIX "[]}"));
    assert(snapshot.count == 0);
    assert(!parse(PREFIX "[{\"title\":\"\",\"completed\":false}]}"));
    assert(!parse(PREFIX "[{\"title\":\"task\",\"completed\":1}]}"));
    assert(!parse(PREFIX "[{\"title\":\"bad\\ntext\",\"completed\":false}]}"));
    assert(!parse(PREFIX "[]} trailing"));
    assert(!parse("{}"));
    assert(!parse("{broken"));
    puts("Task snapshot validation passed");
}
