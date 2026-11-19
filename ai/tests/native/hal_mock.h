#ifndef AI_HAL_MOCK_H
#define AI_HAL_MOCK_H

#define MOCK_OUT_MAX 256
extern char *mock_out[MOCK_OUT_MAX];
extern int mock_out_n;
void mock_send_to_wapp(const char *json);
void mock_out_clear(void);

extern char mock_url[512], mock_headers[2048];
extern char *mock_body;
extern int mock_status;
extern const char *mock_reply;

#endif
