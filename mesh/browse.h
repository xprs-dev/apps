/* mesh/browse.c: the stations as a list and what each offers. */
#ifndef MESH_BROWSE_H
#define MESH_BROWSE_H
void browse_init(void);
/* A core topic was published. */
void browse_changed(const char *topic);
void browse_tick(void);
/* 1 when the command was this file's. */
int browse_command(const char *cmd, const char *full);
#endif
